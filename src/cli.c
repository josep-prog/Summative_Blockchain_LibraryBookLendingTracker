#define _POSIX_C_SOURCE 200809L   /* termios, isatty */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#include "cli_internal.h"

/* Input */

int cli_prompt_line(const char *prompt, char *buf, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);

    if (fgets(buf, (int)size, stdin) == NULL) {
        return 0;
    }

    size_t len = strcspn(buf, "\r\n");
    if (buf[len] == '\0' && len == size - 1) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF);
    }
    buf[len] = '\0';

    /* Show piped input, so a scripted run reads like a typed one. */
    if (!isatty(STDIN_FILENO)) {
        printf("%s\n", buf);
    }
    return 1;
}

int cli_prompt_secret(const char *prompt, char *buf, size_t size)
{
    struct termios old_attr, new_attr;
    int hide = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_attr) == 0;

    if (!hide) {
        /* Piped input: read it, but never echo a secret. */
        printf("%s\n", prompt);
        if (fgets(buf, (int)size, stdin) == NULL) {
            return 0;
        }
        buf[strcspn(buf, "\r\n")] = '\0';
        return 1;
    }

    new_attr = old_attr;
    new_attr.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_attr);

    int ok = cli_prompt_line(prompt, buf, size);

    tcsetattr(STDIN_FILENO, TCSANOW, &old_attr);
    printf("\n");
    return ok;
}

int cli_parse_int(const char *text, int min, int max, int *out)
{
    char *end;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < min || value > max) {
        return 0;
    }
    *out = (int)value;
    return 1;
}

/* "12" or "12.50" coins -> hundredths; negative or zero amounts are refused. */
int cli_parse_coins(const char *text, long *out)
{
    long whole = 0, cents = 0;
    int digits = 0, decimals = 0;
    const char *p = text;

    for (; *p >= '0' && *p <= '9'; p++, digits++) {
        if (whole > 1000000) return 0;
        whole = whole * 10 + (*p - '0');
    }
    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9' && decimals < 2; p++, decimals++) {
            cents = cents * 10 + (*p - '0');
        }
        if (decimals == 1) cents *= 10;
    }
    if (*p != '\0' || (digits == 0 && decimals == 0)) {
        return 0;
    }
    *out = whole * COIN + cents;
    return *out > 0;
}

/* "YYYY-MM-DD" -> the end of that day; rejects dates that do not exist. */
static int parse_date(const char *text, time_t *out)
{
    int year, month, day;

    if (strlen(text) != 10 || text[4] != '-' || text[7] != '-') {
        return 0;
    }
    for (int i = 0; i < 10; i++) {
        if (i != 4 && i != 7 && !isdigit((unsigned char)text[i])) {
            return 0;
        }
    }
    sscanf(text, "%d-%d-%d", &year, &month, &day);

    struct tm when;
    memset(&when, 0, sizeof(when));
    when.tm_year = year - 1900;
    when.tm_mon = month - 1;
    when.tm_mday = day;
    when.tm_hour = 23;
    when.tm_min = 59;
    when.tm_sec = 59;
    when.tm_isdst = -1;

    time_t result = mktime(&when);
    /* mktime turns 2026-02-30 into March: that is not a real date. */
    if (result == (time_t)-1 || when.tm_year != year - 1900 || when.tm_mon != month - 1 ||
        when.tm_mday != day) {
        return 0;
    }
    *out = result;
    return 1;
}

/* Commands and their arguments */

/* Splits the line in place. Double quotes keep spaces inside one argument. */
static char *parse_command_line(char *line, Args *args)
{
    char *command = NULL;
    char *p = line;

    memset(args, 0, sizeof(*args));
    while (*p != '\0') {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;

        char *token;
        int quoted = (*p == '"');
        if (quoted) {
            token = ++p;
            while (*p != '\0' && *p != '"') p++;
        } else {
            token = p;
            while (*p != '\0' && !isspace((unsigned char)*p)) p++;
        }
        if (*p != '\0') *p++ = '\0';

        if (command == NULL) {
            command = token;
        } else if (!quoted && strncmp(token, "id=", 3) == 0) {
            args->opt_id = token + 3;
        } else if (!quoted && strncmp(token, "nonce=", 6) == 0) {
            args->opt_nonce = token + 6;
        } else if (!quoted && strncmp(token, "due=", 4) == 0) {
            args->opt_due = token + 4;
        } else if (!quoted && strncmp(token, "reason=", 7) == 0) {
            args->opt_reason = token + 7;
        } else if (args->argc < MAX_ARGS) {
            args->argv[args->argc++] = token;
        }
    }
    return command;
}

/* Positional argument i, asked for if it was not typed. NULL if there is no value. */
const char *cli_arg(Args *args, int i, const char *prompt)
{
    if (i < args->argc) {
        return args->argv[i];
    }
    if (i >= MAX_ARGS || args->closed) {
        return NULL;
    }

    char *answer = args->answers[i];
    if (!cli_prompt_line(prompt, answer, ANSWER_SIZE)) {
        args->closed = 1;
        return NULL;
    }
    while (isspace((unsigned char)*answer)) answer++;
    char *end = answer + strlen(answer);
    while (end > answer && isspace((unsigned char)end[-1])) *--end = '\0';

    if (*answer == '\0') {
        printf("ERROR: a value is required.\n");
        return NULL;
    }
    return answer;
}

/* Turns the key=value options into request options, validating each. */
int cli_request_options(const Library *lib, const Args *args, RequestOptions *options)
{
    memset(options, 0, sizeof(*options));

    if (args->opt_id != NULL) {
        size_t len = strlen(args->opt_id);
        int ok = len >= 3 && len < RECORD_ID_SIZE;
        for (const char *p = args->opt_id; ok && *p != '\0'; p++) {
            ok = isupper((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '-';
        }
        if (!ok) {
            printf("ERROR: a request ID is 3-%d characters of A-Z, 0-9 and '-'.\n",
                   RECORD_ID_SIZE - 1);
            return 0;
        }
        options->request_id = args->opt_id;
    }

    if (args->opt_nonce != NULL) {
        char *end;
        options->nonce = strtoul(args->opt_nonce, &end, 10);
        if (!isdigit((unsigned char)args->opt_nonce[0]) || *end != '\0') {
            printf("ERROR: The nonce must be a whole number.\n");
            return 0;
        }
        options->has_nonce = 1;
    }

    if (args->opt_due != NULL) {
        if (!parse_date(args->opt_due, &options->due_date)) {
            printf("ERROR: '%s' is not a valid date. Use YYYY-MM-DD.\n", args->opt_due);
            return 0;
        }
        if (options->due_date <= library_now(lib)) {
            printf("ERROR: The due date %s is in the past.\n", args->opt_due);
            return 0;
        }
    }

    options->reason = args->opt_reason;
    return 1;
}

/* Format check first, existence second, so the user knows which one failed. */
int cli_member_id_ok(const char *id)
{
    if (!id_has_format(id, MEMBER_ID_PREFIX)) {
        printf("ERROR: '%s' is not a valid member ID (%s followed by 3-6 digits).\n", id,
               MEMBER_ID_PREFIX);
        return 0;
    }
    return 1;
}

int cli_book_id_ok(const char *id)
{
    if (!id_has_format(id, BOOK_ID_PREFIX)) {
        printf("ERROR: '%s' is not a valid book ID (%s followed by 3-6 digits).\n", id,
               BOOK_ID_PREFIX);
        return 0;
    }
    return 1;
}

int cli_copy_id_ok(const char *id)
{
    if (!copy_id_has_format(id)) {
        printf("ERROR: '%s' is not a valid copy ID (for example BK001-C1).\n", id);
        return 0;
    }
    return 1;
}

const char *cli_member_name(const Library *lib, const char *member_id)
{
    int index = find_member(&lib->registry, member_id);
    return index != -1 ? lib->registry.members[index].full_name : "?";
}

static CmdStatus cmd_help(Library *lib, Args *args);

static CmdStatus cmd_exit(Library *lib, Args *args)
{
    (void)lib;
    (void)args;
    return CMD_QUIT;
}

/* The command table; a NULL run marks a group heading. */
static const Command COMMANDS[] = {
    { "Membership management", NULL, NULL, NULL, 0 },
    { "register_member", "<member_id> <full_name> <course_code>",
      "add a member and generate their ECDSA key pair", cmd_register_member, CMD_WRITES | CMD_SAVES },
    { "view_member", "[member_id]", "one member in detail, or all members", cmd_view_member, 0 },
    { "member_nonce", "<member_id>", "the member's replay-protection nonce", cmd_member_nonce, 0 },

    { "Book and copy management", NULL, NULL, NULL, 0 },
    { "register_book", "<book_id> <title> <author>", "add a title", cmd_register_book,
      CMD_WRITES | CMD_SAVES },
    { "view_book", "[book_id]", "one title with its copies and queue, or all titles",
      cmd_view_book, 0 },
    { "add_copy", "<book_id> [NEW|GOOD|WORN]", "add a physical copy of a title", cmd_add_copy,
      CMD_WRITES | CMD_SAVES },
    { "view_copy", "[copy_id]", "one copy in detail, or the whole inventory", cmd_view_copy, 0 },
    { "mark_lost", "<copy_id> <reason_code>", "librarian override: mark a copy LOST",
      cmd_mark_lost, CMD_WRITES | CMD_SAVES },

    { "Lending operations", NULL, NULL, NULL, 0 },
    { "borrow_book", "<book_id> <member_id> [due=YYYY-MM-DD]", "request a loan of a free copy",
      cmd_borrow_book, CMD_WRITES | CMD_SAVES },
    { "return_book", "<copy_id> [NEW|GOOD|WORN] [reason=CODE]",
      "return a copy; a late return is fined", cmd_return_book, CMD_WRITES | CMD_SAVES },
    { "renew_book", "<copy_id>", "extend the due date of a loan", cmd_renew_book,
      CMD_WRITES | CMD_SAVES },
    { "reserve_book", "<book_id> <member_id>", "join the queue when no copy is free",
      cmd_reserve_book, CMD_WRITES | CMD_SAVES },
    { "reservation_status", "[book_id]", "queues and copies on hold", cmd_reservation_status, 0 },

    { "Fines and overdue", NULL, NULL, NULL, 0 },
    { "check_overdue", "", "loans past their due date and the fine so far", cmd_check_overdue, 0 },
    { "view_fines", "[member_id]", "fines accruing, owed and paid", cmd_view_fines, 0 },
    { "settle_fine", "<member_id> [amount]", "pay a fine in tokens", cmd_settle_fine,
      CMD_WRITES | CMD_SAVES },

    { "Token and ledger operations", NULL, NULL, NULL, 0 },
    { "token_transfer", "<from> <to> <amount>", "UTXO model: send tokens", cmd_token_transfer,
      CMD_WRITES | CMD_SAVES },
    { "token_balance", "[account]", "UTXO model: balances", cmd_token_balance, 0 },
    { "utxo_view", "", "UTXO model: every unspent output and its owner", cmd_utxo_view, 0 },
    { "account_balance", "[account]", "account model: balances", cmd_account_balance, 0 },
    { "account_transfer", "<from> <to> <amount> [nonce]", "account model: send tokens",
      cmd_account_transfer, CMD_WRITES | CMD_SAVES },
    { "account_nonce", "<account>", "account model: the account's nonce", cmd_account_nonce, 0 },
    { "transaction_history", "<member_id>", "the account's token transactions (linked list)",
      cmd_transaction_history, 0 },

    { "Blockchain and mining operations", NULL, NULL, NULL, 0 },
    { "pool_view", "", "pending and suspicious requests, in priority order", cmd_pool_view, 0 },
    { "mine_solo", "", "mine the pool alone", cmd_mine_solo, CMD_WRITES },
    { "mine_pool", "[miners]", "mine with 2-8 simulated miners sharing the reward",
      cmd_mine_pool, CMD_WRITES },
    { "mine_cloud", "<rounds>", "rent mining power for 1-5 rounds", cmd_mine_cloud, CMD_WRITES },
    { "chain_view", "[block_index]", "the chain, or one block with its Merkle tree",
      cmd_chain_view, 0 },
    { "validate_chain", "", "re-validate the whole chain", cmd_validate_chain, 0 },
    { "chain_save", "", "write the full state to disk now", cmd_chain_save, CMD_WRITES },
    { "chain_load", "", "reload the state from disk and re-validate it", cmd_chain_load, 0 },
    { "difficulty_status", "", "current difficulty and block reward", cmd_difficulty_status, 0 },
    { "set_difficulty", "<1-4>", "change the mining difficulty", cmd_set_difficulty,
      CMD_WRITES | CMD_SAVES },
    { "benchmark", "", "compare mining effort at difficulty 1 to 4", cmd_benchmark, 0 },

    { "Fraud and audit operations", NULL, NULL, NULL, 0 },
    { "fraud_review", "", "requests held as SUSPICIOUS and why", cmd_fraud_review, 0 },
    { "approve_suspicious", "<id>", "let a held request be mined", cmd_approve_suspicious,
      CMD_WRITES | CMD_SAVES },
    { "reject_suspicious", "<id>", "discard a held request", cmd_reject_suspicious,
      CMD_WRITES | CMD_SAVES },
    { "lending_history", "", "every confirmed record", cmd_lending_history, 0 },
    { "member_history", "<member_id>", "confirmed records of one member", cmd_member_history, 0 },
    { "book_history", "<book_id>", "confirmed records of one title", cmd_book_history, 0 },

    { "Simulation (ADMIN)", NULL, NULL, NULL, 0 },
    { "advance_time", "<days>", "move the library clock forward to test due dates",
      cmd_advance_time, CMD_WRITES | CMD_SAVES | CMD_ADMIN },
    { "tamper_demo", "[record|hash|signature]", "tamper with the chain in memory and validate",
      cmd_tamper_demo, CMD_WRITES | CMD_ADMIN },

    { "General", NULL, NULL, NULL, 0 },
    { "help", "", "this list", cmd_help, 0 },
    { "exit", "", "save and leave", cmd_exit, 0 }
};

#define COMMAND_COUNT ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

/* Menu number of COMMANDS[index]: 1, 2, 3... in table order, and 0 for exit. */
static int command_number(int index)
{
    int number = 0;

    if (index == COMMAND_COUNT - 1) {
        return 0;
    }
    for (int i = 0; i <= index; i++) {
        if (COMMANDS[i].run != NULL) number++;
    }
    return number;
}

static CmdStatus cmd_help(Library *lib, Args *args)
{
    (void)lib;
    (void)args;
    for (int i = 0; i < COMMAND_COUNT; i++) {
        if (COMMANDS[i].run == NULL) {
            printf("\n %s\n", COMMANDS[i].name);
        } else {
            char left[80];
            snprintf(left, sizeof(left), "%s %s", COMMANDS[i].name, COMMANDS[i].usage);
            printf("  %2d. %-52s %s\n", command_number(i), left, COMMANDS[i].help);
        }
    }
    printf("\n Type a command's number, its name, or the start of its name (utxo = utxo_view).\n");
    printf(" A missing argument is asked for. Put names with spaces in \"double quotes\".\n");
    printf(" Any request also accepts nonce=<n> and id=<request_id>, to test replay and "
           "duplicate detection.\n");
    return CMD_CONTINUE;
}

/* Edits (insert, delete, replace) needed to turn a into b: finds the command a typo meant. */
static int edit_distance(const char *a, const char *b)
{
    int row[64];
    int width = (int)strlen(b);

    if (width >= (int)(sizeof(row) / sizeof(row[0]))) {
        return width;
    }
    for (int j = 0; j <= width; j++) row[j] = j;

    for (int i = 1; a[i - 1] != '\0'; i++) {
        int diagonal = row[0];
        row[0] = i;
        for (int j = 1; j <= width; j++) {
            int best = diagonal + (a[i - 1] != b[j - 1]);
            if (row[j] + 1 < best) best = row[j] + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            diagonal = row[j];
            row[j] = best;
        }
    }
    return row[width];
}

/*
 * Finds a command by its full name, its menu number, or the start of its name
 * when only one command starts that way. Prints why when there is no match.
 */
static const Command *find_command(const char *name)
{
    const Command *prefixed = NULL, *closest = NULL;
    size_t len = strlen(name);
    /* A typo is at most 3 edits, and must leave some of the name intact. */
    int number, prefix_matches = 0, closest_distance = len < 4 ? (int)len : 4;
    int by_number = cli_parse_int(name, 0, COMMAND_COUNT, &number);

    for (int i = 0; i < COMMAND_COUNT; i++) {
        const Command *command = &COMMANDS[i];
        if (command->run == NULL) {
            continue;
        }
        if (strcmp(command->name, name) == 0 || (by_number && command_number(i) == number)) {
            return command;
        }
        if (strncmp(command->name, name, len) == 0) {
            prefixed = command;
            prefix_matches++;
        }
        int distance = edit_distance(name, command->name);
        if (distance < closest_distance) {
            closest = command;
            closest_distance = distance;
        }
    }

    if (prefix_matches == 1) {
        return prefixed;
    }
    if (prefix_matches > 1) {
        printf("'%s' could be:", name);
        for (int i = 0; i < COMMAND_COUNT; i++) {
            if (COMMANDS[i].run != NULL && strncmp(COMMANDS[i].name, name, len) == 0) {
                printf(" %s", COMMANDS[i].name);
            }
        }
        printf(". Type a little more of the name.\n");
    } else if (closest != NULL && !by_number) {
        printf("Unknown command '%s'. Did you mean %s?\n", name, closest->name);
    } else {
        printf("Unknown command '%s'. Type help for the numbered list.\n", name);
    }
    return NULL;
}

void cli_run(Library *lib)
{
    char line[512], prompt[128];
    Args args;

    printf("\nType help for the numbered list of commands.\n");

    for (;;) {
        snprintf(prompt, sizeof(prompt), "\n[%s | difficulty %d | pool %d | chain %d%s] library> ",
                 ledger_model_name(lib->ledger.model), lib->difficulty, lib->pool.count,
                 lib->chain.count, lib->locked ? " | LOCKED" : "");
        if (!cli_prompt_line(prompt, line, sizeof(line))) {
            printf("\nInput closed.\n");
            break;
        }

        const char *name = parse_command_line(line, &args);
        if (name == NULL) {
            continue;
        }
        const Command *command = find_command(name);
        if (command == NULL) {
            continue;
        }
        if ((command->flags & CMD_ADMIN) && !library_is_admin(lib)) {
            printf("ERROR: Only an ADMIN can run %s.\n", command->name);
            continue;
        }
        /* Nothing may change until the loaded chain has passed validation. */
        if ((command->flags & CMD_WRITES) && lib->locked) {
            printf("ERROR: The session is locked because the chain or its file seal is invalid. "
                   "Run validate_chain for details, or chain_load after restoring the file.\n");
            continue;
        }

        /* Overdue detection runs on every CLI invocation. */
        if (!lib->locked && lending_scan_overdue(lib) > 0) {
            library_save(lib);
        }

        CmdStatus status = command->run(lib, &args);

        if ((command->flags & CMD_SAVES) && !lib->locked) {
            library_save(lib);
        }
        if (status == CMD_QUIT || args.closed) {
            break;
        }
    }

    if (library_save(lib)) {
        printf("State saved to %s. Goodbye.\n", lib->data_file);
    } else {
        printf("Goodbye.\n");
    }
}
