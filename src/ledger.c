#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ledger.h"
#include "crypto.h"

/* UTXO keeps tokens as separate unspent outputs, like banknotes; account keeps one balance each. */

/* Helpers */

const char *ledger_model_name(LedgerModel model)
{
    return model == MODEL_UTXO ? "UTXO" : "Account-based";
}

const char *format_coins(long amount, char buf[32])
{
    const char *sign = amount < 0 ? "-" : "";
    long abs_amount = amount < 0 ? -amount : amount;
    snprintf(buf, 32, "%s%ld.%02ld", sign, abs_amount / COIN, abs_amount % COIN);
    return buf;
}

static void set_error(char *err, size_t err_size, const char *fmt, const char *a, const char *b)
{
    if (err != NULL) {
        snprintf(err, err_size, fmt, a, b);
    }
}

/* Grows an array by one slot. */
static int reserve_one(void **array, int count, int *capacity, size_t element_size)
{
    if (count < *capacity) {
        return 1;
    }
    int new_capacity = *capacity == 0 ? 8 : *capacity * 2;
    void *grown = realloc(*array, (size_t)new_capacity * element_size);
    if (grown == NULL) {
        return 0;
    }
    *array = grown;
    *capacity = new_capacity;
    return 1;
}

/* tx_id = SHA-256 of all other fields. */
static void compute_tx_id(Transaction *tx)
{
    char data[2048];
    int len = snprintf(data, sizeof(data), "%s|%s|%ld|%ld|%lu|",
                       tx->sender, tx->recipient, tx->amount, tx->fee, tx->nonce);

    for (int i = 0; i < tx->input_count; i++) {
        len += snprintf(data + len, sizeof(data) - (size_t)len, "in:%s:%d,",
                        tx->inputs[i].tx_id, tx->inputs[i].output_index);
    }
    for (int i = 0; i < tx->output_count; i++) {
        len += snprintf(data + len, sizeof(data) - (size_t)len, "out:%s:%ld,",
                        tx->outputs[i].owner, tx->outputs[i].amount);
    }
    sha256_hex(data, (size_t)len, tx->tx_id);
}

/* Accounts */

void ledger_init(Ledger *ledger, LedgerModel model)
{
    memset(ledger, 0, sizeof(*ledger));
    ledger->model = model;
}

void ledger_free(Ledger *ledger)
{
    for (int i = 0; i < ledger->account_count; i++) {
        HistoryNode *node = ledger->accounts[i].history_head;
        while (node != NULL) {
            HistoryNode *next = node->next;
            free(node);
            node = next;
        }
    }
    free(ledger->accounts);
    free(ledger->utxos);
    memset(ledger, 0, sizeof(*ledger));
}

Account *ledger_find_account(const Ledger *ledger, const char *id)
{
    for (int i = 0; i < ledger->account_count; i++) {
        if (strcmp(ledger->accounts[i].id, id) == 0) {
            return &ledger->accounts[i];
        }
    }
    return NULL;
}

int ledger_open_account(Ledger *ledger, const char *id)
{
    if (ledger_find_account(ledger, id) != NULL) {
        return 1;
    }
    if (strlen(id) == 0 || strlen(id) >= ACCOUNT_ID_SIZE ||
        !reserve_one((void **)&ledger->accounts, ledger->account_count,
                     &ledger->account_capacity, sizeof(Account))) {
        return 0;
    }

    Account *account = &ledger->accounts[ledger->account_count++];
    memset(account, 0, sizeof(*account));
    strcpy(account->id, id);           /* starts at 0 tokens, nonce 0 */
    return 1;
}

long ledger_balance(const Ledger *ledger, const char *id)
{
    long total = 0;

    if (ledger->model == MODEL_ACCOUNT) {
        const Account *account = ledger_find_account(ledger, id);
        return account != NULL ? account->balance : 0;
    }

    /* UTXO: balance = sum of unspent outputs. */
    for (int i = 0; i < ledger->utxo_count; i++) {
        if (strcmp(ledger->utxos[i].owner, id) == 0) {
            total += ledger->utxos[i].amount;
        }
    }
    return total;
}

unsigned long ledger_nonce(const Ledger *ledger, const char *id)
{
    const Account *account = ledger_find_account(ledger, id);
    return account != NULL ? account->nonce : 0;
}

void ledger_bump_nonce(Ledger *ledger, const char *id)
{
    Account *account = ledger_find_account(ledger, id);
    if (account != NULL) {
        account->nonce++;
    }
}

/* Adds one entry at the tail of the account's history list. */
int ledger_restore_history(Ledger *ledger, const char *account_id, const HistoryNode *entry)
{
    Account *account = ledger_find_account(ledger, account_id);
    if (account == NULL) {
        return 0;
    }

    HistoryNode *node = malloc(sizeof(HistoryNode));
    if (node == NULL) {
        return 0;
    }
    *node = *entry;
    node->next = NULL;

    if (account->history_tail == NULL) {
        account->history_head = node;
    } else {
        account->history_tail->next = node;
    }
    account->history_tail = node;
    return 1;
}

static int log_history(Ledger *ledger, const char *account_id, const Transaction *tx, int has_nonce)
{
    HistoryNode entry;

    memset(&entry, 0, sizeof(entry));
    strcpy(entry.tx_id, tx->tx_id);
    strcpy(entry.sender, tx->sender);
    strcpy(entry.recipient, tx->recipient);
    entry.amount = tx->amount;
    entry.fee = tx->fee;
    entry.nonce = tx->nonce;
    entry.has_nonce = has_nonce;
    return ledger_restore_history(ledger, account_id, &entry);
}

/* Adds the transaction to the history of each account in it. */
static void log_to_parties(Ledger *ledger, const Transaction *tx, int has_nonce)
{
    if (strcmp(tx->sender, COINBASE_SENDER) != 0) {
        log_history(ledger, tx->sender, tx, has_nonce);
    }
    if (strcmp(tx->recipient, tx->sender) != 0) {
        log_history(ledger, tx->recipient, tx, has_nonce);
    }
    if (tx->fee > 0 && strcmp(FEE_ACCOUNT, tx->sender) != 0 &&
        strcmp(FEE_ACCOUNT, tx->recipient) != 0) {
        log_history(ledger, FEE_ACCOUNT, tx, has_nonce);
    }
}

/* UTXO */

static int find_utxo(const Ledger *ledger, const char *tx_id, int output_index)
{
    for (int i = 0; i < ledger->utxo_count; i++) {
        if (ledger->utxos[i].output_index == output_index &&
            strcmp(ledger->utxos[i].tx_id, tx_id) == 0) {
            return i;
        }
    }
    return -1;
}

static int add_utxo(Ledger *ledger, const char *tx_id, int output_index, const TxOutput *out)
{
    if (!reserve_one((void **)&ledger->utxos, ledger->utxo_count,
                     &ledger->utxo_capacity, sizeof(Utxo))) {
        return 0;
    }
    Utxo *utxo = &ledger->utxos[ledger->utxo_count++];
    strcpy(utxo->tx_id, tx_id);
    utxo->output_index = output_index;
    strcpy(utxo->owner, out->owner);
    utxo->amount = out->amount;
    return 1;
}

int ledger_restore_utxo(Ledger *ledger, const Utxo *utxo)
{
    TxOutput out;

    memset(&out, 0, sizeof(out));
    strcpy(out.owner, utxo->owner);
    out.amount = utxo->amount;
    return find_utxo(ledger, utxo->tx_id, utxo->output_index) == -1 &&
           add_utxo(ledger, utxo->tx_id, utxo->output_index, &out);
}

/* A spent output is removed, so it cannot be spent twice. */
static void remove_utxo(Ledger *ledger, int position)
{
    ledger->utxos[position] = ledger->utxos[--ledger->utxo_count];
}

/* Minting */

int ledger_mint(Ledger *ledger, const char *tx_id, const char *recipient,
                long gross, long fee, char *err, size_t err_size)
{
    if (gross <= 0 || fee < 0 || fee >= gross) {
        set_error(err, err_size, "reward for %s is not larger than its fee%s", recipient, "");
        return 0;
    }
    if (!ledger_open_account(ledger, recipient) || !ledger_open_account(ledger, FEE_ACCOUNT)) {
        set_error(err, err_size, "could not open an account for %s%s", recipient, "");
        return 0;
    }

    Transaction tx;
    memset(&tx, 0, sizeof(tx));
    strcpy(tx.sender, COINBASE_SENDER);
    strcpy(tx.recipient, recipient);
    tx.amount = gross - fee;
    tx.fee = fee;
    tx.nonce = ledger->mint_sequence++;

    /* Coinbase has no inputs: the tokens are new. */
    strcpy(tx.outputs[0].owner, recipient);
    tx.outputs[0].amount = gross - fee;
    tx.output_count = 1;
    if (fee > 0) {
        strcpy(tx.outputs[1].owner, FEE_ACCOUNT);
        tx.outputs[1].amount = fee;
        tx.output_count = 2;
    }

    if (tx_id != NULL) {
        strcpy(tx.tx_id, tx_id);
    } else {
        compute_tx_id(&tx);
    }

    if (ledger->model == MODEL_UTXO) {
        if (find_utxo(ledger, tx.tx_id, 0) != -1) {
            set_error(err, err_size, "transaction %.16s... was already applied%s", tx.tx_id, "");
            return 0;
        }
        for (int i = 0; i < tx.output_count; i++) {
            if (!add_utxo(ledger, tx.tx_id, i, &tx.outputs[i])) {
                set_error(err, err_size, "out of memory%s%s", "", "");
                return 0;
            }
        }
    } else {
        ledger_find_account(ledger, recipient)->balance += tx.amount;
        ledger_find_account(ledger, FEE_ACCOUNT)->balance += fee;
    }

    log_to_parties(ledger, &tx, 0);
    return 1;
}

/* Transfers */

/* Builds a transfer without applying it. */
static int build_transfer(Ledger *ledger, const char *sender, const char *recipient,
                          long amount, unsigned long nonce, Transaction *tx,
                          char *err, size_t err_size)
{
    memset(tx, 0, sizeof(*tx));

    if (ledger_find_account(ledger, sender) == NULL ||
        ledger_find_account(ledger, recipient) == NULL) {
        set_error(err, err_size, "unknown account (%s -> %s)", sender, recipient);
        return 0;
    }
    if (strcmp(sender, recipient) == 0) {
        set_error(err, err_size, "%s cannot send tokens to itself%s", sender, "");
        return 0;
    }
    if (amount <= 0) {
        set_error(err, err_size, "the amount must be positive%s%s", "", "");
        return 0;
    }

    strcpy(tx->sender, sender);
    strcpy(tx->recipient, recipient);
    tx->amount = amount;
    tx->fee = TX_FEE;
    tx->nonce = nonce;

    if (ledger->model == MODEL_ACCOUNT) {
        compute_tx_id(tx);
        return 1;
    }

    /* UTXO: take the oldest outputs until they cover amount + fee. */
    long needed = amount + TX_FEE;
    long gathered = 0;

    for (int i = 0; i < ledger->utxo_count && gathered < needed; i++) {
        if (strcmp(ledger->utxos[i].owner, sender) != 0) {
            continue;
        }
        if (tx->input_count == MAX_TX_INPUTS) {
            set_error(err, err_size, "%s needs more than the maximum number of inputs%s", sender, "");
            return 0;
        }
        strcpy(tx->inputs[tx->input_count].tx_id, ledger->utxos[i].tx_id);
        tx->inputs[tx->input_count].output_index = ledger->utxos[i].output_index;
        tx->input_count++;
        gathered += ledger->utxos[i].amount;
    }

    if (gathered < needed) {
        char have[32], need[32];
        set_error(err, err_size, "inputs insufficient: %s owns %s", sender, format_coins(gathered, have));
        if (err != NULL) {
            size_t used = strlen(err);
            snprintf(err + used, err_size - used, ", needs %s (amount + fee)",
                     format_coins(needed, need));
        }
        return 0;
    }

    strcpy(tx->outputs[0].owner, recipient);
    tx->outputs[0].amount = amount;
    tx->output_count = 1;

    if (gathered > needed) {           /* change */
        strcpy(tx->outputs[1].owner, sender);
        tx->outputs[1].amount = gathered - needed;
        tx->output_count = 2;
    }

    strcpy(tx->outputs[tx->output_count].owner, FEE_ACCOUNT);
    tx->outputs[tx->output_count].amount = TX_FEE;
    tx->output_count++;

    compute_tx_id(tx);
    return 1;
}

static int submit_utxo(Ledger *ledger, const Transaction *tx, char *err, size_t err_size)
{
    long total_in = 0, total_out = 0;
    int positions[MAX_TX_INPUTS];

    if (tx->input_count < 1 || tx->input_count > MAX_TX_INPUTS ||
        tx->output_count < 1 || tx->output_count > MAX_TX_OUTPUTS) {
        set_error(err, err_size, "malformed transaction%s%s", "", "");
        return 0;
    }

    for (int i = 0; i < tx->input_count; i++) {
        /* The same output twice is a double spend. */
        for (int j = 0; j < i; j++) {
            if (tx->inputs[j].output_index == tx->inputs[i].output_index &&
                strcmp(tx->inputs[j].tx_id, tx->inputs[i].tx_id) == 0) {
                set_error(err, err_size, "DOUBLE SPEND: input %.16s... is listed twice%s",
                          tx->inputs[i].tx_id, "");
                return 0;
            }
        }

        positions[i] = find_utxo(ledger, tx->inputs[i].tx_id, tx->inputs[i].output_index);
        if (positions[i] == -1) {
            set_error(err, err_size,
                      "DOUBLE SPEND: input %.16s... is not in the UTXO set (already spent)%s",
                      tx->inputs[i].tx_id, "");
            return 0;
        }
        if (strcmp(ledger->utxos[positions[i]].owner, tx->sender) != 0) {
            set_error(err, err_size, "input %.16s... does not belong to %s",
                      tx->inputs[i].tx_id, tx->sender);
            return 0;
        }
        total_in += ledger->utxos[positions[i]].amount;
    }

    for (int i = 0; i < tx->output_count; i++) {
        if (tx->outputs[i].amount <= 0) {
            set_error(err, err_size, "output %s has no value%s", tx->outputs[i].owner, "");
            return 0;
        }
        total_out += tx->outputs[i].amount;
    }

    if (total_in < tx->amount + tx->fee || total_in != total_out) {
        set_error(err, err_size, "inputs do not cover outputs plus fee%s%s", "", "");
        return 0;
    }

    Transaction check = *tx;
    compute_tx_id(&check);
    if (strcmp(check.tx_id, tx->tx_id) != 0) {
        set_error(err, err_size, "transaction ID does not match its contents%s%s", "", "");
        return 0;
    }

    /* Spend the inputs, highest position first, because removal swaps from the end. */
    for (int pass = 0; pass < tx->input_count; pass++) {
        int highest = 0;
        for (int i = 1; i < tx->input_count; i++) {
            if (positions[i] > positions[highest]) highest = i;
        }
        remove_utxo(ledger, positions[highest]);
        positions[highest] = -1;
    }

    for (int i = 0; i < tx->output_count; i++) {
        if (!ledger_open_account(ledger, tx->outputs[i].owner) ||
            !add_utxo(ledger, tx->tx_id, i, &tx->outputs[i])) {
            set_error(err, err_size, "out of memory%s%s", "", "");
            return 0;
        }
    }

    log_to_parties(ledger, tx, tx->fee > 0);   /* fee-free charges carry no nonce */
    return 1;
}

static int submit_account(Ledger *ledger, const Transaction *tx, char *err, size_t err_size)
{
    Account *sender = ledger_find_account(ledger, tx->sender);
    char nonce_text[24], expected_text[24];

    if (sender == NULL || ledger_find_account(ledger, tx->recipient) == NULL ||
        !ledger_open_account(ledger, FEE_ACCOUNT)) {
        set_error(err, err_size, "unknown account (%s -> %s)", tx->sender, tx->recipient);
        return 0;
    }
    sender = ledger_find_account(ledger, tx->sender);   /* the array may have moved */

    if (tx->nonce != sender->nonce) {
        snprintf(nonce_text, sizeof(nonce_text), "%lu", tx->nonce);
        snprintf(expected_text, sizeof(expected_text), "%lu", sender->nonce);
        set_error(err, err_size, "bad nonce: got %s, expected %s (reused or out of order)",
                  nonce_text, expected_text);
        return 0;
    }

    if (tx->amount <= 0 || tx->fee != TX_FEE) {
        set_error(err, err_size, "bad amount or fee%s%s", "", "");
        return 0;
    }

    if (sender->balance < tx->amount + tx->fee) {
        char have[32], need[32];
        set_error(err, err_size, "insufficient balance: has %s, needs %s (amount + fee)",
                  format_coins(sender->balance, have), format_coins(tx->amount + tx->fee, need));
        return 0;
    }

    sender->balance -= tx->amount + tx->fee;            /* debit  */
    ledger_find_account(ledger, tx->recipient)->balance += tx->amount;   /* credit */
    ledger_find_account(ledger, FEE_ACCOUNT)->balance += tx->fee;
    /* The nonce moves when the signed request is confirmed: see ledger_bump_nonce(). */

    log_to_parties(ledger, tx, 1);
    return 1;
}

long ledger_charge(Ledger *ledger, const char *payer, const char *payee, long amount)
{
    Transaction tx;

    if (amount <= 0 || strcmp(payer, payee) == 0 ||
        !ledger_open_account(ledger, payer) || !ledger_open_account(ledger, payee)) {
        return 0;
    }

    memset(&tx, 0, sizeof(tx));
    strcpy(tx.sender, payer);
    strcpy(tx.recipient, payee);

    if (ledger->model == MODEL_ACCOUNT) {
        Account *from = ledger_find_account(ledger, payer);
        long paid = from->balance < amount ? from->balance : amount;
        if (paid <= 0) {
            return 0;
        }
        tx.amount = paid;
        tx.nonce = ledger->mint_sequence++;                         /* keeps the tx_id unique */
        compute_tx_id(&tx);

        from->balance -= paid;                                      /* debit  */
        ledger_find_account(ledger, payee)->balance += paid;        /* credit */
        log_to_parties(ledger, &tx, 0);
        return paid;
    }

    /* UTXO: spend outputs until the charge is covered. */
    long gathered = 0;
    for (int i = 0; i < ledger->utxo_count && gathered < amount &&
                    tx.input_count < MAX_TX_INPUTS; i++) {
        if (strcmp(ledger->utxos[i].owner, payer) != 0) {
            continue;
        }
        strcpy(tx.inputs[tx.input_count].tx_id, ledger->utxos[i].tx_id);
        tx.inputs[tx.input_count].output_index = ledger->utxos[i].output_index;
        tx.input_count++;
        gathered += ledger->utxos[i].amount;
    }

    long paid = gathered < amount ? gathered : amount;
    if (paid <= 0) {
        return 0;
    }

    tx.amount = paid;
    strcpy(tx.outputs[0].owner, payee);
    tx.outputs[0].amount = paid;
    tx.output_count = 1;
    if (gathered > paid) {             /* change */
        strcpy(tx.outputs[1].owner, payer);
        tx.outputs[1].amount = gathered - paid;
        tx.output_count = 2;
    }
    compute_tx_id(&tx);

    return submit_utxo(ledger, &tx, NULL, 0) ? paid : 0;
}

int ledger_submit(Ledger *ledger, const Transaction *tx, char *err, size_t err_size)
{
    return ledger->model == MODEL_UTXO ? submit_utxo(ledger, tx, err, err_size)
                                       : submit_account(ledger, tx, err, err_size);
}

int ledger_transfer(Ledger *ledger, const char *sender, const char *recipient,
                    long amount, unsigned long nonce, Transaction *tx_out,
                    char *err, size_t err_size)
{
    Transaction tx;

    if (!build_transfer(ledger, sender, recipient, amount, nonce, &tx, err, err_size) ||
        !ledger_submit(ledger, &tx, err, err_size)) {
        return 0;
    }
    if (tx_out != NULL) {
        *tx_out = tx;
    }
    return 1;
}

int ledger_convert(Ledger *ledger, LedgerModel model)
{
    if (ledger->model == model) {
        return 1;
    }

    if (model == MODEL_ACCOUNT) {
        /* A balance becomes the sum of the account's unspent outputs. */
        for (int i = 0; i < ledger->account_count; i++) {
            ledger->accounts[i].balance = ledger_balance(ledger, ledger->accounts[i].id);
        }
        ledger->utxo_count = 0;
        ledger->model = MODEL_ACCOUNT;
        return 1;
    }

    /* Each balance becomes one unspent output. */
    ledger->model = MODEL_UTXO;
    ledger->utxo_count = 0;
    for (int i = 0; i < ledger->account_count; i++) {
        Account *account = &ledger->accounts[i];
        if (account->balance > 0) {
            char data[96];
            char tx_id[65];
            TxOutput out;

            int len = snprintf(data, sizeof(data), "CONVERT|%s|%ld|%lu", account->id,
                               account->balance, ledger->mint_sequence++);
            sha256_hex(data, (size_t)len, tx_id);

            memset(&out, 0, sizeof(out));
            strcpy(out.owner, account->id);
            out.amount = account->balance;
            if (!add_utxo(ledger, tx_id, 0, &out)) {
                return 0;
            }
        }
        account->balance = 0;
    }
    return 1;
}

/* Printing */

void ledger_print_balances(const Ledger *ledger)
{
    char amount[32];

    printf("\n%-12s %12s %8s\n", "Account", "Balance", "Nonce");
    printf("------------ ------------ --------\n");

    for (int i = 0; i < ledger->account_count; i++) {
        const Account *a = &ledger->accounts[i];
        printf("%-12s %12s %8lu\n", a->id, format_coins(ledger_balance(ledger, a->id), amount),
               a->nonce);
    }
}

void ledger_print_utxo_set(const Ledger *ledger)
{
    char amount[32];
    long total = 0;

    printf("\nUTXO set (%d unspent output%s):\n", ledger->utxo_count,
           ledger->utxo_count == 1 ? "" : "s");
    printf("%-20s %4s  %-12s %10s\n", "TX ID", "Out", "Owner", "Amount");
    printf("-------------------- ----  ------------ ----------\n");

    for (int i = 0; i < ledger->utxo_count; i++) {
        const Utxo *u = &ledger->utxos[i];
        printf("%.16s...  %4d  %-12s %10s\n", u->tx_id, u->output_index, u->owner,
               format_coins(u->amount, amount));
        total += u->amount;
    }
    printf("%-39s %10s\n", "Total supply", format_coins(total, amount));
}

void ledger_print_transaction(const Transaction *tx)
{
    char amount[32], fee[32];

    printf("  TX %.16s...  %s -> %s  amount %s  fee %s",
           tx->tx_id, tx->sender, tx->recipient,
           format_coins(tx->amount, amount), format_coins(tx->fee, fee));
    printf("  nonce %lu\n", tx->nonce);
    for (int i = 0; i < tx->input_count; i++) {
        printf("    in : %.16s...:%d\n", tx->inputs[i].tx_id, tx->inputs[i].output_index);
    }
    for (int i = 0; i < tx->output_count; i++) {
        /* Output 0 is the payment; the others are the sender's change and the fee. */
        printf("    out: %-12s %s%s\n", tx->outputs[i].owner,
               format_coins(tx->outputs[i].amount, amount),
               i == 0 ? "" :
               strcmp(tx->outputs[i].owner, tx->sender) == 0 ? "  (change)" : "  (fee)");
    }
}

int ledger_print_history(const Ledger *ledger, const char *id)
{
    const Account *account = ledger_find_account(ledger, id);
    char amount[32], fee[32], balance[32];

    if (account == NULL) {
        return 0;
    }

    printf("\nTransaction history for %s (balance %s):\n", id,
           format_coins(ledger_balance(ledger, id), balance));
    printf("%-20s %-12s %-12s %10s %6s %6s\n", "TX ID", "Sender", "Recipient", "Amount", "Fee", "Nonce");
    printf("-------------------- ------------ ------------ ---------- ------ ------\n");

    int rows = 0;
    for (const HistoryNode *node = account->history_head; node != NULL; node = node->next) {
        char nonce[24] = "-";
        if (node->has_nonce) snprintf(nonce, sizeof(nonce), "%lu", node->nonce);

        printf("%.16s...  %-12s %-12s %10s %6s %6s\n", node->tx_id, node->sender,
               node->recipient, format_coins(node->amount, amount),
               format_coins(node->fee, fee), nonce);
        rows++;
    }
    if (rows == 0) {
        printf("(no transactions yet)\n");
    }
    return 1;
}
