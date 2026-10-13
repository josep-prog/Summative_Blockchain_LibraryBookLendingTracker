#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "inventory.h"

/* Indexed by the CopyState and Condition enums: the order here must match inventory.h. */
static const char *const STATE_NAMES[] = { "AVAILABLE", "BORROWED", "RESERVED", "LOST" };
static const char *const CONDITION_NAMES[] = { "NEW", "GOOD", "WORN", "LOST" };

const char *copy_state_name(CopyState state)
{
    return STATE_NAMES[state];
}

const char *condition_name(Condition condition)
{
    return CONDITION_NAMES[condition];
}

/* Index of text in names, which is also the enum value, or -1. */
static int parse_name(const char *text, const char *const names[], int count)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(text, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

int parse_condition(const char *text)
{
    return parse_name(text, CONDITION_NAMES, 4);
}

int parse_copy_state(const char *text)
{
    return parse_name(text, STATE_NAMES, 4);
}

int copy_id_has_format(const char *copy_id)
{
    char book_id[COPY_ID_SIZE];
    /* The last dash separates the book ID from the copy number: BK001-C2. */
    const char *dash = strrchr(copy_id, '-');

    if (dash == NULL || strlen(copy_id) >= COPY_ID_SIZE || dash[1] != 'C') {
        return 0;
    }

    size_t digits = strlen(dash + 2);
    if (digits < 1 || digits > 3) {
        return 0;
    }
    for (const char *p = dash + 2; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }

    /* What is left of the dash must itself be a well-formed book ID. */
    memcpy(book_id, copy_id, (size_t)(dash - copy_id));
    book_id[dash - copy_id] = '\0';
    return id_has_format(book_id, BOOK_ID_PREFIX);
}

/*
 * The availability state machine as a lookup table: TRANSITIONS[from][to] is 1
 * when a copy may move from one state to the other. Every state change in the
 * program goes through copy_set_state, so a move that is 0 here cannot happen.
 *
 *   AVAILABLE -> BORROWED   a borrow is confirmed
 *   AVAILABLE -> RESERVED   a free copy is put on hold for a queued member
 *   BORROWED  -> AVAILABLE  a return is confirmed and nobody is queued
 *   BORROWED  -> RESERVED   a return is confirmed and the title has a queue
 *   RESERVED  -> BORROWED   the member the copy is held for borrows it
 *   AVAILABLE or BORROWED -> LOST   librarian override with a reason code
 *
 * LOST is final, and a RESERVED copy can only go to the member it is held for.
 */
static const int TRANSITIONS[4][4] = {
    /* to:           AVAILABLE BORROWED RESERVED LOST */
    /* AVAILABLE */ { 0,        1,       1,       1 },
    /* BORROWED  */ { 1,        0,       1,       1 },
    /* RESERVED  */ { 0,        1,       0,       0 },
    /* LOST      */ { 0,        0,       0,       0 }
};

int copy_transition_allowed(CopyState from, CopyState to)
{
    return TRANSITIONS[from][to];
}

int copy_set_state(Copy *copy, CopyState to)
{
    if (!copy_transition_allowed(copy->state, to)) {
        return 0;
    }
    copy->state = to;
    return 1;
}

void inventory_init(Inventory *inventory)
{
    memset(inventory, 0, sizeof(*inventory));
}

void inventory_free(Inventory *inventory)
{
    /* Each queue is a linked list: free it node by node before the arrays. */
    for (int i = 0; i < inventory->queue_count; i++) {
        ReservationNode *node = inventory->queues[i].head;
        while (node != NULL) {
            ReservationNode *next = node->next;
            free(node);
            node = next;
        }
    }
    free(inventory->queues);
    free(inventory->copies);
    memset(inventory, 0, sizeof(*inventory));
}

Copy *inventory_find_copy(const Inventory *inventory, const char *copy_id)
{
    for (int i = 0; i < inventory->copy_count; i++) {
        if (strcmp(inventory->copies[i].copy_id, copy_id) == 0) {
            return &inventory->copies[i];
        }
    }
    return NULL;
}

Copy *inventory_add_copy(Inventory *inventory, const char *copy_id, const char *book_id,
                         Condition condition)
{
    if (inventory_find_copy(inventory, copy_id) != NULL) {
        return NULL;
    }

    /* The array doubles when full. Callers must not keep a Copy pointer across an add. */
    if (inventory->copy_count == inventory->copy_capacity) {
        int new_capacity = inventory->copy_capacity == 0 ? 16 : inventory->copy_capacity * 2;
        Copy *grown = realloc(inventory->copies, (size_t)new_capacity * sizeof(Copy));
        if (grown == NULL) {
            return NULL;
        }
        inventory->copies = grown;
        inventory->copy_capacity = new_capacity;
    }

    Copy *copy = &inventory->copies[inventory->copy_count++];
    memset(copy, 0, sizeof(*copy));
    snprintf(copy->copy_id, sizeof(copy->copy_id), "%s", copy_id);
    snprintf(copy->book_id, sizeof(copy->book_id), "%s", book_id);
    copy->condition = condition;
    /* A copy recorded as lost in copies.txt starts out LOST. */
    copy->state = condition == COND_LOST ? COPY_LOST : COPY_AVAILABLE;
    return copy;
}

int inventory_load_copies(Inventory *inventory, const char *filename, const Registry *registry)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        printf("ERROR: Could not open '%s'. Copy inventory is required to start the system.\n",
               filename);
        return 0;
    }

    char line[CSV_LINE_SIZE];
    char *fields[3];
    int line_no = 0;

    /* A bad line is reported and skipped, so one typo does not stop the library from opening. */
    while (csv_next_record(file, filename, &line_no, line, fields, 3)) {
        int condition = parse_condition(fields[2]);

        if (!copy_id_has_format(fields[0])) {
            printf("WARNING: %s line %d: copy ID '%s' must look like BK001-C1 - skipped.\n",
                   filename, line_no, fields[0]);
            continue;
        }
        /*
         * Referential integrity: a copy must belong to a registered title, and
         * its ID must start with that title's ID followed by a dash.
         */
        if (find_book(registry, fields[1]) == -1 ||
            strncmp(fields[0], fields[1], strlen(fields[1])) != 0 ||
            fields[0][strlen(fields[1])] != '-') {
            printf("WARNING: %s line %d: copy %s does not belong to a registered book - skipped.\n",
                   filename, line_no, fields[0]);
            continue;
        }
        if (condition < 0) {
            printf("WARNING: %s line %d: condition must be NEW, GOOD, WORN or LOST - skipped.\n",
                   filename, line_no);
            continue;
        }
        if (inventory_find_copy(inventory, fields[0]) != NULL) {
            printf("WARNING: %s line %d: duplicate copy ID %s - skipped.\n",
                   filename, line_no, fields[0]);
            continue;
        }
        if (inventory_add_copy(inventory, fields[0], fields[1], (Condition)condition) == NULL) {
            printf("ERROR: out of memory while loading %s.\n", filename);
            break;
        }
    }
    fclose(file);

    if (inventory->copy_count == 0) {
        printf("ERROR: '%s' is empty or badly formatted. No copies loaded.\n", filename);
    }
    return inventory->copy_count;
}

int inventory_count(const Inventory *inventory, const char *book_id, CopyState state)
{
    int count = 0;

    for (int i = 0; i < inventory->copy_count; i++) {
        if (inventory->copies[i].state == state &&
            strcmp(inventory->copies[i].book_id, book_id) == 0) {
            count++;
        }
    }
    return count;
}

int inventory_copies_of(const Inventory *inventory, const char *book_id)
{
    int count = 0;

    for (int i = 0; i < inventory->copy_count; i++) {
        if (strcmp(inventory->copies[i].book_id, book_id) == 0) {
            count++;
        }
    }
    return count;
}

/*
 * Reservation queues: one first-in-first-out linked list per title, created the
 * first time somebody reserves it. head is the member who has waited longest.
 */

static ReservationQueue *find_queue(const Inventory *inventory, const char *book_id)
{
    for (int i = 0; i < inventory->queue_count; i++) {
        if (strcmp(inventory->queues[i].book_id, book_id) == 0) {
            return &inventory->queues[i];
        }
    }
    return NULL;
}

const ReservationQueue *queue_find(const Inventory *inventory, const char *book_id)
{
    return find_queue(inventory, book_id);
}

int queue_length(const Inventory *inventory, const char *book_id)
{
    const ReservationQueue *queue = find_queue(inventory, book_id);
    return queue != NULL ? queue->length : 0;
}

int queue_position(const Inventory *inventory, const char *book_id, const char *member_id)
{
    const ReservationQueue *queue = find_queue(inventory, book_id);
    int position = 1;

    if (queue == NULL) {
        return 0;
    }
    /* Walk from the head, counting from 1. */
    for (const ReservationNode *node = queue->head; node != NULL; node = node->next, position++) {
        if (strcmp(node->member_id, member_id) == 0) {
            return position;
        }
    }
    return 0;
}

int queue_push(Inventory *inventory, const char *book_id, const char *member_id,
               time_t reservation_date)
{
    ReservationQueue *queue = find_queue(inventory, book_id);

    /* First reservation of this title: give it a queue of its own. */
    if (queue == NULL) {
        if (inventory->queue_count == inventory->queue_capacity) {
            int new_capacity = inventory->queue_capacity == 0 ? 8 : inventory->queue_capacity * 2;
            ReservationQueue *grown = realloc(inventory->queues,
                                              (size_t)new_capacity * sizeof(ReservationQueue));
            if (grown == NULL) {
                return 0;
            }
            inventory->queues = grown;
            inventory->queue_capacity = new_capacity;
        }
        queue = &inventory->queues[inventory->queue_count++];
        memset(queue, 0, sizeof(*queue));
        snprintf(queue->book_id, sizeof(queue->book_id), "%s", book_id);
    }

    ReservationNode *node = malloc(sizeof(ReservationNode));
    if (node == NULL) {
        return 0;
    }
    snprintf(node->member_id, sizeof(node->member_id), "%s", member_id);
    node->reservation_date = reservation_date;
    node->next = NULL;

    /* Join at the tail: first in, first out. */
    if (queue->tail == NULL) {
        queue->head = node;
    } else {
        queue->tail->next = node;
    }
    queue->tail = node;
    queue->length++;
    return 1;
}

int queue_pop(Inventory *inventory, const char *book_id, char member_id[ID_SIZE])
{
    ReservationQueue *queue = find_queue(inventory, book_id);

    if (queue == NULL || queue->head == NULL) {
        return 0;
    }

    /* Serve from the head: the member who has waited longest. */
    ReservationNode *node = queue->head;
    strcpy(member_id, node->member_id);

    queue->head = node->next;
    /* That was the only member waiting, so the tail goes too. */
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    queue->length--;
    free(node);
    return 1;
}
