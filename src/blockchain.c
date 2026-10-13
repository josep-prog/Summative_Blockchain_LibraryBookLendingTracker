#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "blockchain.h"
#include "ledger.h"
#include "merkle.h"

const char *format_time(time_t when, char buf[20])
{
    struct tm *local = when != 0 ? localtime(&when) : NULL;

    if (local == NULL || strftime(buf, 20, "%Y-%m-%d %H:%M", local) == 0) {
        strcpy(buf, "-");
    }
    return buf;
}

/* Records */

static const char *const ACTIONS[] = {
    ACTION_BORROWED, ACTION_RETURNED, ACTION_RESERVED, ACTION_RENEWED,
    ACTION_OVERDUE, ACTION_LOST, ACTION_SETTLED, ACTION_TRANSFER
};

int record_action_known(const char *action)
{
    for (size_t i = 0; i < sizeof(ACTIONS) / sizeof(ACTIONS[0]); i++) {
        if (strcmp(action, ACTIONS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_action(const LendingRecord *record, const char *action)
{
    return strcmp(record->action, action) == 0;
}

/* Signed part: every field of the record except the signature itself. */
size_t record_signed_data(const LendingRecord *record, unsigned char data[RECORD_DATA_SIZE])
{
    int len = snprintf(
        (char *)data,
        RECORD_DATA_SIZE,
        "%s|%ld|%s|%s|%s|%s|%s|%s|%s|%ld|%ld|%ld|%s|%s|%d|%s|%lu|%s|%s",
        record->record_id,
        (long)record->timestamp,
        record->action,
        record->book_id,
        record->copy_id,
        record->book_title,
        record->member_id,
        record->member_name,
        record->peer_id,
        (long)record->due_date,
        record->fine_amount,
        record->amount,
        record->condition,
        record->reason,
        record->token_reward,
        record->tx_id,
        record->nonce,
        record->signer_id,
        record->signer_pubkey
    );
    return (size_t)len;
}

void record_hash(const LendingRecord *record, unsigned char digest[HASH_BYTES])
{
    unsigned char data[RECORD_DATA_SIZE + SIGNATURE_MAX];
    size_t len = record_signed_data(record, data);
    unsigned int signature_len = record->signature_length <= SIGNATURE_MAX
                                 ? record->signature_length : SIGNATURE_MAX;

    memcpy(data + len, record->signature, signature_len);
    sha256_raw(data, len + signature_len, digest);
}

/* The same input always gives the same id. */
void record_reward_tx_id(const LendingRecord *record, char out_hex[HASH_HEX_SIZE])
{
    char data[160];
    int len = snprintf(data, sizeof(data), "REWARD|%s|%ld|%s|%s|%d",
                       record->record_id, (long)record->timestamp, record->copy_id,
                       record->member_id, record->token_reward);
    sha256_hex(data, (size_t)len, out_hex);
}

long overdue_fine(time_t due_date, time_t when)
{
    if (when <= due_date) {
        return 0;
    }
    /* Every started day counts. */
    long days_late = ((long)(when - due_date) + DAY_SECONDS - 1) / DAY_SECONDS;
    return days_late * FINE_PER_DAY;
}

int record_verify(const LendingRecord *record, const char **reason)
{
    const char *why = NULL;
    int moves_tokens = is_action(record, ACTION_SETTLED) || is_action(record, ACTION_TRANSFER);

    if (!record_action_known(record->action)) {
        why = "unknown action";
    } else if (record->fine_amount < 0 || record->amount < 0 || record->token_reward < 0) {
        why = "amounts cannot be negative";
    } else if (is_action(record, ACTION_RETURNED)) {
        /* Only RETURNED earns tokens: 10 on time, 5 late, and its tx_id must match. */
        int late = record->timestamp > record->due_date;
        char expected[HASH_HEX_SIZE];
        record_reward_tx_id(record, expected);

        if (record->token_reward != (late ? REWARD_LATE : REWARD_ON_TIME)) {
            why = "reward does not match the return (10 on time, 5 late)";
        } else if (strcmp(record->tx_id, expected) != 0) {
            why = "reward transaction ID does not match the record";
        } else if (record->fine_amount != overdue_fine(record->due_date, record->timestamp)) {
            why = "fine does not match the number of days late";
        }
    } else if (record->token_reward != 0 || record->tx_id[0] != '\0') {
        why = "only RETURNED records may carry a token reward";
    } else if (is_action(record, ACTION_OVERDUE)) {
        if (record->fine_amount <= 0 ||
            record->fine_amount != overdue_fine(record->due_date, record->timestamp)) {
            why = "OVERDUE record does not match its due date";
        }
    } else if (record->fine_amount != 0) {
        why = "only late RETURNED and OVERDUE records may carry a fine";
    }

    if (why == NULL && moves_tokens != (record->amount > 0)) {
        why = "token amount does not fit the action";
    }

    if (why == NULL) {
        /* Verify against the public key stored in the record itself. */
        EVP_PKEY *key = record->signature_length > 0 && record->signature_length <= SIGNATURE_MAX
                        ? public_key_from_hex(record->signer_pubkey) : NULL;
        unsigned char data[RECORD_DATA_SIZE];
        size_t len = record_signed_data(record, data);

        if (key == NULL) {
            why = "signature or signer public key is missing or malformed";
        } else if (!verify_signature(key, data, len, record->signature, record->signature_length)) {
            why = "digital signature is invalid";
        }
        EVP_PKEY_free(key);
    }

    if (reason != NULL) *reason = why;
    return why == NULL;
}

void print_record(const LendingRecord *record)
{
    char when[20], due[20], amount[32];

    printf("    %-10s %-8s ", record->record_id, record->action);
    if (is_action(record, ACTION_TRANSFER)) {
        printf("%s -> %s  amount %s", record->member_id, record->peer_id,
               format_coins(record->amount, amount));
    } else if (is_action(record, ACTION_SETTLED)) {
        printf("%s (%s) pays a fine of %s", record->member_id, record->member_name,
               format_coins(record->amount, amount));
    } else {
        printf("%-9s '%.40s'  %s%s%s%s", record->copy_id[0] != '\0' ? record->copy_id : record->book_id,
               record->book_title, record->member_id,
               record->member_id[0] != '\0' ? " (" : "(no member)", record->member_name,
               record->member_id[0] != '\0' ? ")" : "");
    }
    printf("\n               at %s", format_time(record->timestamp, when));
    if (record->due_date != 0) printf(" | due %s", format_time(record->due_date, due));
    if (record->condition[0] != '\0') printf(" | condition %s", record->condition);
    if (record->reason[0] != '\0') printf(" | reason %s", record->reason);
    if (record->fine_amount > 0) printf(" | fine %s", format_coins(record->fine_amount, amount));
    if (record->token_reward > 0) printf(" | reward %d coins", record->token_reward);
    printf("\n               nonce %lu | signed by %s (key %.16s...) | signature %s\n",
           record->nonce, record->signer_id, record->signer_pubkey,
           record_verify(record, NULL) ? "VALID" : "INVALID");
}

/* Blocks */

int block_merkle_root(const Block *block, char out_hex[HASH_HEX_SIZE], FILE *trace)
{
    unsigned char leaves[BLOCK_CAPACITY][HASH_BYTES];
    unsigned char root[HASH_BYTES];
    int count = block->record_count;

    if (count < 0 || count > BLOCK_CAPACITY) {
        return 0;
    }
    for (int i = 0; i < count; i++) {
        record_hash(&block->records[i], leaves[i]);
    }
    if (!merkle_root((const unsigned char (*)[HASH_BYTES])leaves, count, root, trace)) {
        return 0;
    }
    bytes_to_hex(root, HASH_BYTES, out_hex);
    return 1;
}

/* Total reward of the block and one ID covering its reward transactions. */
static void reward_summary(const Block *block, int *total, char tx_id[HASH_HEX_SIZE])
{
    char joined[BLOCK_CAPACITY * HASH_HEX_SIZE + 1] = "";

    *total = 0;
    for (int i = 0; i < block->record_count && i < BLOCK_CAPACITY; i++) {
        if (block->records[i].token_reward > 0) {
            *total += block->records[i].token_reward;
            strcat(joined, block->records[i].tx_id);
        }
    }

    if (joined[0] == '\0') {
        tx_id[0] = '\0';             /* no return in this block: no transaction */
    } else {
        sha256_hex(joined, strlen(joined), tx_id);
    }
}

int block_seal_records(Block *block)
{
    reward_summary(block, &block->token_reward, block->transaction_id);
    return block_merkle_root(block, block->merkle_root, NULL);
}

#define HEADER_SIZE 384

/* Header = all hashed fields except the nonce. The records are covered by merkle_root. */
static size_t build_hash_header(const Block *block, char buf[HEADER_SIZE])
{
    return (size_t)snprintf(buf, HEADER_SIZE - 32, "%d|%ld|%d|%s|%s|%s|%d|%s|%d",
                            block->index, (long)block->timestamp, block->record_count,
                            block->previous_hash, block->merkle_root, block->sealed_by,
                            block->token_reward, block->transaction_id, block->difficulty);
}

static void hash_with_nonce(char buf[HEADER_SIZE], size_t header_len, unsigned long nonce,
                            char out[HASH_HEX_SIZE])
{
    size_t len = header_len + (size_t)snprintf(buf + header_len, 32, "|%lu", nonce);
    sha256_hex(buf, len, out);
}

void calculate_hash(Block *block)
{
    char buf[HEADER_SIZE];
    size_t header_len = build_hash_header(block, buf);
    hash_with_nonce(buf, header_len, block->nonce, block->hash);
}

/* Difficulty 2 means the hash must start with "00", difficulty 3 with "000". */
int hash_meets_difficulty(const char *hash, int difficulty)
{
    for (int i = 0; i < difficulty; i++) {
        if (hash[i] != '0') {
            return 0;
        }
    }
    return 1;
}

/* Proof of work: try nonce 0, 1, 2... until the block's hash starts with enough zeros. */
int mine_attempts(Block *block, unsigned long max_attempts, unsigned long *attempts_used)
{
    char buf[HEADER_SIZE];
    size_t header_len = build_hash_header(block, buf);
    char hash[HASH_HEX_SIZE];

    *attempts_used = 0;
    while (*attempts_used < max_attempts) {
        hash_with_nonce(buf, header_len, block->nonce, hash);
        (*attempts_used)++;

        if (hash_meets_difficulty(hash, block->difficulty)) {
            strcpy(block->hash, hash);
            return 1;
        }
        block->nonce++;
    }
    return 0;
}

void create_genesis_block(Block *block, int difficulty, time_t now)
{
    unsigned long attempts;

    memset(block, 0, sizeof(Block));

    block->index = 0;
    block->timestamp = now;
    strcpy(block->sealed_by, "GENESIS");

    /* Block 0 has no block before it, so its previous hash is 64 zeros. */
    memset(block->previous_hash, '0', 64);
    block->previous_hash[64] = '\0';

    block_seal_records(block);
    block->difficulty = difficulty;
    mine_attempts(block, (unsigned long)-1, &attempts);
}

void print_block(const Block *block, int show_merkle)
{
    char when[20];

    printf("\n--------------------------------------------------------------------------\n");
    printf("Block #%d  [%s]  %d record%s, sealed by %s\n", block->index,
           block->index == 0 ? "GENESIS" : "LENDING", block->record_count,
           block->record_count == 1 ? "" : "s", block->sealed_by);
    printf("  Time        : %s\n", format_time(block->timestamp, when));
    printf("  PoW         : difficulty %d, nonce %lu\n", block->difficulty, block->nonce);
    printf("  Hash        : %s\n", block->hash);
    printf("  Prev        : %s\n", block->previous_hash);
    printf("  Merkle root : %s\n", block->merkle_root);
    printf("  Reward      : %d coin%s for returns", block->token_reward,
           block->token_reward == 1 ? "" : "s");
    if (block->transaction_id[0] != '\0') {
        printf(", TX %.16s...", block->transaction_id);
    }
    printf("\n");

    if (show_merkle && block->record_count > 0) {
        char root[HASH_HEX_SIZE];
        printf("  Merkle tree (level 0 = record hashes):\n");
        block_merkle_root(block, root, stdout);
    }
    for (int i = 0; i < block->record_count && i < BLOCK_CAPACITY; i++) {
        print_record(&block->records[i]);
    }
}

/* Chain */

void chain_init(Chain *chain)
{
    memset(chain, 0, sizeof(*chain));
}

void chain_free(Chain *chain)
{
    free(chain->blocks);
    memset(chain, 0, sizeof(*chain));
}

int chain_append(Chain *chain, const Block *block)
{
    if (chain->count == chain->capacity) {
        int new_capacity = chain->capacity == 0 ? 16 : chain->capacity * 2;
        Block *grown = realloc(chain->blocks, (size_t)new_capacity * sizeof(Block));
        if (grown == NULL) {
            return 0;
        }
        chain->blocks = grown;
        chain->capacity = new_capacity;
    }
    chain->blocks[chain->count++] = *block;
    return 1;
}

const LendingRecord *chain_last_record(const Chain *chain, const char *copy_id,
                                       const char *action)
{
    for (int b = chain->count - 1; b > 0; b--) {
        const Block *block = &chain->blocks[b];
        for (int r = block->record_count - 1; r >= 0; r--) {
            if (strcmp(block->records[r].copy_id, copy_id) == 0 &&
                strcmp(block->records[r].action, action) == 0) {
                return &block->records[r];
            }
        }
    }
    return NULL;
}

const LendingRecord *chain_find_record(const Chain *chain, const char *record_id)
{
    for (int b = 1; b < chain->count; b++) {
        const Block *block = &chain->blocks[b];
        for (int r = 0; r < block->record_count; r++) {
            if (strcmp(block->records[r].record_id, record_id) == 0) {
                return &block->records[r];
            }
        }
    }
    return NULL;
}

static int invalid(ChainFault *fault, int block, int record, const char *why)
{
    if (fault != NULL) {
        fault->block = block;
        fault->record = record;
        snprintf(fault->reason, sizeof(fault->reason), "%s", why);
    }
    return 0;
}

/* Next expected nonce of every signer seen so far. */
typedef struct {
    char id[ID_SIZE];
    unsigned long next;
} SignerNonce;

static const char *check_record(const Chain *chain, int b, int r, const Registry *registry,
                                const KeyStore *keys, SignerNonce signers[], int *signer_count)
{
    const LendingRecord *record = &chain->blocks[b].records[r];
    const char *why;

    if (!record_verify(record, &why)) {
        return why;
    }

    /* The key in the record must be the one registered for that identity. */
    const KeyEntry *entry = keystore_find(keys, record->signer_id);
    if (entry == NULL) {
        return "signer has no registered key";
    }
    if (strcmp(entry->public_hex, record->signer_pubkey) != 0) {
        return "signer public key is not the key registered for that identity";
    }

    /* Referential integrity between the chain and the registries. */
    if (record->book_id[0] != '\0' && find_book(registry, record->book_id) == -1) {
        return "book ID is not in the book registry";
    }
    if (record->member_id[0] != '\0' && find_member(registry, record->member_id) == -1 &&
        find_librarian(registry, record->member_id) == -1) {
        return "member ID is not in the member registry";
    }

    /* Replay protection: each signer's nonces must run 0, 1, 2, ... along the chain. */
    int s = 0;
    while (s < *signer_count && strcmp(signers[s].id, record->signer_id) != 0) {
        s++;
    }
    if (s == *signer_count) {
        if (s == MAX_MEMBERS + MAX_LIBRARIANS) {
            return "more signers than registered identities";
        }
        strcpy(signers[s].id, record->signer_id);
        signers[s].next = 0;
        (*signer_count)++;
    }
    if (record->nonce != signers[s].next) {
        return "nonce is out of sequence (a replayed or reordered record)";
    }
    signers[s].next++;

    /* A record ID may appear only once. */
    for (int pb = 1; pb <= b; pb++) {
        int limit = pb == b ? r : chain->blocks[pb].record_count;
        for (int pr = 0; pr < limit; pr++) {
            if (strcmp(chain->blocks[pb].records[pr].record_id, record->record_id) == 0) {
                return "record ID was already used by an earlier record";
            }
        }
    }
    return NULL;
}

int validate_chain(const Chain *chain, const Registry *registry, const KeyStore *keys,
                   ChainFault *fault)
{
    /* Every signer is a registered member or librarian, so this is large enough. */
    SignerNonce signers[MAX_MEMBERS + MAX_LIBRARIANS];
    int signer_count = 0;

    if (chain->count < 1) {
        return invalid(fault, 0, -1, "the chain has no genesis block");
    }

    for (int i = 0; i < chain->count; i++) {
        const Block *block = &chain->blocks[i];

        if (block->index != i) {
            return invalid(fault, i, -1, "block index is out of order");
        }
        /* Only the genesis block may be empty, and it must be. */
        if (block->record_count < 0 || block->record_count > BLOCK_CAPACITY ||
            (block->record_count == 0) != (i == 0)) {
            return invalid(fault, i, -1, "block has an invalid number of records");
        }

        Block copy = *block;
        calculate_hash(&copy);
        if (strcmp(block->hash, copy.hash) != 0) {
            return invalid(fault, i, -1, "stored hash does not match the block's contents");
        }

        if (block->difficulty < MIN_DIFFICULTY || block->difficulty > MAX_DIFFICULTY ||
            !hash_meets_difficulty(block->hash, block->difficulty)) {
            return invalid(fault, i, -1, "hash does not meet the proof-of-work difficulty");
        }

        /* Recompute the Merkle root from the records and compare. */
        if (!block_seal_records(&copy) || strcmp(block->merkle_root, copy.merkle_root) != 0) {
            return invalid(fault, i, -1,
                           "Merkle root does not match the records (a record was changed)");
        }
        if (block->token_reward != copy.token_reward ||
            strcmp(block->transaction_id, copy.transaction_id) != 0) {
            return invalid(fault, i, -1, "block reward does not match its return records");
        }

        if (i == 0) {
            if (strspn(block->previous_hash, "0") != 64) {
                return invalid(fault, i, -1, "genesis block must have 64 zeros as previous_hash");
            }
            continue;
        }

        if (strcmp(block->previous_hash, chain->blocks[i - 1].hash) != 0) {
            return invalid(fault, i, -1, "previous_hash does not match the previous block's hash");
        }
        if (block->timestamp <= chain->blocks[i - 1].timestamp) {
            return invalid(fault, i, -1, "timestamp is not later than the previous block's");
        }

        for (int r = 0; r < block->record_count; r++) {
            const char *why = check_record(chain, i, r, registry, keys, signers, &signer_count);
            if (why != NULL) {
                return invalid(fault, i, r, why);
            }
        }
    }

    return 1;
}
