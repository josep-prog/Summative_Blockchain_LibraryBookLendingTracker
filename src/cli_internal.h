#ifndef CLI_INTERNAL_H
#define CLI_INTERNAL_H

/*
 * Shared by the cli_*.c files only: the parsed command line, the command table
 * entry, the argument helpers in cli.c, and one handler per command.
 */

#include "cli.h"
#include "mining.h"

#define MAX_ARGS     6
#define ANSWER_SIZE  128

/*
 * A command line: positional arguments, plus options written as key=value
 * (id=, nonce=, due=, reason=). A missing positional argument is asked for.
 */
typedef struct {
    int argc;
    char *argv[MAX_ARGS];
    const char *opt_id;
    const char *opt_nonce;
    const char *opt_due;
    const char *opt_reason;
    char answers[MAX_ARGS][ANSWER_SIZE];
    int closed;                      /* input ended while asking */
} Args;

typedef enum { CMD_CONTINUE, CMD_QUIT } CmdStatus;

/* Each flag is one bit, so a command can combine several with '|'. */
#define CMD_WRITES 1u    /* changes state: refused while the session is locked */
#define CMD_SAVES  2u    /* state is saved to disk afterwards */
#define CMD_ADMIN  4u    /* ADMIN librarians only */

typedef struct {
    const char *name;
    const char *usage;
    const char *help;
    CmdStatus (*run)(Library *lib, Args *args);
    unsigned flags;
} Command;

/* Argument helpers (cli.c). */

/* "12" or "12.50" coins -> hundredths; negative or zero amounts are refused. */
int cli_parse_coins(const char *text, long *out);
/* Positional argument i, asked for if it was not typed. NULL if there is no value. */
const char *cli_arg(Args *args, int i, const char *prompt);
/* Turns the key=value options into request options, validating each. */
int cli_request_options(const Library *lib, const Args *args, RequestOptions *options);
/* Format checks; each prints its own error. Whether the ID exists is checked by the command. */
int cli_member_id_ok(const char *id);
int cli_book_id_ok(const char *id);
int cli_copy_id_ok(const char *id);
/* The member's full name, or "?" if the ID is not registered. */
const char *cli_member_name(const Library *lib, const char *member_id);

/* Printing helpers used by more than one file. */
void cli_print_nonce(const Library *lib, const char *id);          /* cli_catalog.c */
void cli_print_queue(const Library *lib, const char *book_id);     /* cli_catalog.c */

/* Command handlers, in the order of the command table in cli.c. */

/* cli_catalog.c */
CmdStatus cmd_register_member(Library *lib, Args *args);
CmdStatus cmd_view_member(Library *lib, Args *args);
CmdStatus cmd_member_nonce(Library *lib, Args *args);
CmdStatus cmd_register_book(Library *lib, Args *args);
CmdStatus cmd_view_book(Library *lib, Args *args);
CmdStatus cmd_add_copy(Library *lib, Args *args);
CmdStatus cmd_view_copy(Library *lib, Args *args);
CmdStatus cmd_mark_lost(Library *lib, Args *args);

/* cli_lending.c */
CmdStatus cmd_borrow_book(Library *lib, Args *args);
CmdStatus cmd_return_book(Library *lib, Args *args);
CmdStatus cmd_renew_book(Library *lib, Args *args);
CmdStatus cmd_reserve_book(Library *lib, Args *args);
CmdStatus cmd_reservation_status(Library *lib, Args *args);
CmdStatus cmd_check_overdue(Library *lib, Args *args);
CmdStatus cmd_view_fines(Library *lib, Args *args);
CmdStatus cmd_settle_fine(Library *lib, Args *args);

/* cli_tokens.c */
CmdStatus cmd_token_transfer(Library *lib, Args *args);
CmdStatus cmd_account_transfer(Library *lib, Args *args);
CmdStatus cmd_token_balance(Library *lib, Args *args);
CmdStatus cmd_utxo_view(Library *lib, Args *args);
CmdStatus cmd_account_balance(Library *lib, Args *args);
CmdStatus cmd_account_nonce(Library *lib, Args *args);
CmdStatus cmd_transaction_history(Library *lib, Args *args);

/* cli_chain.c */
CmdStatus cmd_pool_view(Library *lib, Args *args);
CmdStatus cmd_mine_solo(Library *lib, Args *args);
CmdStatus cmd_mine_pool(Library *lib, Args *args);
CmdStatus cmd_mine_cloud(Library *lib, Args *args);
CmdStatus cmd_chain_view(Library *lib, Args *args);
CmdStatus cmd_validate_chain(Library *lib, Args *args);
CmdStatus cmd_chain_save(Library *lib, Args *args);
CmdStatus cmd_chain_load(Library *lib, Args *args);
CmdStatus cmd_difficulty_status(Library *lib, Args *args);
CmdStatus cmd_set_difficulty(Library *lib, Args *args);
CmdStatus cmd_benchmark(Library *lib, Args *args);
CmdStatus cmd_fraud_review(Library *lib, Args *args);
CmdStatus cmd_approve_suspicious(Library *lib, Args *args);
CmdStatus cmd_reject_suspicious(Library *lib, Args *args);
CmdStatus cmd_lending_history(Library *lib, Args *args);
CmdStatus cmd_member_history(Library *lib, Args *args);
CmdStatus cmd_book_history(Library *lib, Args *args);
CmdStatus cmd_advance_time(Library *lib, Args *args);
CmdStatus cmd_tamper_demo(Library *lib, Args *args);

#endif
