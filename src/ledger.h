#ifndef LEDGER_H
#define LEDGER_H

#include <stddef.h>

#include "config.h"

/* Token balances in UTXO or ACCOUNT model; amounts are in hundredths of a coin. */

typedef enum { MODEL_UTXO, MODEL_ACCOUNT } LedgerModel;

#define TX_FEE          (1 * COIN)     /* fee on every reward and transfer */
#define FEE_ACCOUNT     "LIBRARY"      /* collects all fees and settled fines */
#define COINBASE_SENDER "COINBASE"     /* sender of new tokens */
#define POOL_OPERATOR_ACCOUNT  "POOL-OP"    /* receives the pool fee */
#define CLOUD_PROVIDER_ACCOUNT "CLOUD-OP"   /* receives the rental fees */

#define ACCOUNT_ID_SIZE 20
#define MAX_TX_INPUTS   32
#define MAX_TX_OUTPUTS  3              /* recipient, change, fee */

typedef struct {
    char tx_id[65];       /* transaction that made the output */
    int output_index;
} TxInput;

typedef struct {
    char owner[ACCOUNT_ID_SIZE];
    long amount;
} TxOutput;

typedef struct {
    char tx_id[65];                    /* SHA-256 of the fields below */
    char sender[ACCOUNT_ID_SIZE];
    char recipient[ACCOUNT_ID_SIZE];
    long amount;                       /* what the recipient gets */
    long fee;
    unsigned long nonce;               /* the sender's nonce */

    int input_count;                   /* UTXO model only */
    TxInput inputs[MAX_TX_INPUTS];
    int output_count;
    TxOutput outputs[MAX_TX_OUTPUTS];
} Transaction;

/* One entry in the UTXO set. */
typedef struct {
    char tx_id[65];
    int output_index;
    char owner[ACCOUNT_ID_SIZE];
    long amount;
} Utxo;

/* Account history, a linked list. */
typedef struct HistoryNode {
    char tx_id[65];
    char sender[ACCOUNT_ID_SIZE];
    char recipient[ACCOUNT_ID_SIZE];
    long amount;
    long fee;
    unsigned long nonce;
    int has_nonce;                     /* 0 for minted tokens and rental charges */
    struct HistoryNode *next;
} HistoryNode;

typedef struct {
    char id[ACCOUNT_ID_SIZE];
    long balance;                      /* account model only */
    /*
     * Next nonce this account must use. It counts every signed request of the
     * account (lending requests and transfers), in both models, and moves only
     * when a request is confirmed in a mined block.
     */
    unsigned long nonce;
    HistoryNode *history_head;
    HistoryNode *history_tail;
} Account;

typedef struct {
    LedgerModel model;

    Account *accounts;                 /* heap arrays */
    int account_count;
    int account_capacity;

    Utxo *utxos;
    int utxo_count;
    int utxo_capacity;

    unsigned long mint_sequence;       /* makes every coinbase tx_id unique */
} Ledger;

void ledger_init(Ledger *ledger, LedgerModel model);
void ledger_free(Ledger *ledger);
const char *ledger_model_name(LedgerModel model);

/* Creates the account if missing; returns 0 if memory ran out. */
int ledger_open_account(Ledger *ledger, const char *id);
Account *ledger_find_account(const Ledger *ledger, const char *id);
long ledger_balance(const Ledger *ledger, const char *id);

/* The account's next nonce, 0 for an unknown account. */
unsigned long ledger_nonce(const Ledger *ledger, const char *id);
/* Called once for every confirmed request the account signed. */
void ledger_bump_nonce(Ledger *ledger, const char *id);

/* Creates new tokens; fee is taken from gross, tx_id may be NULL. */
int ledger_mint(Ledger *ledger, const char *tx_id, const char *recipient,
                long gross, long fee, char *err, size_t err_size);

/*
 * Moves amount from sender to recipient and the fixed fee to the library.
 * Rejects double spends, a wrong nonce (account model) and low balances.
 * tx_out may be NULL.
 */
int ledger_transfer(Ledger *ledger, const char *sender, const char *recipient,
                    long amount, unsigned long nonce, Transaction *tx_out,
                    char *err, size_t err_size);

/* Applies an already built transaction again: used to show that a replay is refused. */
int ledger_submit(Ledger *ledger, const Transaction *tx, char *err, size_t err_size);

/* Moves up to amount from payer to payee with no fee; returns what was paid. */
long ledger_charge(Ledger *ledger, const char *payer, const char *payee, long amount);

/* Switches the model, carrying every balance over. */
int ledger_convert(Ledger *ledger, LedgerModel model);

/* Used when restoring saved state. */
int ledger_restore_utxo(Ledger *ledger, const Utxo *utxo);
int ledger_restore_history(Ledger *ledger, const char *account_id, const HistoryNode *entry);

/* Amount as text, like "12.50". */
const char *format_coins(long amount, char buf[32]);

void ledger_print_balances(const Ledger *ledger);
void ledger_print_utxo_set(const Ledger *ledger);
void ledger_print_transaction(const Transaction *tx);
int ledger_print_history(const Ledger *ledger, const char *id);

#endif
