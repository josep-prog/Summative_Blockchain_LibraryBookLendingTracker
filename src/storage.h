#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>

#include "library.h"

/*
 * Disk persistence in one plain-text file, one record per line, fields
 * separated by '|'. The first field names the kind of line:
 *
 *   LIBRARY-LEDGER|version
 *   META|difficulty|block_reward|model|clock_offset|next_request_seq|mint_sequence|chain_height
 *   BLOCK|index|timestamp|record_count|previous_hash|merkle_root|sealed_by|token_reward|
 *         transaction_id|difficulty|nonce|hash
 *   RECORD|<record>                       (record_count of these follow their BLOCK)
 *   COPY|copy_id|book_id|condition|state|current_holder_id|due_date|renewals|overdue_flagged
 *   QUEUE|book_id|member_id|reservation_date          (in queue order)
 *   POOL|status|priority|flag|flag_detail|<record>    (in pool order)
 *   FINES|member_id|fines_owed|fines_paid
 *   ACCOUNT|id|balance|nonce
 *   UTXO|tx_id|output_index|owner|amount
 *   HISTORY|account|tx_id|sender|recipient|amount|fee|nonce|has_nonce
 *   SEAL|sha256_of_all_lines_above|librarian_id|ecdsa_signature_hex
 *
 *   <record> = record_id|timestamp|action|book_id|copy_id|book_title|member_id|
 *              member_name|peer_id|due_date|fine_amount|amount|condition|reason|
 *              token_reward|tx_id|nonce|signer_id|signer_pubkey|signature_hex
 *
 * The file is written to a temporary name and renamed, so a crash cannot leave
 * half a file. The SEAL line lets the next start detect any edit made outside
 * the program, including to balances and inventory that are not on the chain.
 */

typedef enum { LOAD_OK, LOAD_MISSING, LOAD_CORRUPT } LoadStatus;

int storage_exists(const char *filename);

/* Returns 0 and fills err on failure. */
int storage_save(const Library *lib, char *err, size_t err_size);

/*
 * Restores the chain, inventory states, queues, pool, fines and ledger into lib,
 * which must hold freshly loaded registries and copies. seal_ok tells whether the
 * seal matched; if it did not, err says why.
 */
LoadStatus storage_load(Library *lib, int *seal_ok, char *err, size_t err_size);

#endif
