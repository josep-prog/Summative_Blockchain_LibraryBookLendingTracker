#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library.h"

/* Helpers */

static int is_action(const LendingRecord *record, const char *action)
{
    return strcmp(record->action, action) == 0;
}

/* A request still in the pool, pending or held as suspicious. */
static int in_flight(const PendingRequest *request)
{
    return request->status != REQ_CONFIRMED;
}

/* Example: 2 confirmed requests and 1 still in the pool means the next nonce is 3. */
unsigned long lending_next_nonce(const Library *lib, const char *signer_id)
{
    unsigned long next = ledger_nonce(&lib->ledger, signer_id);

    for (int i = 0; i < lib->pool.count; i++) {
        if (in_flight(&lib->pool.items[i]) &&
            strcmp(lib->pool.items[i].record.signer_id, signer_id) == 0) {
            next++;
        }
    }
    return next;
}

long record_spend(const LendingRecord *record)
{
    if (is_action(record, ACTION_TRANSFER) || is_action(record, ACTION_SETTLED)) {
        return record->amount + TX_FEE;
    }
    return 0;
}

/* Actions that move a copy through the state machine. */
static int changes_copy(const char *action)
{
    return strcmp(action, ACTION_BORROWED) == 0 || strcmp(action, ACTION_RETURNED) == 0 ||
           strcmp(action, ACTION_RENEWED) == 0 || strcmp(action, ACTION_LOST) == 0;
}

/* A copy takes one state-changing request at a time: the one found here, if any. */
static const PendingRequest *copy_request_in_flight(const Library *lib, const char *copy_id)
{
    for (int i = 0; i < lib->pool.count; i++) {
        const PendingRequest *request = &lib->pool.items[i];
        if (in_flight(request) && changes_copy(request->action_requested) &&
            strcmp(request->copy_id, copy_id) == 0) {
            return request;
        }
    }
    return NULL;
}

/* Tokens the member's pool requests will spend, and fines they will settle. */
static long pending_spend(const Library *lib, const char *member_id, long *settlements)
{
    long spend = 0;

    if (settlements != NULL) *settlements = 0;
    for (int i = 0; i < lib->pool.count; i++) {
        const PendingRequest *request = &lib->pool.items[i];
        if (!in_flight(request) || strcmp(request->member_id, member_id) != 0) {
            continue;
        }
        spend += record_spend(&request->record);
        if (settlements != NULL && is_action(&request->record, ACTION_SETTLED)) {
            *settlements += request->record.amount;
        }
    }
    return spend;
}

int lending_copies_held(const Library *lib, const char *member_id)
{
    int held = 0;

    for (int i = 0; i < lib->inventory.copy_count; i++) {
        const Copy *copy = &lib->inventory.copies[i];
        if (copy->state == COPY_BORROWED && strcmp(copy->current_holder_id, member_id) == 0) {
            held++;
        }
    }
    return held;
}

/* A copy of the title the member has on loan or on hold, or NULL. */
static const Copy *copy_held_of_title(const Library *lib, const char *book_id,
                                      const char *member_id)
{
    for (int i = 0; i < lib->inventory.copy_count; i++) {
        const Copy *copy = &lib->inventory.copies[i];
        if (strcmp(copy->book_id, book_id) == 0 &&
            (copy->state == COPY_BORROWED || copy->state == COPY_RESERVED) &&
            strcmp(copy->current_holder_id, member_id) == 0) {
            return copy;
        }
    }
    return NULL;
}

static int reason_code_ok(const char *reason)
{
    size_t len = strlen(reason);

    if (len < 2 || len >= REASON_SIZE) {
        return 0;
    }
    for (const char *p = reason; *p != '\0'; p++) {
        if (!(*p >= 'A' && *p <= 'Z') && !(*p >= '0' && *p <= '9') && *p != '_') {
            return 0;
        }
    }
    return 1;
}

/* Validation */

/* FAIL(...) writes the reason into 'why' and returns 0 from request_check(). */
#define FAIL(...) do { snprintf(why, why_size, __VA_ARGS__); return 0; } while (0)

int request_check(const Library *lib, const LendingRecord *record, int at_mining,
                  char *why, size_t why_size)
{
    const Registry *registry = &lib->registry;
    const Inventory *inventory = &lib->inventory;
    const char *member = record->member_id;
    char amount_a[32], amount_b[32];

    int by_librarian = is_action(record, ACTION_OVERDUE) || is_action(record, ACTION_LOST);
    int token_action = is_action(record, ACTION_SETTLED) || is_action(record, ACTION_TRANSFER);
    int names_copy = changes_copy(record->action) || is_action(record, ACTION_OVERDUE);

    if (!record_action_known(record->action)) {
        FAIL("unknown action '%s'", record->action);
    }

    /* Who may sign what. */
    if (keystore_find(&lib->keys, record->signer_id) == NULL) {
        FAIL("%s has no registered key", record->signer_id);
    }
    if (by_librarian && find_librarian(registry, record->signer_id) == -1) {
        FAIL("only a librarian can sign a %s record", record->action);
    }
    if (!by_librarian && strcmp(record->signer_id, member) != 0) {
        FAIL("a %s request must be signed by the member it concerns", record->action);
    }

    /* Referential integrity with the registries. */
    int member_index = find_member(registry, member);
    if (is_action(record, ACTION_TRANSFER)) {
        if (member_index == -1 && find_librarian(registry, member) == -1) {
            FAIL("sender %s is not a registered member or librarian", member);
        }
    } else if (member_index == -1 && !(is_action(record, ACTION_LOST) && member[0] == '\0')) {
        FAIL("member %s is not in the member registry", member);
    }
    if (!token_action && find_book(registry, record->book_id) == -1) {
        FAIL("book %s is not in the book registry", record->book_id);
    }

    Copy *copy = NULL;
    if (names_copy) {
        copy = inventory_find_copy(inventory, record->copy_id);
        if (copy == NULL || strcmp(copy->book_id, record->book_id) != 0) {
            FAIL("copy %s is not a copy of book %s", record->copy_id, record->book_id);
        }
    }

    /* One state-changing request per copy at a time. */
    if (!at_mining && changes_copy(record->action)) {
        const PendingRequest *other = copy_request_in_flight(lib, record->copy_id);
        if (other != NULL) {
            FAIL("copy %s already has a %s request in the pool (%s) - mine the pool first",
                 record->copy_id, other->action_requested, other->request_id);
        }
    }

    if (is_action(record, ACTION_BORROWED)) {
        if (copy->state == COPY_RESERVED) {
            if (strcmp(copy->current_holder_id, member) != 0) {
                FAIL("copy %s is reserved for %s", copy->copy_id, copy->current_holder_id);
            }
        } else if (!copy_transition_allowed(copy->state, COPY_BORROWED)) {
            FAIL("copy %s is %s, so it cannot be borrowed", copy->copy_id,
                 copy_state_name(copy->state));
        }
        if (record->due_date <= record->timestamp) {
            FAIL("the due date is in the past");
        }
        if (record->due_date > record->timestamp + MAX_LOAN_DAYS * DAY_SECONDS) {
            FAIL("the due date is more than %d days away", MAX_LOAN_DAYS);
        }

    } else if (is_action(record, ACTION_RETURNED)) {
        int condition = parse_condition(record->condition);

        if (copy->state != COPY_BORROWED || strcmp(copy->current_holder_id, member) != 0) {
            FAIL("copy %s is not on loan to %s", copy->copy_id, member);
        }
        if (record->due_date != copy->due_date) {
            FAIL("the due date does not match the loan");
        }
        if (condition < 0 || condition == COND_LOST) {
            FAIL("condition must be NEW, GOOD or WORN (use mark_lost for a lost copy)");
        }
        if (condition < (int)copy->condition) {
            FAIL("a copy in %s condition cannot come back as %s",
                 condition_name(copy->condition), record->condition);
        }
        if (record->reason[0] != '\0' && !reason_code_ok(record->reason)) {
            FAIL("a reason code is 2-%d characters of A-Z, 0-9 and _", REASON_SIZE - 1);
        }

    } else if (is_action(record, ACTION_RENEWED)) {
        if (copy->state != COPY_BORROWED || strcmp(copy->current_holder_id, member) != 0) {
            FAIL("copy %s is not on loan to %s", copy->copy_id, member);
        }
        if (record->timestamp > copy->due_date) {
            FAIL("copy %s is overdue - an overdue loan cannot be renewed", copy->copy_id);
        }
        if (copy->renewals >= MAX_RENEWALS) {
            FAIL("copy %s was already renewed the maximum %d times", copy->copy_id, MAX_RENEWALS);
        }
        if (queue_length(inventory, record->book_id) > 0) {
            FAIL("another member has reserved this title, so it cannot be renewed");
        }
        for (int i = 0; !at_mining && i < lib->pool.count; i++) {
            const PendingRequest *other = &lib->pool.items[i];
            if (in_flight(other) && strcmp(other->action_requested, ACTION_RESERVED) == 0 &&
                strcmp(other->book_id, record->book_id) == 0) {
                FAIL("a reservation for this title is pending, so it cannot be renewed");
            }
        }
        if (record->due_date != copy->due_date + RENEWAL_DAYS * DAY_SECONDS) {
            FAIL("a renewal extends the due date by exactly %d days", RENEWAL_DAYS);
        }

    } else if (is_action(record, ACTION_RESERVED)) {
        const Copy *held = copy_held_of_title(lib, record->book_id, member);

        if (inventory_copies_of(inventory, record->book_id) ==
            inventory_count(inventory, record->book_id, COPY_LOST)) {
            FAIL("book %s has no copies that can be lent", record->book_id);
        }
        if (held != NULL) {
            FAIL("%s already has copy %s of this title %s", member, held->copy_id,
                 held->state == COPY_BORROWED ? "on loan" : "on hold");
        }
        if (queue_position(inventory, record->book_id, member) > 0) {
            FAIL("%s is already in the reservation queue for this title", member);
        }
        /* A copy can become free between the request and mining; it is then offered at once. */
        for (int i = 0; !at_mining && i < inventory->copy_count; i++) {
            const Copy *other = &inventory->copies[i];
            if (strcmp(other->book_id, record->book_id) == 0 && other->state == COPY_AVAILABLE &&
                copy_request_in_flight(lib, other->copy_id) == NULL) {
                FAIL("copy %s is available - borrow it instead of reserving", other->copy_id);
            }
        }
        for (int i = 0; !at_mining && i < lib->pool.count; i++) {
            const PendingRequest *other = &lib->pool.items[i];
            if (in_flight(other) && strcmp(other->action_requested, ACTION_RESERVED) == 0 &&
                strcmp(other->book_id, record->book_id) == 0 &&
                strcmp(other->member_id, member) == 0) {
                FAIL("%s already has a reservation for this title in the pool", member);
            }
        }

    } else if (is_action(record, ACTION_OVERDUE)) {
        /* By mining time the copy may already be back; the flag is still a true record. */
        if (!at_mining && (copy->state != COPY_BORROWED ||
                           strcmp(copy->current_holder_id, member) != 0 ||
                           record->due_date != copy->due_date)) {
            FAIL("copy %s is not an overdue loan of %s", copy->copy_id, member);
        }

    } else if (is_action(record, ACTION_LOST)) {
        if (!copy_transition_allowed(copy->state, COPY_LOST)) {
            FAIL("copy %s is %s and cannot be marked LOST", copy->copy_id,
                 copy_state_name(copy->state));
        }
        if (!reason_code_ok(record->reason)) {
            FAIL("marking a copy LOST needs a reason code (2-%d characters of A-Z, 0-9 and _)",
                 REASON_SIZE - 1);
        }

    } else if (token_action) {
        long settlements = 0;
        long reserved = at_mining ? 0 : pending_spend(lib, member, &settlements);
        long available = ledger_balance(&lib->ledger, member) - reserved;

        if (record->amount <= 0) {
            FAIL("the amount must be positive");
        }
        if (is_action(record, ACTION_SETTLED)) {
            long owed = registry->members[member_index].fines_owed - settlements;
            if (record->amount > owed) {
                FAIL("%s owes only %s in fines", member, format_coins(owed, amount_a));
            }
        } else {
            if (ledger_find_account(&lib->ledger, record->peer_id) == NULL) {
                FAIL("recipient account %s does not exist", record->peer_id);
            }
            if (strcmp(record->peer_id, member) == 0) {
                FAIL("%s cannot send tokens to itself", member);
            }
        }
        if (available < record->amount + TX_FEE) {
            FAIL("insufficient tokens: %s has %s available, needs %s (amount + fee)", member,
                 format_coins(available, amount_a),
                 format_coins(record->amount + TX_FEE, amount_b));
        }
    }

    return 1;
}

/* Building and submitting requests */

static int id_in_use(const Library *lib, const char *record_id)
{
    return chain_find_record(&lib->chain, record_id) != NULL ||
           pending_find(&lib->pool, record_id, 0) != -1;
}

/* Starts a request: time, signer identity and nonce. submit() gives it its ID. */
static void begin_record(Library *lib, LendingRecord *record, const char *action,
                         const char *signer_id, const RequestOptions *options)
{
    const KeyEntry *key = keystore_find(&lib->keys, signer_id);

    memset(record, 0, sizeof(*record));
    strcpy(record->action, action);
    record->timestamp = library_now(lib);

    if (options != NULL && options->request_id != NULL) {
        snprintf(record->record_id, sizeof(record->record_id), "%s", options->request_id);
    }

    snprintf(record->signer_id, sizeof(record->signer_id), "%s", signer_id);
    if (key != NULL) {
        strcpy(record->signer_pubkey, key->public_hex);
    }
    record->nonce = (options != NULL && options->has_nonce)
                    ? options->nonce : lending_next_nonce(lib, signer_id);
    if (options != NULL && options->reason != NULL) {
        snprintf(record->reason, sizeof(record->reason), "%s", options->reason);
    }
}

static void set_book(LendingRecord *record, const Book *book, const char *copy_id)
{
    strcpy(record->book_id, book->book_id);
    strcpy(record->book_title, book->title);    /* copied at the time of the transaction */
    if (copy_id != NULL) {
        strcpy(record->copy_id, copy_id);
    }
}

static void set_member(LendingRecord *record, const Member *member)
{
    strcpy(record->member_id, member->member_id);
    strcpy(record->member_name, member->full_name);
}

/* Checks, signs and queues a request. */
static int submit(Library *lib, LendingRecord *record, int priority)
{
    char why[220];
    const char *reason;
    int generated_id = 0;
    unsigned long expected = lending_next_nonce(lib, record->signer_id);

    /* Replay protection: only the signer's next nonce is accepted. */
    if (record->nonce != expected) {
        printf("REJECTED: bad nonce for %s: got %lu, expected %lu (reused or out of order).\n",
               record->signer_id, record->nonce, expected);
        return 0;
    }
    if (!request_check(lib, record, 0, why, sizeof(why))) {
        printf("REJECTED: %s.\n", why);
        return 0;
    }

    /* A request that passed the checks gets the next free ID, unless one was given. */
    while (record->record_id[0] == '\0' || (generated_id && id_in_use(lib, record->record_id))) {
        snprintf(record->record_id, sizeof(record->record_id), "REQ-%06lu",
                 lib->next_request_seq++);
        generated_id = 1;
    }

    if (record->token_reward > 0) {
        record_reward_tx_id(record, record->tx_id);
    }

    /* Sign with the signer's private key before the request enters the pool. */
    unsigned char data[RECORD_DATA_SIZE];
    size_t data_len = record_signed_data(record, data);
    if (!keystore_sign(&lib->keys, record->signer_id, data, data_len,
                       record->signature, &record->signature_length)) {
        printf("ERROR: Could not sign the %s request with %s's private key.\n",
               record->action, record->signer_id);
        return 0;
    }
    if (!record_verify(record, &reason)) {
        printf("REJECTED: %s.\n", reason);
        return 0;
    }

    /* Misuse heuristics: a flagged request is held in the pool, not mined. */
    FraudFlag flag;
    int suspicious = fraud_check(lib, record, &flag);

    if (!pending_add(&lib->pool, record, priority, suspicious ? REQ_SUSPICIOUS : REQ_PENDING,
                     suspicious ? flag.heuristic : NULL, suspicious ? flag.detail : NULL)) {
        printf("ERROR: out of memory - request not queued.\n");
        return 0;
    }

    printf("%s request %s signed by %s (nonce %lu) entered the pending pool.\n",
           record->action, record->record_id, record->signer_id, record->nonce);
    if (suspicious) {
        printf("  FLAGGED SUSPICIOUS [%s]: %s.\n", flag.heuristic, flag.detail);
        printf("  It will not be mined until a librarian runs approve_suspicious %s "
               "(or reject_suspicious %s).\n", record->record_id, record->record_id);
    } else if (record->token_reward > 0) {
        printf("  Reward transaction created: %d coins to %s (TX %.16s...), paid when mined.\n",
               record->token_reward, record->member_id, record->tx_id);
    }
    return 1;
}

static int unknown_ids(const Library *lib, const char *book_id, const char *member_id)
{
    int missing = 0;

    if (book_id != NULL && find_book(&lib->registry, book_id) == -1) {
        printf("ERROR: Book ID '%s' is not in the book registry.\n", book_id);
        missing = 1;
    }
    if (member_id != NULL && find_member(&lib->registry, member_id) == -1) {
        printf("ERROR: Member ID '%s' is not in the member registry.\n", member_id);
        missing = 1;
    }
    return missing;
}

int lending_borrow(Library *lib, const char *book_id, const char *member_id,
                   const RequestOptions *options)
{
    if (unknown_ids(lib, book_id, member_id)) {
        return 0;
    }
    const Book *book = &lib->registry.books[find_book(&lib->registry, book_id)];
    const Member *member = &lib->registry.members[find_member(&lib->registry, member_id)];

    /* A copy held for this member comes first, then any free copy. */
    const Copy *chosen = NULL;
    for (int pass = 0; pass < 2 && chosen == NULL; pass++) {
        for (int i = 0; i < lib->inventory.copy_count && chosen == NULL; i++) {
            const Copy *copy = &lib->inventory.copies[i];
            if (strcmp(copy->book_id, book_id) != 0 ||
                copy_request_in_flight(lib, copy->copy_id) != NULL) {
                continue;
            }
            if (pass == 0 ? (copy->state == COPY_RESERVED &&
                             strcmp(copy->current_holder_id, member_id) == 0)
                          : copy->state == COPY_AVAILABLE) {
                chosen = copy;
            }
        }
    }

    if (chosen == NULL) {
        printf("No copy of '%s' is available (%d of %d on loan, %d on hold, %d waiting in the queue).\n",
               book->title, inventory_count(&lib->inventory, book_id, COPY_BORROWED),
               inventory_copies_of(&lib->inventory, book_id),
               inventory_count(&lib->inventory, book_id, COPY_RESERVED),
               queue_length(&lib->inventory, book_id));
        printf("Use: reserve_book %s %s\n", book_id, member_id);
        return 0;
    }

    LendingRecord record;
    begin_record(lib, &record, ACTION_BORROWED, member_id, options);
    set_book(&record, book, chosen->copy_id);
    set_member(&record, member);
    record.due_date = (options != NULL && options->due_date != 0)
                      ? options->due_date : record.timestamp + LOAN_DAYS * DAY_SECONDS;

    if (!submit(lib, &record, PRIORITY_NORMAL)) {
        return 0;
    }

    char due[20];
    printf("  Copy %s of '%s' for %s, due %s.\n", record.copy_id, book->title,
           member->full_name, format_time(record.due_date, due));
    return 1;
}

int lending_return(Library *lib, const char *copy_id, const RequestOptions *options)
{
    const Copy *copy = inventory_find_copy(&lib->inventory, copy_id);
    if (copy == NULL) {
        printf("ERROR: Copy ID '%s' is not in the copy inventory.\n", copy_id);
        return 0;
    }

    /* The loan being closed is the copy's most recent BORROWED record on the chain. */
    const LendingRecord *loan = chain_last_record(&lib->chain, copy_id, ACTION_BORROWED);
    if (loan == NULL || copy->state != COPY_BORROWED) {
        const PendingRequest *pending = copy_request_in_flight(lib, copy_id);
        if (pending != NULL && strcmp(pending->action_requested, ACTION_BORROWED) == 0) {
            printf("ERROR: The borrow of copy %s is still in the pending pool - mine it first.\n",
                   copy_id);
        } else {
            printf("ERROR: Copy %s has no open BORROWED record on the chain (state: %s).\n",
                   copy_id, copy_state_name(copy->state));
        }
        return 0;
    }

    int book_index = find_book(&lib->registry, copy->book_id);
    int member_index = find_member(&lib->registry, copy->current_holder_id);
    if (book_index == -1 || member_index == -1) {
        printf("ERROR: The loan of copy %s refers to a book or member missing from the registry.\n",
               copy_id);
        return 0;
    }

    LendingRecord record;
    begin_record(lib, &record, ACTION_RETURNED, copy->current_holder_id, options);
    set_book(&record, &lib->registry.books[book_index], copy_id);
    set_member(&record, &lib->registry.members[member_index]);
    record.due_date = copy->due_date;
    snprintf(record.condition, sizeof(record.condition), "%s",
             (options != NULL && options->condition != NULL) ? options->condition
                                                             : condition_name(copy->condition));

    /* Late relative to the due date: a smaller reward and a fine per day. */
    int late = record.timestamp > record.due_date;
    record.fine_amount = overdue_fine(record.due_date, record.timestamp);
    record.token_reward = late ? REWARD_LATE : REWARD_ON_TIME;

    if (!submit(lib, &record, late ? PRIORITY_OVERDUE : PRIORITY_NORMAL)) {
        return 0;
    }

    char fine[32];
    if (late) {
        printf("  Return is LATE by %ld day(s): fine %s coins, reward %d coins.\n",
               record.fine_amount / FINE_PER_DAY, format_coins(record.fine_amount, fine),
               REWARD_LATE);
    } else {
        printf("  Return is ON TIME: no fine, reward %d coins.\n", REWARD_ON_TIME);
    }
    return 1;
}

int lending_renew(Library *lib, const char *copy_id, const RequestOptions *options)
{
    const Copy *copy = inventory_find_copy(&lib->inventory, copy_id);
    if (copy == NULL) {
        printf("ERROR: Copy ID '%s' is not in the copy inventory.\n", copy_id);
        return 0;
    }
    if (copy->state != COPY_BORROWED) {
        printf("ERROR: Copy %s is not on loan (state: %s).\n", copy_id,
               copy_state_name(copy->state));
        return 0;
    }

    int book_index = find_book(&lib->registry, copy->book_id);
    int member_index = find_member(&lib->registry, copy->current_holder_id);
    if (book_index == -1 || member_index == -1) {
        printf("ERROR: The loan of copy %s refers to a book or member missing from the registry.\n",
               copy_id);
        return 0;
    }

    LendingRecord record;
    begin_record(lib, &record, ACTION_RENEWED, copy->current_holder_id, options);
    set_book(&record, &lib->registry.books[book_index], copy_id);
    set_member(&record, &lib->registry.members[member_index]);
    record.due_date = copy->due_date + RENEWAL_DAYS * DAY_SECONDS;

    if (!submit(lib, &record, PRIORITY_NORMAL)) {
        return 0;
    }

    char due[20];
    printf("  Renewal %d of %d: new due date %s once mined.\n", copy->renewals + 1,
           MAX_RENEWALS, format_time(record.due_date, due));
    return 1;
}

int lending_reserve(Library *lib, const char *book_id, const char *member_id,
                    const RequestOptions *options)
{
    if (unknown_ids(lib, book_id, member_id)) {
        return 0;
    }

    LendingRecord record;
    begin_record(lib, &record, ACTION_RESERVED, member_id, options);
    set_book(&record, &lib->registry.books[find_book(&lib->registry, book_id)], NULL);
    set_member(&record, &lib->registry.members[find_member(&lib->registry, member_id)]);

    if (!submit(lib, &record, PRIORITY_NORMAL)) {
        return 0;
    }
    printf("  %s joins the reservation queue for '%s' once mined (%d already waiting).\n",
           record.member_name, record.book_title, queue_length(&lib->inventory, book_id));
    return 1;
}

int lending_mark_lost(Library *lib, const char *copy_id, const RequestOptions *options)
{
    const Copy *copy = inventory_find_copy(&lib->inventory, copy_id);
    if (copy == NULL) {
        printf("ERROR: Copy ID '%s' is not in the copy inventory.\n", copy_id);
        return 0;
    }

    int book_index = find_book(&lib->registry, copy->book_id);
    if (book_index == -1) {
        printf("ERROR: Copy %s refers to a book missing from the registry.\n", copy_id);
        return 0;
    }

    /* A librarian override, signed with the librarian's own key. */
    LendingRecord record;
    begin_record(lib, &record, ACTION_LOST, lib->user->librarian_id, options);
    set_book(&record, &lib->registry.books[book_index], copy_id);

    int holder = copy->state == COPY_BORROWED
                 ? find_member(&lib->registry, copy->current_holder_id) : -1;
    if (holder != -1) {
        set_member(&record, &lib->registry.members[holder]);
    }

    return submit(lib, &record, PRIORITY_NORMAL);
}

int lending_settle_fine(Library *lib, const char *member_id, long amount,
                        const RequestOptions *options)
{
    if (unknown_ids(lib, NULL, member_id)) {
        return 0;
    }
    const Member *member = &lib->registry.members[find_member(&lib->registry, member_id)];

    long settlements = 0;
    pending_spend(lib, member_id, &settlements);
    long owed = member->fines_owed - settlements;

    if (owed <= 0) {
        printf("%s has no fines left to settle.\n", member_id);
        return 0;
    }

    LendingRecord record;
    begin_record(lib, &record, ACTION_SETTLED, member_id, options);
    set_member(&record, member);
    strcpy(record.peer_id, FEE_ACCOUNT);
    record.amount = amount > 0 ? amount : owed;     /* no amount given: settle everything */

    return submit(lib, &record, PRIORITY_OVERDUE);
}

int lending_transfer(Library *lib, const char *sender, const char *recipient, long amount,
                     const RequestOptions *options)
{
    int member_index = find_member(&lib->registry, sender);
    int librarian_index = find_librarian(&lib->registry, sender);

    if (member_index == -1 && librarian_index == -1) {
        printf("ERROR: Sender '%s' is not a registered member or librarian.\n", sender);
        return 0;
    }
    if (strlen(recipient) >= ID_SIZE) {
        printf("ERROR: Recipient account '%s' does not exist.\n", recipient);
        return 0;
    }

    LendingRecord record;
    begin_record(lib, &record, ACTION_TRANSFER, sender, options);
    strcpy(record.member_id, sender);
    strcpy(record.member_name, member_index != -1
           ? lib->registry.members[member_index].full_name
           : lib->registry.librarians[librarian_index].full_name);
    strcpy(record.peer_id, recipient);
    record.amount = amount;

    return submit(lib, &record, PRIORITY_NORMAL);
}

int lending_scan_overdue(Library *lib)
{
    time_t now = library_now(lib);
    int flagged = 0;

    for (int i = 0; i < lib->inventory.copy_count; i++) {
        Copy *copy = &lib->inventory.copies[i];
        if (copy->state != COPY_BORROWED || copy->overdue_flagged || now <= copy->due_date) {
            continue;
        }

        int book_index = find_book(&lib->registry, copy->book_id);
        int member_index = find_member(&lib->registry, copy->current_holder_id);
        if (book_index == -1 || member_index == -1) {
            continue;
        }

        /* Not returned, so no reward transaction: only the fine is recorded. */
        LendingRecord record;
        begin_record(lib, &record, ACTION_OVERDUE, lib->user->librarian_id, NULL);
        set_book(&record, &lib->registry.books[book_index], copy->copy_id);
        set_member(&record, &lib->registry.members[member_index]);
        record.due_date = copy->due_date;
        record.fine_amount = overdue_fine(copy->due_date, now);

        char fine[32];
        printf("OVERDUE: copy %s held by %s is %ld day(s) late (fine so far %s coins).\n",
               copy->copy_id, copy->current_holder_id, record.fine_amount / FINE_PER_DAY,
               format_coins(record.fine_amount, fine));
        if (!submit(lib, &record, PRIORITY_OVERDUE)) {
            break;
        }

        copy->overdue_flagged = 1;     /* one OVERDUE record per loan */
        flagged++;
    }
    return flagged;
}

/* Applying confirmed records */

/* Hands a free copy to the member who has waited longest, or makes it generally available. */
static void release_copy(Library *lib, Copy *copy)
{
    char next_member[ID_SIZE];

    copy->current_holder_id[0] = '\0';
    copy->due_date = 0;
    copy->renewals = 0;
    copy->overdue_flagged = 0;

    if (queue_pop(&lib->inventory, copy->book_id, next_member)) {
        copy_set_state(copy, COPY_RESERVED);
        strcpy(copy->current_holder_id, next_member);
        printf("  -> copy %s is RESERVED: offered to %s, next in the reservation queue.\n",
               copy->copy_id, next_member);
    } else {
        copy_set_state(copy, COPY_AVAILABLE);
        printf("  -> copy %s is AVAILABLE again.\n", copy->copy_id);
    }
}

/* Offers copies that are free to members waiting in the title's queue. */
static void offer_free_copies(Library *lib, const char *book_id)
{
    for (int i = 0; i < lib->inventory.copy_count; i++) {
        Copy *copy = &lib->inventory.copies[i];
        if (queue_length(&lib->inventory, book_id) == 0) {
            return;
        }
        if (strcmp(copy->book_id, book_id) == 0 && copy->state == COPY_AVAILABLE) {
            release_copy(lib, copy);
        }
    }
}

void lending_apply(Library *lib, const LendingRecord *record)
{
    Copy *copy = inventory_find_copy(&lib->inventory, record->copy_id);
    int member_index = find_member(&lib->registry, record->member_id);
    Member *member = member_index != -1 ? &lib->registry.members[member_index] : NULL;
    char err[200], text[32], owed[32], when[20];

    if (is_action(record, ACTION_BORROWED) && copy != NULL) {
        if (copy_set_state(copy, COPY_BORROWED)) {
            strcpy(copy->current_holder_id, record->member_id);
            copy->due_date = record->due_date;
            copy->renewals = 0;
            copy->overdue_flagged = 0;
            printf("  -> copy %s is BORROWED by %s, due %s.\n", copy->copy_id,
                   record->member_id, format_time(copy->due_date, when));
        }

    } else if (is_action(record, ACTION_RETURNED) && copy != NULL) {
        copy->condition = (Condition)parse_condition(record->condition);

        /* The reward exists only now that the return is confirmed. */
        if (ledger_mint(&lib->ledger, record->tx_id, record->member_id,
                        record->token_reward * COIN, TX_FEE, err, sizeof(err))) {
            printf("  -> reward TX confirmed: %d coins - %s fee credited to %s.\n",
                   record->token_reward, format_coins(TX_FEE, text), record->member_id);
        } else {
            printf("WARNING: reward for %s not applied: %s.\n", record->record_id, err);
        }
        if (record->fine_amount > 0 && member != NULL) {
            member->fines_owed += record->fine_amount;
            printf("  -> fine of %s coins assessed on %s.\n",
                   format_coins(record->fine_amount, text), record->member_id);
        }
        release_copy(lib, copy);

    } else if (is_action(record, ACTION_RENEWED) && copy != NULL) {
        copy->due_date = record->due_date;
        copy->renewals++;
        printf("  -> copy %s renewed (%d of %d), now due %s.\n", copy->copy_id, copy->renewals,
               MAX_RENEWALS, format_time(copy->due_date, when));

    } else if (is_action(record, ACTION_RESERVED)) {
        if (queue_push(&lib->inventory, record->book_id, record->member_id, record->timestamp)) {
            printf("  -> %s is number %d in the reservation queue for %s.\n", record->member_id,
                   queue_length(&lib->inventory, record->book_id), record->book_id);
        }
        offer_free_copies(lib, record->book_id);

    } else if (is_action(record, ACTION_OVERDUE) && copy != NULL) {
        printf("  -> copy %s recorded OVERDUE for %s: no token transaction.\n", copy->copy_id,
               record->member_id);

    } else if (is_action(record, ACTION_LOST) && copy != NULL) {
        if (copy_set_state(copy, COPY_LOST)) {
            copy->condition = COND_LOST;
            copy->current_holder_id[0] = '\0';
            copy->due_date = 0;
            copy->renewals = 0;
            copy->overdue_flagged = 0;
            printf("  -> copy %s is LOST (reason %s).\n", copy->copy_id, record->reason);
        }

    } else if (is_action(record, ACTION_SETTLED) || is_action(record, ACTION_TRANSFER)) {
        Transaction tx;

        if (!ledger_transfer(&lib->ledger, record->member_id, record->peer_id, record->amount,
                             record->nonce, &tx, err, sizeof(err))) {
            printf("WARNING: tokens of %s not moved: %s.\n", record->record_id, err);
        } else {
            ledger_print_transaction(&tx);
            if (is_action(record, ACTION_SETTLED) && member != NULL) {
                member->fines_owed -= record->amount;
                member->fines_paid += record->amount;
                printf("  -> %s settled %s coins of fines (%s still owed).\n", record->member_id,
                       format_coins(record->amount, text),
                       format_coins(member->fines_owed, owed));
            }
        }
    }

    /* The signer's nonce moves only now, on confirmation. */
    ledger_bump_nonce(&lib->ledger, record->signer_id);
}

/* Dropping requests */

static void forget_request(Library *lib, const PendingRequest *request)
{
    /* A dropped OVERDUE flag may be raised again by the next scan. */
    if (strcmp(request->action_requested, ACTION_OVERDUE) == 0) {
        Copy *copy = inventory_find_copy(&lib->inventory, request->copy_id);
        if (copy != NULL) {
            copy->overdue_flagged = 0;
        }
    }
}

void lending_drop_request(Library *lib, int index, const char *why)
{
    if (index < 0 || index >= lib->pool.count) {
        return;
    }

    PendingRequest dropped = lib->pool.items[index];
    printf("DROPPED %s request %s of %s: %s.\n", dropped.action_requested, dropped.request_id,
           dropped.record.signer_id, why);
    forget_request(lib, &dropped);
    pending_remove_at(&lib->pool, index);

    /* The signer's later requests carry nonces that can never become valid now. */
    for (int i = 0; i < lib->pool.count; ) {
        const PendingRequest *other = &lib->pool.items[i];
        if (in_flight(other) && strcmp(other->record.signer_id, dropped.record.signer_id) == 0 &&
            other->record.nonce > dropped.record.nonce) {
            printf("DROPPED %s request %s of %s: its nonce %lu follows the dropped request.\n",
                   other->action_requested, other->request_id, other->record.signer_id,
                   other->record.nonce);
            forget_request(lib, other);
            pending_remove_at(&lib->pool, i);
        } else {
            i++;
        }
    }
}
