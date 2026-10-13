#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pending.h"

#define INITIAL_CAPACITY 8

void pending_init(PendingPool *pool)
{
    pool->items = NULL;
    pool->count = 0;
    pool->capacity = 0;
}

void pending_free(PendingPool *pool)
{
    free(pool->items);
    pending_init(pool);
}

const char *request_status_name(RequestStatus status)
{
    switch (status) {
    case REQ_PENDING:    return "PENDING";
    case REQ_CONFIRMED:  return "CONFIRMED";
    case REQ_SUSPICIOUS: return "SUSPICIOUS";
    }
    return "?";
}

/*
 * Returns 1 if a must be served before b.
 * First key: the priority class (PRIORITY_OVERDUE is 0, so it sorts ahead of PRIORITY_NORMAL).
 * Second key: the request time, oldest first, so the member who has waited longest wins a tie.
 */
static int served_before(const PendingRequest *a, const PendingRequest *b)
{
    if (a->priority != b->priority) {
        return a->priority < b->priority;
    }
    /* Strictly less: two requests made in the same second are never swapped. */
    return a->record.timestamp < b->record.timestamp;
}

/*
 * Insertion sort: stable, so equal requests stay in arrival order.
 * The pool is already sorted except for the request just appended, so each call
 * is one pass that shifts later requests right until the new one finds its place.
 * The miner then takes the first BLOCK_CAPACITY PENDING requests from the front.
 */
void pending_sort(PendingPool *pool)
{
    for (int i = 1; i < pool->count; i++) {
        PendingRequest moving = pool->items[i];
        int j = i - 1;

        /* Everything that must wait behind 'moving' moves one slot to the right. */
        while (j >= 0 && served_before(&moving, &pool->items[j])) {
            pool->items[j + 1] = pool->items[j];
            j--;
        }
        pool->items[j + 1] = moving;
    }
}

int pending_add(PendingPool *pool, const LendingRecord *record, int priority,
                RequestStatus status, const char *flag, const char *flag_detail)
{
    /* The array doubles when full, so adding stays cheap however long the pool gets. */
    if (pool->count == pool->capacity) {
        int new_capacity = pool->capacity == 0 ? INITIAL_CAPACITY : pool->capacity * 2;
        PendingRequest *grown = realloc(pool->items, (size_t)new_capacity * sizeof(PendingRequest));
        if (grown == NULL) {
            return 0;
        }
        pool->items = grown;
        pool->capacity = new_capacity;
    }

    PendingRequest *request = &pool->items[pool->count++];
    memset(request, 0, sizeof(*request));
    /* The pool fields the brief lists are copied out of the record; they have the same sizes. */
    strcpy(request->request_id, record->record_id);
    strcpy(request->book_id, record->book_id);
    strcpy(request->copy_id, record->copy_id);
    strcpy(request->member_id, record->member_id);
    strcpy(request->action_requested, record->action);
    request->priority = priority;
    request->status = status;
    snprintf(request->flag, sizeof(request->flag), "%s", flag != NULL ? flag : "");
    snprintf(request->flag_detail, sizeof(request->flag_detail), "%s",
             flag_detail != NULL ? flag_detail : "");
    request->record = *record;

    /* Appended at the end, then moved up to its place in the priority order. */
    pending_sort(pool);
    return 1;
}

int pending_find(const PendingPool *pool, const char *request_id, int suspicious_only)
{
    for (int i = 0; i < pool->count; i++) {
        if (strcmp(pool->items[i].request_id, request_id) == 0 &&
            (!suspicious_only || pool->items[i].status == REQ_SUSPICIOUS)) {
            return i;
        }
    }
    return -1;
}

void pending_remove_at(PendingPool *pool, int index)
{
    if (index < 0 || index >= pool->count) {
        return;
    }
    /* Close the gap; the requests behind keep their order, so the pool stays sorted. */
    memmove(&pool->items[index], &pool->items[index + 1],
            (size_t)(pool->count - index - 1) * sizeof(PendingRequest));
    pool->count--;
}

int pending_remove_confirmed(PendingPool *pool)
{
    int kept = 0;

    /* Compact in place: survivors slide to the front in their existing (sorted) order. */
    for (int i = 0; i < pool->count; i++) {
        if (pool->items[i].status != REQ_CONFIRMED) {
            pool->items[kept++] = pool->items[i];
        }
    }

    int removed = pool->count - kept;
    pool->count = kept;
    return removed;
}

int pending_count_status(const PendingPool *pool, RequestStatus status)
{
    int count = 0;

    for (int i = 0; i < pool->count; i++) {
        if (pool->items[i].status == status) {
            count++;
        }
    }
    return count;
}
