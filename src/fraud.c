#include <stdio.h>
#include <string.h>

#include "library.h"

/*
 * Misuse and fraud heuristics. A request that trips one is not rejected: it is
 * held in the pending pool as SUSPICIOUS until a librarian approves or rejects it.
 */

static int is_action(const LendingRecord *record, const char *action)
{
    return strcmp(record->action, action) == 0;
}

static int flag_it(FraudFlag *flag, const char *heuristic)
{
    flag->heuristic = heuristic;
    return 1;
}

/* When the member last returned a copy of this title: chain first, then the pool. */
static time_t last_return_of_title(const Library *lib, const LendingRecord *request)
{
    time_t latest = 0;

    for (int b = 1; b < lib->chain.count; b++) {
        const Block *block = &lib->chain.blocks[b];
        for (int r = 0; r < block->record_count; r++) {
            const LendingRecord *record = &block->records[r];
            if (is_action(record, ACTION_RETURNED) &&
                strcmp(record->book_id, request->book_id) == 0 &&
                strcmp(record->member_id, request->member_id) == 0 &&
                record->timestamp > latest) {
                latest = record->timestamp;
            }
        }
    }
    for (int i = 0; i < lib->pool.count; i++) {
        const LendingRecord *record = &lib->pool.items[i].record;
        if (is_action(record, ACTION_RETURNED) &&
            strcmp(record->book_id, request->book_id) == 0 &&
            strcmp(record->member_id, request->member_id) == 0 &&
            record->timestamp > latest) {
            latest = record->timestamp;
        }
    }
    return latest;
}

int fraud_check(const Library *lib, const LendingRecord *record, FraudFlag *flag)
{
    flag->heuristic = NULL;
    flag->detail[0] = '\0';

    /* Duplicate request: the same request ID is already on the chain or in the pool. */
    if (chain_find_record(&lib->chain, record->record_id) != NULL ||
        pending_find(&lib->pool, record->record_id, 0) != -1) {
        snprintf(flag->detail, sizeof(flag->detail),
                 "request ID %s already exists on the chain or in the pending pool",
                 record->record_id);
        return flag_it(flag, "DUPLICATE_REQUEST");
    }

    if (is_action(record, ACTION_BORROWED)) {
        /* Excessive concurrent borrows: copies held plus borrows already in the pool. */
        int holding = lending_copies_held(lib, record->member_id);
        for (int i = 0; i < lib->pool.count; i++) {
            const PendingRequest *other = &lib->pool.items[i];
            if (other->status != REQ_CONFIRMED &&
                strcmp(other->action_requested, ACTION_BORROWED) == 0 &&
                strcmp(other->member_id, record->member_id) == 0) {
                holding++;
            }
        }
        if (holding + 1 > MAX_CONCURRENT_BORROWS) {
            snprintf(flag->detail, sizeof(flag->detail),
                     "%s would hold %d copies at once (maximum %d)", record->member_id,
                     holding + 1, MAX_CONCURRENT_BORROWS);
            return flag_it(flag, "EXCESSIVE_BORROWS");
        }

        /* Rapid re-borrow: the same title again within 24 hours of returning it. */
        time_t returned = last_return_of_title(lib, record);
        if (returned != 0 && record->timestamp - returned < REBORROW_WINDOW_SECONDS) {
            snprintf(flag->detail, sizeof(flag->detail),
                     "%s returned %s only %ld minute(s) ago and is borrowing it again",
                     record->member_id, record->book_id,
                     (long)(record->timestamp - returned) / 60);
            return flag_it(flag, "RAPID_REBORROW");
        }
    }

    if (is_action(record, ACTION_RETURNED)) {
        /* Condition manipulation: down more than one level with no librarian reason code. */
        const Copy *copy = inventory_find_copy(&lib->inventory, record->copy_id);
        int returned_as = parse_condition(record->condition);

        /* Conditions are numbered NEW 0, GOOD 1, WORN 2, so NEW to WORN is a drop of 2. */
        if (copy != NULL && returned_as - (int)copy->condition > 1 && record->reason[0] == '\0') {
            snprintf(flag->detail, sizeof(flag->detail),
                     "copy %s went from %s to %s with no librarian reason code", copy->copy_id,
                     condition_name(copy->condition), record->condition);
            return flag_it(flag, "CONDITION_MANIPULATION");
        }
    }

    return 0;
}
