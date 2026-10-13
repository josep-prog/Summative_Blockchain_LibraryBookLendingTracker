#ifndef PENDING_H
#define PENDING_H

#include "blockchain.h"

/*
 * The pending pool: signed requests waiting for a miner.
 * Nothing in the library changes until a request is confirmed in a mined block.
 */

typedef enum { REQ_PENDING, REQ_CONFIRMED, REQ_SUSPICIOUS } RequestStatus;

#define PRIORITY_OVERDUE 0    /* returns, fine settlements and flags on overdue copies */
#define PRIORITY_NORMAL  1

#define FLAG_NAME_SIZE   32
#define FLAG_DETAIL_SIZE 128

typedef struct {
    char request_id[RECORD_ID_SIZE];
    char book_id[ID_SIZE];
    char copy_id[COPY_ID_SIZE];
    char member_id[ID_SIZE];
    char action_requested[ACTION_SIZE];
    int priority;                      /* lower is served first */
    RequestStatus status;

    char flag[FLAG_NAME_SIZE];         /* fraud heuristic that fired, "" if none */
    char flag_detail[FLAG_DETAIL_SIZE];

    LendingRecord record;              /* the signed request, as it will go on the chain */
} PendingRequest;

typedef struct {
    PendingRequest *items;   /* heap array, kept in priority order */
    int count;
    int capacity;
} PendingPool;

void pending_init(PendingPool *pool);
void pending_free(PendingPool *pool);

/* Adds a request and re-sorts the pool; returns 0 if memory ran out. */
int pending_add(PendingPool *pool, const LendingRecord *record, int priority,
                RequestStatus status, const char *flag, const char *flag_detail);

/*
 * Priority order: overdue-related requests first, then the oldest request first
 * (the member who has waited longest), ties kept in arrival order.
 */
void pending_sort(PendingPool *pool);

/* Index of the request, or -1. With suspicious_only, PENDING requests are skipped. */
int pending_find(const PendingPool *pool, const char *request_id, int suspicious_only);
void pending_remove_at(PendingPool *pool, int index);
/* Removes every request marked CONFIRMED; returns how many. */
int pending_remove_confirmed(PendingPool *pool);
int pending_count_status(const PendingPool *pool, RequestStatus status);

const char *request_status_name(RequestStatus status);

#endif
