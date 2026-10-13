#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <openssl/evp.h>

#include "storage.h"

#define FORMAT_NAME    "LIBRARY-LEDGER"
#define FORMAT_VERSION 1
#define LINE_SIZE      2048
#define SIGNATURE_HEX_SIZE (SIGNATURE_MAX * 2 + 1)

int storage_exists(const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

/* Writing */

/* Every line written also goes into the running SHA-256 of the file. */
typedef struct {
    FILE *file;
    EVP_MD_CTX *digest;
} Writer;

static void put(Writer *writer, const char *format, ...)
{
    char line[LINE_SIZE];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    fputs(line, writer->file);
    EVP_DigestUpdate(writer->digest, line, strlen(line));
}

/* <record> as one string: the signed fields, then the signature in hex. */
static void record_to_text(const LendingRecord *record, char text[LINE_SIZE])
{
    unsigned char data[RECORD_DATA_SIZE];
    char signature_hex[SIGNATURE_HEX_SIZE];
    unsigned int signature_len = record->signature_length <= SIGNATURE_MAX
                                 ? record->signature_length : SIGNATURE_MAX;

    record_signed_data(record, data);
    bytes_to_hex(record->signature, signature_len, signature_hex);
    snprintf(text, LINE_SIZE, "%s|%s", (const char *)data, signature_hex);
}

static void digest_hex(EVP_MD_CTX *digest, char out_hex[HASH_HEX_SIZE])
{
    unsigned char raw[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    EVP_DigestFinal_ex(digest, raw, &len);
    bytes_to_hex(raw, HASH_BYTES, out_hex);
}

int storage_save(const Library *lib, char *err, size_t err_size)
{
    char temp_name[256];
    char text[LINE_SIZE];
    Writer writer;

    snprintf(temp_name, sizeof(temp_name), "%s.tmp", lib->data_file);
    writer.file = fopen(temp_name, "w");
    writer.digest = EVP_MD_CTX_new();
    if (writer.file == NULL || writer.digest == NULL ||
        EVP_DigestInit_ex(writer.digest, EVP_sha256(), NULL) != 1) {
        if (writer.file != NULL) fclose(writer.file);
        EVP_MD_CTX_free(writer.digest);
        snprintf(err, err_size, "could not open %s for writing", temp_name);
        return 0;
    }

    put(&writer, "%s|%d\n", FORMAT_NAME, FORMAT_VERSION);
    put(&writer, "META|%d|%d|%s|%ld|%lu|%lu|%d\n", lib->difficulty, BLOCK_REWARD,
        lib->ledger.model == MODEL_UTXO ? "UTXO" : "ACCOUNT", lib->clock_offset,
        lib->next_request_seq, lib->ledger.mint_sequence, lib->chain.count);

    /* The full blockchain: every block followed by its lending records. */
    for (int b = 0; b < lib->chain.count; b++) {
        const Block *block = &lib->chain.blocks[b];
        put(&writer, "BLOCK|%d|%ld|%d|%s|%s|%s|%d|%s|%d|%lu|%s\n", block->index,
            (long)block->timestamp, block->record_count, block->previous_hash,
            block->merkle_root, block->sealed_by, block->token_reward, block->transaction_id,
            block->difficulty, block->nonce, block->hash);
        for (int r = 0; r < block->record_count; r++) {
            record_to_text(&block->records[r], text);
            put(&writer, "RECORD|%s\n", text);
        }
    }

    /* Copy inventory and reservation queues. */
    for (int i = 0; i < lib->inventory.copy_count; i++) {
        const Copy *copy = &lib->inventory.copies[i];
        put(&writer, "COPY|%s|%s|%s|%s|%s|%ld|%d|%d\n", copy->copy_id, copy->book_id,
            condition_name(copy->condition), copy_state_name(copy->state),
            copy->current_holder_id, (long)copy->due_date, copy->renewals,
            copy->overdue_flagged);
    }
    for (int q = 0; q < lib->inventory.queue_count; q++) {
        const ReservationQueue *queue = &lib->inventory.queues[q];
        for (const ReservationNode *node = queue->head; node != NULL; node = node->next) {
            put(&writer, "QUEUE|%s|%s|%ld\n", queue->book_id, node->member_id,
                (long)node->reservation_date);
        }
    }

    /* Pending pool. */
    for (int i = 0; i < lib->pool.count; i++) {
        const PendingRequest *request = &lib->pool.items[i];
        record_to_text(&request->record, text);
        put(&writer, "POOL|%s|%d|%s|%s|%s\n", request_status_name(request->status),
            request->priority, request->flag, request->flag_detail, text);
    }

    /* Fines, then token balances or the UTXO set, nonces and histories. */
    for (int i = 0; i < lib->registry.member_count; i++) {
        const Member *member = &lib->registry.members[i];
        if (member->fines_owed != 0 || member->fines_paid != 0) {
            put(&writer, "FINES|%s|%ld|%ld\n", member->member_id, member->fines_owed,
                member->fines_paid);
        }
    }
    for (int i = 0; i < lib->ledger.account_count; i++) {
        const Account *account = &lib->ledger.accounts[i];
        put(&writer, "ACCOUNT|%s|%ld|%lu\n", account->id, account->balance, account->nonce);
    }
    for (int i = 0; i < lib->ledger.utxo_count; i++) {
        const Utxo *utxo = &lib->ledger.utxos[i];
        put(&writer, "UTXO|%s|%d|%s|%ld\n", utxo->tx_id, utxo->output_index, utxo->owner,
            utxo->amount);
    }
    for (int i = 0; i < lib->ledger.account_count; i++) {
        const Account *account = &lib->ledger.accounts[i];
        for (const HistoryNode *node = account->history_head; node != NULL; node = node->next) {
            put(&writer, "HISTORY|%s|%s|%s|%s|%ld|%ld|%lu|%d\n", account->id, node->tx_id,
                node->sender, node->recipient, node->amount, node->fee, node->nonce,
                node->has_nonce);
        }
    }

    /* Seal: the librarian signs the hash of everything written above. */
    char hash[HASH_HEX_SIZE];
    unsigned char signature[SIGNATURE_MAX];
    unsigned int signature_len = 0;
    char signature_hex[SIGNATURE_HEX_SIZE] = "";

    digest_hex(writer.digest, hash);
    EVP_MD_CTX_free(writer.digest);

    int sealed = lib->user != NULL &&
                 keystore_sign(&lib->keys, lib->user->librarian_id,
                               (const unsigned char *)hash, strlen(hash),
                               signature, &signature_len);
    if (sealed) {
        bytes_to_hex(signature, signature_len, signature_hex);
        fprintf(writer.file, "SEAL|%s|%s|%s\n", hash, lib->user->librarian_id, signature_hex);
    }

    int written = !ferror(writer.file);
    if (fclose(writer.file) != 0) {
        written = 0;
    }
    if (!sealed || !written) {
        remove(temp_name);
        snprintf(err, err_size, "%s", sealed ? "write error" : "could not sign the file seal");
        return 0;
    }

    /* Write a temp file, then rename, so a crash cannot break the saved state. */
    if (rename(temp_name, lib->data_file) != 0) {
        snprintf(err, err_size, "could not replace %s", lib->data_file);
        return 0;
    }
    return 1;
}

/* Reading */

/* Cuts the next '|'-separated field off the line and moves the cursor past it. */
static char *next_field(char **cursor)
{
    char *start = *cursor;
    if (start == NULL) {
        return NULL;
    }

    char *bar = strchr(start, '|');
    if (bar != NULL) {
        *bar = '\0';
        *cursor = bar + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

/* Takes the next field as text that must fit in size bytes. */
static int take_text(char **cursor, char *out, size_t size)
{
    const char *field = next_field(cursor);

    if (field == NULL || strlen(field) >= size) {
        return 0;
    }
    strcpy(out, field);
    return 1;
}

static int take_long(char **cursor, long *out)
{
    const char *field = next_field(cursor);
    char *end;

    if (field == NULL || field[0] == '\0') {
        return 0;
    }
    *out = strtol(field, &end, 10);
    return *end == '\0';
}

static int take_ulong(char **cursor, unsigned long *out)
{
    const char *field = next_field(cursor);
    char *end;

    if (field == NULL || field[0] == '\0' || field[0] == '-') {
        return 0;
    }
    *out = strtoul(field, &end, 10);
    return *end == '\0';
}

static int take_int(char **cursor, int *out)
{
    long value;

    if (!take_long(cursor, &value)) {
        return 0;
    }
    *out = (int)value;
    return 1;
}

static int take_time(char **cursor, time_t *out)
{
    long value;

    if (!take_long(cursor, &value)) {
        return 0;
    }
    *out = (time_t)value;
    return 1;
}

static int parse_record(char **cursor, LendingRecord *record)
{
    char signature_hex[SIGNATURE_HEX_SIZE];
    size_t signature_len = 0;

    memset(record, 0, sizeof(*record));
    if (!take_text(cursor, record->record_id, sizeof(record->record_id)) ||
        !take_time(cursor, &record->timestamp) ||
        !take_text(cursor, record->action, sizeof(record->action)) ||
        !take_text(cursor, record->book_id, sizeof(record->book_id)) ||
        !take_text(cursor, record->copy_id, sizeof(record->copy_id)) ||
        !take_text(cursor, record->book_title, sizeof(record->book_title)) ||
        !take_text(cursor, record->member_id, sizeof(record->member_id)) ||
        !take_text(cursor, record->member_name, sizeof(record->member_name)) ||
        !take_text(cursor, record->peer_id, sizeof(record->peer_id)) ||
        !take_time(cursor, &record->due_date) ||
        !take_long(cursor, &record->fine_amount) ||
        !take_long(cursor, &record->amount) ||
        !take_text(cursor, record->condition, sizeof(record->condition)) ||
        !take_text(cursor, record->reason, sizeof(record->reason)) ||
        !take_int(cursor, &record->token_reward) ||
        !take_text(cursor, record->tx_id, sizeof(record->tx_id)) ||
        !take_ulong(cursor, &record->nonce) ||
        !take_text(cursor, record->signer_id, sizeof(record->signer_id)) ||
        !take_text(cursor, record->signer_pubkey, sizeof(record->signer_pubkey)) ||
        !take_text(cursor, signature_hex, sizeof(signature_hex)) ||
        *cursor != NULL ||
        !hex_to_bytes(signature_hex, record->signature, SIGNATURE_MAX, &signature_len)) {
        return 0;
    }
    record->signature_length = (unsigned int)signature_len;
    return 1;
}

static int parse_meta(char **cursor, Library *lib)
{
    char model[12];
    int block_reward, height;

    if (!take_int(cursor, &lib->difficulty) || !take_int(cursor, &block_reward) ||
        !take_text(cursor, model, sizeof(model)) || !take_long(cursor, &lib->clock_offset) ||
        !take_ulong(cursor, &lib->next_request_seq) ||
        !take_ulong(cursor, &lib->ledger.mint_sequence) || !take_int(cursor, &height)) {
        return 0;
    }
    if (strcmp(model, "UTXO") != 0 && strcmp(model, "ACCOUNT") != 0) {
        return 0;
    }
    lib->ledger.model = strcmp(model, "UTXO") == 0 ? MODEL_UTXO : MODEL_ACCOUNT;
    return lib->difficulty >= MIN_DIFFICULTY && lib->difficulty <= MAX_DIFFICULTY;
}

static int parse_block(char **cursor, Block *block)
{
    memset(block, 0, sizeof(*block));
    return take_int(cursor, &block->index) && take_time(cursor, &block->timestamp) &&
           take_int(cursor, &block->record_count) &&
           take_text(cursor, block->previous_hash, sizeof(block->previous_hash)) &&
           take_text(cursor, block->merkle_root, sizeof(block->merkle_root)) &&
           take_text(cursor, block->sealed_by, sizeof(block->sealed_by)) &&
           take_int(cursor, &block->token_reward) &&
           take_text(cursor, block->transaction_id, sizeof(block->transaction_id)) &&
           take_int(cursor, &block->difficulty) && take_ulong(cursor, &block->nonce) &&
           take_text(cursor, block->hash, sizeof(block->hash)) &&
           block->record_count >= 0 && block->record_count <= BLOCK_CAPACITY;
}

static int parse_copy(char **cursor, Library *lib)
{
    char copy_id[COPY_ID_SIZE], book_id[ID_SIZE], condition_text[12], state_text[12];
    char holder[ID_SIZE];
    time_t due_date;
    int renewals, flagged;

    if (!take_text(cursor, copy_id, sizeof(copy_id)) || !take_text(cursor, book_id, sizeof(book_id)) ||
        !take_text(cursor, condition_text, sizeof(condition_text)) ||
        !take_text(cursor, state_text, sizeof(state_text)) ||
        !take_text(cursor, holder, sizeof(holder)) || !take_time(cursor, &due_date) ||
        !take_int(cursor, &renewals) || !take_int(cursor, &flagged)) {
        return 0;
    }

    int condition = parse_condition(condition_text);
    int state = parse_copy_state(state_text);
    if (condition < 0 || state < 0) {
        return 0;
    }

    /* Normally the copy is already there from copies.txt. */
    Copy *copy = inventory_find_copy(&lib->inventory, copy_id);
    if (copy == NULL) {
        copy = inventory_add_copy(&lib->inventory, copy_id, book_id, (Condition)condition);
        if (copy == NULL) {
            return 0;
        }
    }
    copy->condition = (Condition)condition;
    copy->state = (CopyState)state;
    strcpy(copy->current_holder_id, holder);
    copy->due_date = due_date;
    copy->renewals = renewals;
    copy->overdue_flagged = flagged;
    return 1;
}

static int parse_queue(char **cursor, Library *lib)
{
    char book_id[ID_SIZE], member_id[ID_SIZE];
    time_t date;

    return take_text(cursor, book_id, sizeof(book_id)) &&
           take_text(cursor, member_id, sizeof(member_id)) && take_time(cursor, &date) &&
           queue_push(&lib->inventory, book_id, member_id, date);
}

static int parse_pool(char **cursor, Library *lib)
{
    char status_text[12], flag[FLAG_NAME_SIZE], detail[FLAG_DETAIL_SIZE];
    int priority;
    LendingRecord record;

    if (!take_text(cursor, status_text, sizeof(status_text)) || !take_int(cursor, &priority) ||
        !take_text(cursor, flag, sizeof(flag)) || !take_text(cursor, detail, sizeof(detail)) ||
        !parse_record(cursor, &record)) {
        return 0;
    }

    RequestStatus status;
    if (strcmp(status_text, "PENDING") == 0) {
        status = REQ_PENDING;
    } else if (strcmp(status_text, "SUSPICIOUS") == 0) {
        status = REQ_SUSPICIOUS;
    } else {
        return 0;
    }
    return pending_add(&lib->pool, &record, priority, status, flag, detail);
}

static int parse_fines(char **cursor, Library *lib)
{
    char member_id[ID_SIZE];
    long owed, paid;

    if (!take_text(cursor, member_id, sizeof(member_id)) || !take_long(cursor, &owed) ||
        !take_long(cursor, &paid)) {
        return 0;
    }

    int index = find_member(&lib->registry, member_id);
    if (index == -1 || owed < 0 || paid < 0) {
        return 0;
    }
    lib->registry.members[index].fines_owed = owed;
    lib->registry.members[index].fines_paid = paid;
    return 1;
}

static int parse_account(char **cursor, Library *lib)
{
    char id[ACCOUNT_ID_SIZE];
    long balance;
    unsigned long nonce;

    if (!take_text(cursor, id, sizeof(id)) || !take_long(cursor, &balance) ||
        !take_ulong(cursor, &nonce) || balance < 0 || !ledger_open_account(&lib->ledger, id)) {
        return 0;
    }

    Account *account = ledger_find_account(&lib->ledger, id);
    account->balance = balance;
    account->nonce = nonce;
    return 1;
}

static int parse_utxo(char **cursor, Library *lib)
{
    Utxo utxo;

    memset(&utxo, 0, sizeof(utxo));
    return take_text(cursor, utxo.tx_id, sizeof(utxo.tx_id)) &&
           take_int(cursor, &utxo.output_index) &&
           take_text(cursor, utxo.owner, sizeof(utxo.owner)) &&
           take_long(cursor, &utxo.amount) && utxo.amount > 0 &&
           ledger_restore_utxo(&lib->ledger, &utxo);
}

static int parse_history(char **cursor, Library *lib)
{
    char account[ACCOUNT_ID_SIZE];
    HistoryNode node;

    memset(&node, 0, sizeof(node));
    return take_text(cursor, account, sizeof(account)) &&
           take_text(cursor, node.tx_id, sizeof(node.tx_id)) &&
           take_text(cursor, node.sender, sizeof(node.sender)) &&
           take_text(cursor, node.recipient, sizeof(node.recipient)) &&
           take_long(cursor, &node.amount) && take_long(cursor, &node.fee) &&
           take_ulong(cursor, &node.nonce) && take_int(cursor, &node.has_nonce) &&
           ledger_restore_history(&lib->ledger, account, &node);
}

/* Checks the SEAL line against the hash of the lines read and the signer's key. */
static int check_seal(const Library *lib, char *seal_fields, const char *computed_hash,
                      char *err, size_t err_size)
{
    char hash[HASH_HEX_SIZE], signer[ID_SIZE], signature_hex[SIGNATURE_HEX_SIZE];
    unsigned char signature[SIGNATURE_MAX];
    size_t signature_len = 0;
    char *cursor = seal_fields;

    if (!take_text(&cursor, hash, sizeof(hash)) || !take_text(&cursor, signer, sizeof(signer)) ||
        !take_text(&cursor, signature_hex, sizeof(signature_hex)) ||
        !hex_to_bytes(signature_hex, signature, sizeof(signature), &signature_len)) {
        snprintf(err, err_size, "the SEAL line is malformed");
        return 0;
    }
    if (strcmp(hash, computed_hash) != 0) {
        snprintf(err, err_size, "the file was changed after it was sealed (hash mismatch)");
        return 0;
    }

    const KeyEntry *key = keystore_find(&lib->keys, signer);
    if (key == NULL || find_librarian(&lib->registry, signer) == -1) {
        snprintf(err, err_size, "the seal was signed by '%s', who is not a registered librarian",
                 signer);
        return 0;
    }
    if (!verify_signature(key->public_key, (const unsigned char *)hash, strlen(hash),
                          signature, signature_len)) {
        snprintf(err, err_size, "the seal's signature by %s is invalid", signer);
        return 0;
    }
    return 1;
}

LoadStatus storage_load(Library *lib, int *seal_ok, char *err, size_t err_size)
{
    FILE *file = fopen(lib->data_file, "r");
    if (file == NULL) {
        return LOAD_MISSING;
    }

    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    if (digest == NULL || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(digest);
        fclose(file);
        snprintf(err, err_size, "out of memory");
        return LOAD_CORRUPT;
    }

    char line[LINE_SIZE];
    char seal_fields[LINE_SIZE] = "";
    int line_no = 0;
    int have_header = 0, have_meta = 0, have_seal = 0;
    int records_expected = 0;       /* records still to come for the last block */
    const char *problem = NULL;

    while (problem == NULL && fgets(line, sizeof(line), file) != NULL) {
        line_no++;

        if (have_seal) {
            problem = "there is data after the SEAL line";
            break;
        }
        if (strncmp(line, "SEAL|", 5) == 0) {
            line[strcspn(line, "\r\n")] = '\0';
            strcpy(seal_fields, line + 5);
            have_seal = 1;
            continue;
        }

        EVP_DigestUpdate(digest, line, strlen(line));
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        char *cursor = line;
        const char *tag = next_field(&cursor);
        int ok;

        if (!have_header) {
            ok = strcmp(tag, FORMAT_NAME) == 0 && cursor != NULL && atoi(cursor) == FORMAT_VERSION;
            have_header = 1;
            if (!ok) problem = "this is not a version 1 library ledger file";
            continue;
        }

        /* A RECORD line fills the next empty slot of the BLOCK line before it. */
        if (strcmp(tag, "RECORD") == 0) {
            Block *block = lib->chain.count > 0 ? &lib->chain.blocks[lib->chain.count - 1] : NULL;
            ok = block != NULL && records_expected > 0 &&
                 parse_record(&cursor, &block->records[block->record_count - records_expected]);
            if (ok) records_expected--;
        } else if (records_expected > 0) {
            ok = 0;                 /* a block is missing some of its records */
        } else if (strcmp(tag, "META") == 0) {
            ok = have_meta = parse_meta(&cursor, lib);
        } else if (strcmp(tag, "BLOCK") == 0) {
            Block block;
            ok = parse_block(&cursor, &block) && chain_append(&lib->chain, &block);
            if (ok) records_expected = block.record_count;
        } else if (strcmp(tag, "COPY") == 0) {
            ok = parse_copy(&cursor, lib);
        } else if (strcmp(tag, "QUEUE") == 0) {
            ok = parse_queue(&cursor, lib);
        } else if (strcmp(tag, "POOL") == 0) {
            ok = parse_pool(&cursor, lib);
        } else if (strcmp(tag, "FINES") == 0) {
            ok = parse_fines(&cursor, lib);
        } else if (strcmp(tag, "ACCOUNT") == 0) {
            ok = parse_account(&cursor, lib);
        } else if (strcmp(tag, "UTXO") == 0) {
            ok = parse_utxo(&cursor, lib);
        } else if (strcmp(tag, "HISTORY") == 0) {
            ok = parse_history(&cursor, lib);
        } else {
            ok = 0;
        }

        if (!ok) problem = "a line is malformed";
    }
    fclose(file);

    char computed_hash[HASH_HEX_SIZE];
    digest_hex(digest, computed_hash);
    EVP_MD_CTX_free(digest);

    if (problem == NULL && (!have_meta || lib->chain.count == 0 || records_expected > 0)) {
        problem = "the file is incomplete";
        line_no = 0;
    }
    if (problem != NULL) {
        if (line_no > 0) {
            snprintf(err, err_size, "line %d: %s", line_no, problem);
        } else {
            snprintf(err, err_size, "%s", problem);
        }
        return LOAD_CORRUPT;
    }

    if (!have_seal) {
        snprintf(err, err_size, "the file has no SEAL line");
        *seal_ok = 0;
    } else {
        *seal_ok = check_seal(lib, seal_fields, computed_hash, err, err_size);
    }
    return LOAD_OK;
}
