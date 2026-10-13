/* Commands for token transfers, balances and transaction history. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "cli_internal.h"

/* Token and ledger operations */

/* Returns 1 if the session runs the model the command belongs to. */
static int needs_model(const Library *lib, LedgerModel model, const char *command,
                       const char *other_command)
{
    if (lib->ledger.model == model) {
        return 1;
    }
    printf("ERROR: %s belongs to the %s model, but this session uses the %s model.\n", command,
           ledger_model_name(model), ledger_model_name(lib->ledger.model));
    printf("Use %s, or restart with --model %s.\n", other_command,
           model == MODEL_UTXO ? "utxo" : "account");
    return 0;
}

static CmdStatus transfer(Library *lib, Args *args, int nonce_argument)
{
    const char *sender = cli_arg(args, 0, "From account: ");
    const char *recipient = sender != NULL ? cli_arg(args, 1, "To account  : ") : NULL;
    const char *amount_text = recipient != NULL
                              ? cli_arg(args, 2, "Amount (coins, fee is 1 extra): ") : NULL;
    RequestOptions options;
    long amount;

    if (amount_text == NULL || !cli_request_options(lib, args, &options)) {
        return CMD_CONTINUE;
    }
    if (!cli_parse_coins(amount_text, &amount)) {
        printf("ERROR: Enter a positive amount such as 3 or 2.50.\n");
        return CMD_CONTINUE;
    }
    /* account_transfer also takes the nonce as a fourth argument. */
    if (nonce_argument && args->argc > 3) {
        char *end;
        options.nonce = strtoul(args->argv[3], &end, 10);
        if (!isdigit((unsigned char)args->argv[3][0]) || *end != '\0') {
            printf("ERROR: The nonce must be a whole number.\n");
            return CMD_CONTINUE;
        }
        options.has_nonce = 1;
    }

    lending_transfer(lib, sender, recipient, amount, &options);
    return CMD_CONTINUE;
}

CmdStatus cmd_token_transfer(Library *lib, Args *args)
{
    if (needs_model(lib, MODEL_UTXO, "token_transfer", "account_transfer")) {
        return transfer(lib, args, 0);
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_account_transfer(Library *lib, Args *args)
{
    if (needs_model(lib, MODEL_ACCOUNT, "account_transfer", "token_transfer")) {
        return transfer(lib, args, 1);
    }
    return CMD_CONTINUE;
}

static void print_balance(const Library *lib, const char *id)
{
    char amount[32];

    if (ledger_find_account(&lib->ledger, id) == NULL) {
        printf("ERROR: No account '%s'.\n", id);
        return;
    }
    printf("%s: %s coins", id, format_coins(ledger_balance(&lib->ledger, id), amount));
    if (lib->ledger.model == MODEL_UTXO) {
        int outputs = 0;
        for (int i = 0; i < lib->ledger.utxo_count; i++) {
            if (strcmp(lib->ledger.utxos[i].owner, id) == 0) outputs++;
        }
        printf(" = the sum of %d unspent output%s", outputs, outputs == 1 ? "" : "s");
    } else {
        printf(", nonce %lu", ledger_nonce(&lib->ledger, id));
    }
    printf("\n");
}

CmdStatus cmd_token_balance(Library *lib, Args *args)
{
    if (!needs_model(lib, MODEL_UTXO, "token_balance", "account_balance")) {
        return CMD_CONTINUE;
    }
    if (args->argc > 0) {
        print_balance(lib, args->argv[0]);
    } else {
        ledger_print_balances(&lib->ledger);
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_utxo_view(Library *lib, Args *args)
{
    (void)args;
    if (needs_model(lib, MODEL_UTXO, "utxo_view", "account_balance")) {
        ledger_print_utxo_set(&lib->ledger);
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_account_balance(Library *lib, Args *args)
{
    if (!needs_model(lib, MODEL_ACCOUNT, "account_balance", "token_balance")) {
        return CMD_CONTINUE;
    }
    if (args->argc > 0) {
        print_balance(lib, args->argv[0]);
    } else {
        ledger_print_balances(&lib->ledger);
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_account_nonce(Library *lib, Args *args)
{
    const char *id;

    if (!needs_model(lib, MODEL_ACCOUNT, "account_nonce", "member_nonce") ||
        (id = cli_arg(args, 0, "Account ID: ")) == NULL) {
        return CMD_CONTINUE;
    }
    if (ledger_find_account(&lib->ledger, id) == NULL) {
        printf("ERROR: No account '%s'.\n", id);
        return CMD_CONTINUE;
    }
    cli_print_nonce(lib, id);
    return CMD_CONTINUE;
}

CmdStatus cmd_transaction_history(Library *lib, Args *args)
{
    const char *id = cli_arg(args, 0, "Account ID: ");

    if (id != NULL && !ledger_print_history(&lib->ledger, id)) {
        printf("ERROR: No account '%s'.\n", id);
    }
    return CMD_CONTINUE;
}
