/* Commands for members, books and copies. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cli_internal.h"

/* Membership management */

CmdStatus cmd_register_member(Library *lib, Args *args)
{
    const char *id = cli_arg(args, 0, "Member ID (ALU###): ");
    const char *name = id != NULL ? cli_arg(args, 1, "Full name: ") : NULL;
    const char *course = name != NULL ? cli_arg(args, 2, "Course code: ") : NULL;
    Member member;
    int created = 0;

    if (course == NULL || !cli_member_id_ok(id)) {
        return CMD_CONTINUE;
    }
    if (find_member(&lib->registry, id) != -1) {
        printf("ERROR: Member %s is already registered.\n", id);
        return CMD_CONTINUE;
    }
    if (!text_field_ok(name, NAME_SIZE) || !text_field_ok(course, COURSE_SIZE)) {
        printf("ERROR: The name (max %d) and course code (max %d) cannot be empty or contain "
               "',' or '|'.\n", NAME_SIZE - 1, COURSE_SIZE - 1);
        return CMD_CONTINUE;
    }

    memset(&member, 0, sizeof(member));
    strcpy(member.member_id, id);
    strcpy(member.full_name, name);
    strcpy(member.course_code, course);

    /* The key pair comes first: a member without a key could never sign a request. */
    if (!keystore_ensure(&lib->keys, id, &created)) {
        printf("ERROR: Could not create a key pair for %s.\n", id);
        return CMD_CONTINUE;
    }
    if (!register_member(lib->members_file, &lib->registry, &member) ||
        !ledger_open_account(&lib->ledger, id)) {
        printf("ERROR: Could not add %s to %s (registry full or file not writable).\n", id,
               lib->members_file);
        return CMD_CONTINUE;
    }

    printf("Member %s (%s, %s) registered.\n", id, name, course);
    printf("  ECDSA key pair %s; public key (on-chain identity):\n  %s\n",
           created ? "generated" : "found", keystore_find(&lib->keys, id)->public_hex);
    printf("  Token balance 0, nonce 0.\n");
    return CMD_CONTINUE;
}

static void print_member(const Library *lib, const Member *member)
{
    const KeyEntry *key = keystore_find(&lib->keys, member->member_id);
    char a[32], b[32], when[20];
    unsigned long confirmed = ledger_nonce(&lib->ledger, member->member_id);

    printf("\nMember %s: %s (%s)\n", member->member_id, member->full_name, member->course_code);
    printf("  Public key : %s\n", key != NULL ? key->public_hex : "(no key)");
    printf("  Nonce      : %lu confirmed, next to use %lu\n", confirmed,
           lending_next_nonce(lib, member->member_id));
    printf("  Tokens     : %s (%s model)\n",
           format_coins(ledger_balance(&lib->ledger, member->member_id), a),
           ledger_model_name(lib->ledger.model));
    printf("  Fines      : %s owed, %s paid\n", format_coins(member->fines_owed, a),
           format_coins(member->fines_paid, b));

    for (int i = 0; i < lib->inventory.copy_count; i++) {
        const Copy *copy = &lib->inventory.copies[i];
        if (strcmp(copy->current_holder_id, member->member_id) != 0) {
            continue;
        }
        if (copy->state == COPY_BORROWED) {
            printf("  On loan    : %s, due %s%s\n", copy->copy_id,
                   format_time(copy->due_date, when),
                   library_now(lib) > copy->due_date ? "  (OVERDUE)" : "");
        } else if (copy->state == COPY_RESERVED) {
            printf("  On hold    : %s is being held for this member\n", copy->copy_id);
        }
    }
    for (int q = 0; q < lib->inventory.queue_count; q++) {
        int position = queue_position(&lib->inventory, lib->inventory.queues[q].book_id,
                                      member->member_id);
        if (position > 0) {
            printf("  Waiting    : %s, position %d of %d\n", lib->inventory.queues[q].book_id,
                   position, lib->inventory.queues[q].length);
        }
    }
}

CmdStatus cmd_view_member(Library *lib, Args *args)
{
    char amount[32], fines[32];

    if (args->argc > 0) {
        int index = cli_member_id_ok(args->argv[0]) ? find_member(&lib->registry, args->argv[0]) : -2;
        if (index == -1) {
            printf("ERROR: Member ID '%s' is not in the member registry.\n", args->argv[0]);
        } else if (index >= 0) {
            print_member(lib, &lib->registry.members[index]);
        }
        return CMD_CONTINUE;
    }

    printf("\n%-8s %-24s %-7s %7s %10s %10s %6s\n", "Member", "Name", "Course", "On loan",
           "Tokens", "Fines owed", "Nonce");
    printf("-------- ------------------------ ------- ------- ---------- ---------- ------\n");
    for (int i = 0; i < lib->registry.member_count; i++) {
        const Member *member = &lib->registry.members[i];
        printf("%-8s %-24.24s %-7s %7d %10s %10s %6lu\n", member->member_id, member->full_name,
               member->course_code, lending_copies_held(lib, member->member_id),
               format_coins(ledger_balance(&lib->ledger, member->member_id), amount),
               format_coins(member->fines_owed, fines),
               ledger_nonce(&lib->ledger, member->member_id));
    }
    printf("Use view_member <member_id> for the public key, loans and reservations.\n");
    return CMD_CONTINUE;
}

void cli_print_nonce(const Library *lib, const char *id)
{
    unsigned long confirmed = ledger_nonce(&lib->ledger, id);
    unsigned long next = lending_next_nonce(lib, id);

    printf("%s: confirmed nonce %lu, %lu request%s in the pending pool, next nonce to use %lu.\n",
           id, confirmed, next - confirmed, next - confirmed == 1 ? "" : "s", next);
    printf("The nonce moves only when a request is confirmed in a mined block.\n");
}

CmdStatus cmd_member_nonce(Library *lib, Args *args)
{
    const char *id = cli_arg(args, 0, "Member ID: ");

    if (id == NULL || !cli_member_id_ok(id)) {
        return CMD_CONTINUE;
    }
    if (find_member(&lib->registry, id) == -1) {
        printf("ERROR: Member ID '%s' is not in the member registry.\n", id);
        return CMD_CONTINUE;
    }
    cli_print_nonce(lib, id);
    return CMD_CONTINUE;
}

/* Book and copy management */

CmdStatus cmd_register_book(Library *lib, Args *args)
{
    const char *id = cli_arg(args, 0, "Book ID (BK###): ");
    const char *title = id != NULL ? cli_arg(args, 1, "Title: ") : NULL;
    const char *author = title != NULL ? cli_arg(args, 2, "Author: ") : NULL;
    Book book;

    if (author == NULL || !cli_book_id_ok(id)) {
        return CMD_CONTINUE;
    }
    if (find_book(&lib->registry, id) != -1) {
        printf("ERROR: Book %s is already registered.\n", id);
        return CMD_CONTINUE;
    }
    if (!text_field_ok(title, TITLE_SIZE) || !text_field_ok(author, NAME_SIZE)) {
        printf("ERROR: The title (max %d) and author (max %d) cannot be empty or contain "
               "',' or '|'.\n", TITLE_SIZE - 1, NAME_SIZE - 1);
        return CMD_CONTINUE;
    }

    memset(&book, 0, sizeof(book));
    strcpy(book.book_id, id);
    strcpy(book.title, title);
    strcpy(book.author, author);

    if (!register_book(lib->books_file, &lib->registry, &book)) {
        printf("ERROR: Could not add %s to %s (registry full or file not writable).\n", id,
               lib->books_file);
        return CMD_CONTINUE;
    }
    printf("Book %s '%s' by %s registered. It has no copies yet: use add_copy %s.\n", id, title,
           author, id);
    return CMD_CONTINUE;
}

static void print_copy_line(const Library *lib, const Copy *copy)
{
    char when[20];

    printf("  %-10s %-5s %-10s", copy->copy_id, condition_name(copy->condition),
           copy_state_name(copy->state));
    if (copy->state == COPY_BORROWED) {
        printf(" %s (%s), due %s, renewals %d/%d%s", copy->current_holder_id,
               cli_member_name(lib, copy->current_holder_id), format_time(copy->due_date, when),
               copy->renewals, MAX_RENEWALS,
               library_now(lib) > copy->due_date ? ", OVERDUE" : "");
    } else if (copy->state == COPY_RESERVED) {
        printf(" held for %s (%s)", copy->current_holder_id,
               cli_member_name(lib, copy->current_holder_id));
    }
    printf("\n");
}

void cli_print_queue(const Library *lib, const char *book_id)
{
    const ReservationQueue *queue = queue_find(&lib->inventory, book_id);
    char when[20];
    int position = 1;

    if (queue == NULL || queue->length == 0) {
        printf("  Reservation queue: empty\n");
        return;
    }
    printf("  Reservation queue (%d waiting, first in first out):\n", queue->length);
    for (const ReservationNode *node = queue->head; node != NULL; node = node->next) {
        printf("    %d. %s (%s), reserved %s\n", position++, node->member_id,
               cli_member_name(lib, node->member_id), format_time(node->reservation_date, when));
    }
}

CmdStatus cmd_view_book(Library *lib, Args *args)
{
    const Inventory *inventory = &lib->inventory;

    if (args->argc > 0) {
        int index = cli_book_id_ok(args->argv[0]) ? find_book(&lib->registry, args->argv[0]) : -2;
        if (index == -1) {
            printf("ERROR: Book ID '%s' is not in the book registry.\n", args->argv[0]);
        } else if (index >= 0) {
            const Book *book = &lib->registry.books[index];
            printf("\nBook %s: '%s' by %s\n", book->book_id, book->title, book->author);
            printf("  Copies (%d):\n", inventory_copies_of(inventory, book->book_id));
            for (int i = 0; i < inventory->copy_count; i++) {
                if (strcmp(inventory->copies[i].book_id, book->book_id) == 0) {
                    printf("  ");
                    print_copy_line(lib, &inventory->copies[i]);
                }
            }
            cli_print_queue(lib, book->book_id);
        }
        return CMD_CONTINUE;
    }

    printf("\n%-6s %-42s %6s %9s %8s %8s %4s %5s\n", "Book", "Title", "Copies", "Available",
           "Borrowed", "Reserved", "Lost", "Queue");
    printf("------ ------------------------------------------ ------ --------- -------- -------- "
           "---- -----\n");
    for (int i = 0; i < lib->registry.book_count; i++) {
        const Book *book = &lib->registry.books[i];
        printf("%-6s %-42.42s %6d %9d %8d %8d %4d %5d\n", book->book_id, book->title,
               inventory_copies_of(inventory, book->book_id),
               inventory_count(inventory, book->book_id, COPY_AVAILABLE),
               inventory_count(inventory, book->book_id, COPY_BORROWED),
               inventory_count(inventory, book->book_id, COPY_RESERVED),
               inventory_count(inventory, book->book_id, COPY_LOST),
               queue_length(inventory, book->book_id));
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_add_copy(Library *lib, Args *args)
{
    const char *book_id = cli_arg(args, 0, "Book ID: ");
    const char *condition_text = args->argc > 1 ? args->argv[1] : "NEW";
    char copy_id[COPY_ID_SIZE], line[CSV_LINE_SIZE];

    if (book_id == NULL || !cli_book_id_ok(book_id)) {
        return CMD_CONTINUE;
    }
    if (find_book(&lib->registry, book_id) == -1) {
        printf("ERROR: Book ID '%s' is not in the book registry.\n", book_id);
        return CMD_CONTINUE;
    }

    int condition = parse_condition(condition_text);
    if (condition < 0 || condition == COND_LOST) {
        printf("ERROR: Condition must be NEW, GOOD or WORN.\n");
        return CMD_CONTINUE;
    }

    /* Next free copy number of this title. */
    int number = 1;
    do {
        snprintf(copy_id, sizeof(copy_id), "%s-C%d", book_id, number++);
    } while (inventory_find_copy(&lib->inventory, copy_id) != NULL && number < 1000);
    if (number >= 1000) {
        printf("ERROR: Book %s already has the maximum number of copies.\n", book_id);
        return CMD_CONTINUE;
    }

    snprintf(line, sizeof(line), "%s,%s,%s", copy_id, book_id, condition_text);
    if (!csv_append_line(lib->copies_file, line) ||
        inventory_add_copy(&lib->inventory, copy_id, book_id, (Condition)condition) == NULL) {
        printf("ERROR: Could not add the copy to %s.\n", lib->copies_file);
        return CMD_CONTINUE;
    }

    /* A member may already be waiting for this title. */
    printf("Copy %s (%s) added to book %s: AVAILABLE.\n", copy_id, condition_text, book_id);
    if (queue_length(&lib->inventory, book_id) > 0) {
        printf("%d member(s) are waiting for this title: the copy is offered when the next "
               "reservation or return for it is mined.\n",
               queue_length(&lib->inventory, book_id));
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_view_copy(Library *lib, Args *args)
{
    if (args->argc == 0) {
        printf("\n  %-10s %-5s %-10s %s\n", "Copy", "Cond.", "State", "Holder");
        printf("  ---------- ----- ---------- ----------------------------------------\n");
        for (int i = 0; i < lib->inventory.copy_count; i++) {
            print_copy_line(lib, &lib->inventory.copies[i]);
        }
        return CMD_CONTINUE;
    }

    if (!cli_copy_id_ok(args->argv[0])) {
        return CMD_CONTINUE;
    }
    const Copy *copy = inventory_find_copy(&lib->inventory, args->argv[0]);
    if (copy == NULL) {
        printf("ERROR: Copy ID '%s' is not in the copy inventory.\n", args->argv[0]);
        return CMD_CONTINUE;
    }

    int book_index = find_book(&lib->registry, copy->book_id);
    char when[20];
    printf("\nCopy %s of %s '%s'\n", copy->copy_id, copy->book_id,
           book_index != -1 ? lib->registry.books[book_index].title : "?");
    printf("  Condition      : %s\n", condition_name(copy->condition));
    printf("  State          : %s\n", copy_state_name(copy->state));
    printf("  Current holder : %s%s%s\n",
           copy->current_holder_id[0] != '\0' ? copy->current_holder_id : "(none)",
           copy->state == COPY_RESERVED ? " (held for this member)" : "",
           copy->state == COPY_BORROWED && library_now(lib) > copy->due_date ? " - OVERDUE" : "");
    printf("  Due date       : %s\n", format_time(copy->due_date, when));
    printf("  Renewals       : %d of %d\n", copy->renewals, MAX_RENEWALS);

    for (int i = 0; i < lib->pool.count; i++) {
        const PendingRequest *request = &lib->pool.items[i];
        if (strcmp(request->copy_id, copy->copy_id) == 0) {
            printf("  In the pool    : %s request %s (%s)\n", request->action_requested,
                   request->request_id, request_status_name(request->status));
        }
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_mark_lost(Library *lib, Args *args)
{
    const char *copy_id = cli_arg(args, 0, "Copy ID: ");
    RequestOptions options;

    if (copy_id == NULL || !cli_copy_id_ok(copy_id) || !cli_request_options(lib, args, &options)) {
        return CMD_CONTINUE;
    }
    /* The reason code may be the second argument or reason=CODE. */
    if (options.reason == NULL) {
        options.reason = cli_arg(args, 1, "Reason code (e.g. DAMAGED, NOT_RETURNED, STOLEN): ");
        if (options.reason == NULL) {
            return CMD_CONTINUE;
        }
    }
    lending_mark_lost(lib, copy_id, &options);
    return CMD_CONTINUE;
}
