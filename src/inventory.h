#ifndef INVENTORY_H
#define INVENTORY_H

#include <time.h>

#include "registry.h"

#define COPY_ID_SIZE 28    /* <book_id>-C<number> */

typedef enum { COND_NEW, COND_GOOD, COND_WORN, COND_LOST } Condition;
typedef enum { COPY_AVAILABLE, COPY_BORROWED, COPY_RESERVED, COPY_LOST } CopyState;

typedef struct {
    char copy_id[COPY_ID_SIZE];
    char book_id[ID_SIZE];
    Condition condition;
    CopyState state;
    char current_holder_id[ID_SIZE];
    time_t due_date;
    int renewals;
    int overdue_flagged;
} Copy;

/* First-in-first-out reservation queue of one title, a linked list. */
typedef struct ReservationNode {
    char member_id[ID_SIZE];
    time_t reservation_date;
    struct ReservationNode *next;
} ReservationNode;

typedef struct {
    char book_id[ID_SIZE];
    ReservationNode *head;   /* waited longest */
    ReservationNode *tail;
    int length;
} ReservationQueue;

typedef struct {
    Copy *copies;            /* heap arrays */
    int copy_count;
    int copy_capacity;
    ReservationQueue *queues;
    int queue_count;
    int queue_capacity;
} Inventory;

void inventory_init(Inventory *inventory);
void inventory_free(Inventory *inventory);

/* copies.txt: copy_id,book_id,condition. Returns the number of copies, 0 on error. */
int inventory_load_copies(Inventory *inventory, const char *filename, const Registry *registry);

Copy *inventory_find_copy(const Inventory *inventory, const char *copy_id);
/* Returns the stored copy, or NULL if memory ran out or the ID is taken. */
Copy *inventory_add_copy(Inventory *inventory, const char *copy_id, const char *book_id,
                         Condition condition);
int inventory_count(const Inventory *inventory, const char *book_id, CopyState state);
/* Number of copies of the title, lost ones included. */
int inventory_copies_of(const Inventory *inventory, const char *book_id);

/* The availability state machine: only the listed transitions are legal. */
int copy_transition_allowed(CopyState from, CopyState to);
/* Returns 0 and leaves the copy unchanged if the transition is illegal. */
int copy_set_state(Copy *copy, CopyState to);

const char *copy_state_name(CopyState state);
const char *condition_name(Condition condition);
/* Returns -1 for an unknown name. */
int parse_condition(const char *text);
int parse_copy_state(const char *text);
/* <book_id>-C<1 to 3 digits> */
int copy_id_has_format(const char *copy_id);

/* Reservation queues. */
int queue_push(Inventory *inventory, const char *book_id, const char *member_id,
               time_t reservation_date);
/* Removes the member who has waited longest; returns 0 if the queue is empty. */
int queue_pop(Inventory *inventory, const char *book_id, char member_id[ID_SIZE]);
const ReservationQueue *queue_find(const Inventory *inventory, const char *book_id);
int queue_length(const Inventory *inventory, const char *book_id);
/* Position in the queue starting at 1, or 0 if the member is not waiting. */
int queue_position(const Inventory *inventory, const char *book_id, const char *member_id);

#endif
