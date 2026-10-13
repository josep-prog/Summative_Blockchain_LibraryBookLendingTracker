#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#include <stdio.h>
#include <time.h>
#include <stddef.h>

#include "config.h"
#include "crypto.h"
#include "keystore.h"
#include "registry.h"
#include "inventory.h"

#define RECORD_ID_SIZE   24
#define ACTION_SIZE      10
#define CONDITION_SIZE   8
#define REASON_SIZE      20
#define RECORD_DATA_SIZE 768   /* longest signed text of one record */

/* Lending actions. */
#define ACTION_BORROWED "BORROWED"
#define ACTION_RETURNED "RETURNED"
#define ACTION_RESERVED "RESERVED"
#define ACTION_RENEWED  "RENEWED"
#define ACTION_OVERDUE  "OVERDUE"
/* Extensions, so that these also go through the pending pool and mining. */
#define ACTION_LOST     "LOST"       /* librarian override: copy marked lost */
#define ACTION_SETTLED  "SETTLED"    /* fine paid in tokens to the library */
#define ACTION_TRANSFER "TRANSFER"   /* manual token transfer between accounts */

/* One lending record (transaction). Everything except the signature is signed. */
typedef struct {
    char record_id[RECORD_ID_SIZE];
    time_t timestamp;                  /* when the request was made */
    char action[ACTION_SIZE];

    char book_id[ID_SIZE];
    char copy_id[COPY_ID_SIZE];
    char book_title[TITLE_SIZE];       /* copied from the book registry */

    char member_id[ID_SIZE];
    char member_name[NAME_SIZE];       /* copied from the member registry */
    char peer_id[ID_SIZE];             /* TRANSFER: the recipient */

    time_t due_date;
    long fine_amount;                  /* 0 unless a late RETURNED or an OVERDUE record */
    long amount;                       /* tokens moved by SETTLED and TRANSFER */
    char condition[CONDITION_SIZE];    /* RETURNED: condition of the copy */
    char reason[REASON_SIZE];          /* librarian override reason code */

    int token_reward;                  /* RETURNED: 10 on time, 5 late */
    char tx_id[HASH_HEX_SIZE];         /* SHA-256 of the reward transaction, "" if none */

    unsigned long nonce;               /* signer's counter: stops replays */
    char signer_id[ID_SIZE];
    char signer_pubkey[PUBKEY_HEX_SIZE];   /* the signer's identity on the chain */

    unsigned char signature[SIGNATURE_MAX];    /* ECDSA over the fields above */
    unsigned int signature_length;
} LendingRecord;

typedef struct {
    int index;                         /* 0 = genesis */
    time_t timestamp;
    int record_count;
    char previous_hash[HASH_HEX_SIZE];
    char merkle_root[HASH_HEX_SIZE];   /* over the hashes of the records */
    char hash[HASH_HEX_SIZE];
    char sealed_by[ID_SIZE];           /* miner that sealed the block */
    int token_reward;                  /* coins credited for this block's returns */
    char transaction_id[HASH_HEX_SIZE];    /* SHA-256 over the block's reward transactions */

    int difficulty;                    /* leading zeros */
    unsigned long nonce;               /* proof-of-work counter */

    LendingRecord records[BLOCK_CAPACITY];
} Block;

typedef struct {
    Block *blocks;     /* heap array */
    int count;
    int capacity;
} Chain;

/* Where and why validation failed; record is -1 for a fault in the block itself. */
typedef struct {
    int block;
    int record;
    char reason[160];
} ChainFault;

/* Records */

int record_action_known(const char *action);
/* The signed fields as text; returns the length. */
size_t record_signed_data(const LendingRecord *record, unsigned char data[RECORD_DATA_SIZE]);
/* Leaf of the Merkle tree: SHA-256 of the signed fields and the signature. */
void record_hash(const LendingRecord *record, unsigned char digest[HASH_BYTES]);
/* Id of the reward transaction. */
void record_reward_tx_id(const LendingRecord *record, char out_hex[HASH_HEX_SIZE]);
/* Fine for returning at 'when' a loan that was due at 'due_date'. */
long overdue_fine(time_t due_date, time_t when);
/* Checks the reward, fine and amount rules and the ECDSA signature. */
int record_verify(const LendingRecord *record, const char **reason);
void print_record(const LendingRecord *record);

/* Blocks */

/* Genesis is mined too. */
void create_genesis_block(Block *block, int difficulty, time_t now);
/* Fills merkle_root, token_reward and transaction_id from the block's records. */
int block_seal_records(Block *block);
int block_merkle_root(const Block *block, char out_hex[HASH_HEX_SIZE], FILE *trace);
void calculate_hash(Block *block);
int hash_meets_difficulty(const char *hash, int difficulty);
/* Tries up to max_attempts nonces; returns 1 when a valid hash is found. */
int mine_attempts(Block *block, unsigned long max_attempts, unsigned long *attempts_used);
/* With show_merkle, the levels of the Merkle tree are printed too. */
void print_block(const Block *block, int show_merkle);

/* Chain */

void chain_init(Chain *chain);
void chain_free(Chain *chain);
int chain_append(Chain *chain, const Block *block);

/*
 * Full validation: index order, previous-hash linkage, recomputed block hash,
 * proof of work, strictly increasing timestamps, Merkle root, reward summary,
 * and for every record its ECDSA signature, the signer's registered key,
 * registry references, unique record ID and nonce sequence.
 */
int validate_chain(const Chain *chain, const Registry *registry, const KeyStore *keys,
                   ChainFault *fault);

/* Newest confirmed record of this copy with this action, or NULL. */
const LendingRecord *chain_last_record(const Chain *chain, const char *copy_id,
                                       const char *action);
const LendingRecord *chain_find_record(const Chain *chain, const char *record_id);

/* Time as "YYYY-MM-DD HH:MM", or "-" for 0. */
const char *format_time(time_t when, char buf[20]);

#endif
