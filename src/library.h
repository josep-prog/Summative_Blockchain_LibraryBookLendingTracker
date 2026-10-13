#ifndef LIBRARY_H
#define LIBRARY_H

#include <time.h>

#include "config.h"
#include "registry.h"
#include "inventory.h"
#include "keystore.h"
#include "blockchain.h"
#include "pending.h"
#include "ledger.h"

/* Everything the program knows, in one place. */
typedef struct {
    Registry registry;
    Inventory inventory;
    Chain chain;
    PendingPool pool;
    Ledger ledger;
    KeyStore keys;

    int difficulty;                  /* for blocks mined from now on */
    long clock_offset;               /* seconds added to the system clock (advance_time) */
    unsigned long next_request_seq;  /* next generated request ID */

    const Librarian *user;           /* the logged-in librarian */
    int seal_ok;                     /* the saved file's seal was valid */
    int locked;                      /* 1 until the loaded chain has passed validation */

    const char *books_file;
    const char *members_file;
    const char *copies_file;
    const char *data_file;
} Library;

/* Optional parts of a request; zero means "use the default". */
typedef struct {
    const char *request_id;          /* NULL: generate one */
    int has_nonce;                   /* 0: use the signer's next nonce */
    unsigned long nonce;
    time_t due_date;                 /* borrow: 0 = LOAN_DAYS from now */
    const char *condition;           /* return: NULL = unchanged */
    const char *reason;              /* librarian override reason code */
} RequestOptions;

/* A fraud heuristic that fired. */
typedef struct {
    const char *heuristic;
    char detail[FLAG_DETAIL_SIZE];
} FraudFlag;

/* library.c: session state */

/* The library's clock: the system clock plus the simulated offset. */
time_t library_now(const Library *lib);
int library_is_admin(const Library *lib);
void library_free(Library *lib);
/*
 * Loads the saved state, or starts a new chain with a genesis block, then
 * validates the chain. Returns 0 if the program cannot continue.
 */
int library_boot(Library *lib, LedgerModel model, int model_given);
/* Re-validates the whole chain, prints the result and locks or unlocks the session. */
int library_validate(Library *lib);
/* Writes the state to disk; an invalid chain is never saved. */
int library_save(Library *lib);

/* lending.c: lending requests and their effects */

/* Each returns 1 if a signed request entered the pending pool. */
int lending_borrow(Library *lib, const char *book_id, const char *member_id,
                   const RequestOptions *options);
int lending_return(Library *lib, const char *copy_id, const RequestOptions *options);
int lending_renew(Library *lib, const char *copy_id, const RequestOptions *options);
int lending_reserve(Library *lib, const char *book_id, const char *member_id,
                    const RequestOptions *options);
int lending_mark_lost(Library *lib, const char *copy_id, const RequestOptions *options);
int lending_settle_fine(Library *lib, const char *member_id, long amount,
                        const RequestOptions *options);
int lending_transfer(Library *lib, const char *sender, const char *recipient, long amount,
                     const RequestOptions *options);

/* Raises an OVERDUE record for every loan past its due date; returns how many. */
int lending_scan_overdue(Library *lib);

/*
 * The hard rules a request must pass, checked when it is submitted and again
 * when a miner picks it. At mining time only confirmed state is considered.
 */
int request_check(const Library *lib, const LendingRecord *record, int at_mining,
                  char *why, size_t why_size);
/* Tokens the request takes from its member: amount plus fee, or 0. */
long record_spend(const LendingRecord *record);
/* Applies a confirmed record to the inventory, queues, fines and ledger. */
void lending_apply(Library *lib, const LendingRecord *record);

/* The signer's confirmed nonce plus its requests still in the pool. */
unsigned long lending_next_nonce(const Library *lib, const char *signer_id);
/* Removes a request, and the signer's later requests whose nonces can no longer be used. */
void lending_drop_request(Library *lib, int index, const char *why);
/* Copies a member currently holds (BORROWED). */
int lending_copies_held(const Library *lib, const char *member_id);

/* fraud.c: misuse heuristics */

/* Returns 1 and fills flag if the request looks like lending abuse. */
int fraud_check(const Library *lib, const LendingRecord *record, FraudFlag *flag);

#endif
