/* Commands for lending, reservations, overdue loans and fines. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cli_internal.h"

/* Lending operations */

CmdStatus cmd_borrow_book(Library *lib, Args *args)
{
    const char *book_id = cli_arg(args, 0, "Book ID  : ");
    const char *member_id = book_id != NULL ? cli_arg(args, 1, "Member ID: ") : NULL;
    RequestOptions options;

    if (member_id == NULL || !cli_request_options(lib, args, &options)) {
        return CMD_CONTINUE;
    }
    /* Check both IDs before stopping, so both mistakes are reported. */
    int ids_ok = cli_book_id_ok(book_id);
    ids_ok = cli_member_id_ok(member_id) && ids_ok;
    if (ids_ok) {
        lending_borrow(lib, book_id, member_id, &options);
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_return_book(Library *lib, Args *args)
{
    const char *copy_id = cli_arg(args, 0, "Copy ID: ");
    RequestOptions options;

    if (copy_id == NULL || !cli_copy_id_ok(copy_id) || !cli_request_options(lib, args, &options)) {
        return CMD_CONTINUE;
    }
    if (args->argc > 1) {
        options.condition = args->argv[1];     /* NEW, GOOD or WORN, as assessed at the desk */
    }
    lending_return(lib, copy_id, &options);
    return CMD_CONTINUE;
}

CmdStatus cmd_renew_book(Library *lib, Args *args)
{
    const char *copy_id = cli_arg(args, 0, "Copy ID: ");
    RequestOptions options;

    if (copy_id == NULL || !cli_copy_id_ok(copy_id) || !cli_request_options(lib, args, &options)) {
        return CMD_CONTINUE;
    }
    lending_renew(lib, copy_id, &options);
    return CMD_CONTINUE;
}

CmdStatus cmd_reserve_book(Library *lib, Args *args)
{
    const char *book_id = cli_arg(args, 0, "Book ID  : ");
    const char *member_id = book_id != NULL ? cli_arg(args, 1, "Member ID: ") : NULL;
    RequestOptions options;

    if (member_id == NULL || !cli_request_options(lib, args, &options)) {
        return CMD_CONTINUE;
    }
    int ids_ok = cli_book_id_ok(book_id);
    ids_ok = cli_member_id_ok(member_id) && ids_ok;
    if (ids_ok) {
        lending_reserve(lib, book_id, member_id, &options);
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_reservation_status(Library *lib, Args *args)
{
    int shown = 0;

    if (args->argc > 0 && (!cli_book_id_ok(args->argv[0]) ||
                           find_book(&lib->registry, args->argv[0]) == -1)) {
        if (id_has_format(args->argv[0], BOOK_ID_PREFIX)) {
            printf("ERROR: Book ID '%s' is not in the book registry.\n", args->argv[0]);
        }
        return CMD_CONTINUE;
    }

    for (int i = 0; i < lib->registry.book_count; i++) {
        const Book *book = &lib->registry.books[i];
        int held = inventory_count(&lib->inventory, book->book_id, COPY_RESERVED);

        if (args->argc > 0 ? strcmp(book->book_id, args->argv[0]) != 0
                           : (held == 0 && queue_length(&lib->inventory, book->book_id) == 0)) {
            continue;
        }
        printf("\n%s '%s': %d of %d copies available\n", book->book_id, book->title,
               inventory_count(&lib->inventory, book->book_id, COPY_AVAILABLE),
               inventory_copies_of(&lib->inventory, book->book_id));
        for (int c = 0; c < lib->inventory.copy_count; c++) {
            const Copy *copy = &lib->inventory.copies[c];
            if (copy->state == COPY_RESERVED && strcmp(copy->book_id, book->book_id) == 0) {
                printf("  Copy %s is on hold for %s (%s): only they can borrow it.\n",
                       copy->copy_id, copy->current_holder_id,
                       cli_member_name(lib, copy->current_holder_id));
            }
        }
        cli_print_queue(lib, book->book_id);
        shown++;
    }

    /* Reservations that are not mined yet. */
    for (int i = 0; i < lib->pool.count; i++) {
        const PendingRequest *request = &lib->pool.items[i];
        if (strcmp(request->action_requested, ACTION_RESERVED) == 0 &&
            (args->argc == 0 || strcmp(request->book_id, args->argv[0]) == 0)) {
            printf("  In the pool: reservation %s of %s for %s (%s) - not confirmed yet.\n",
                   request->request_id, request->book_id, request->member_id,
                   request_status_name(request->status));
            shown++;
        }
    }
    if (shown == 0) {
        printf("No reservations and no copies on hold.\n");
    }
    return CMD_CONTINUE;
}

/* Fines and overdue */

CmdStatus cmd_check_overdue(Library *lib, Args *args)
{
    time_t now = library_now(lib);
    char when[20], fine[32];
    int overdue = 0;

    (void)args;
    /* The scan itself already ran before this command, as it does before every command. */
    printf("\nLibrary time is %s. Fine: %s coins per day late.\n", format_time(now, when),
           format_coins(FINE_PER_DAY, fine));
    printf("%-10s %-8s %-22s %-16s %9s %12s %s\n", "Copy", "Member", "Name", "Due", "Days late",
           "Fine so far", "OVERDUE record");
    printf("---------- -------- ---------------------- ---------------- --------- ------------ "
           "--------------\n");

    for (int i = 0; i < lib->inventory.copy_count; i++) {
        const Copy *copy = &lib->inventory.copies[i];
        if (copy->state != COPY_BORROWED || now <= copy->due_date) {
            continue;
        }

        long accrued = overdue_fine(copy->due_date, now);
        const char *recorded = "raised";
        if (chain_last_record(&lib->chain, copy->copy_id, ACTION_OVERDUE) != NULL &&
            chain_last_record(&lib->chain, copy->copy_id, ACTION_OVERDUE)->due_date ==
            copy->due_date) {
            recorded = "on chain";
        } else if (copy->overdue_flagged) {
            recorded = "in the pool";
        }

        printf("%-10s %-8s %-22.22s %-16s %9ld %12s %s\n", copy->copy_id,
               copy->current_holder_id, cli_member_name(lib, copy->current_holder_id),
               format_time(copy->due_date, when), accrued / FINE_PER_DAY,
               format_coins(accrued, fine), recorded);
        overdue++;
    }

    if (overdue == 0) {
        printf("(no overdue loans)\n");
    } else {
        printf("%d overdue loan%s. The fine is fixed when the copy is returned.\n", overdue,
               overdue == 1 ? "" : "s");
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_view_fines(Library *lib, Args *args)
{
    time_t now = library_now(lib);
    char a[32], b[32], c[32];
    int rows = 0;

    if (args->argc > 0 && (!cli_member_id_ok(args->argv[0]) ||
                           find_member(&lib->registry, args->argv[0]) == -1)) {
        if (id_has_format(args->argv[0], MEMBER_ID_PREFIX)) {
            printf("ERROR: Member ID '%s' is not in the member registry.\n", args->argv[0]);
        }
        return CMD_CONTINUE;
    }

    printf("\n%-8s %-22s %14s %12s %10s\n", "Member", "Name", "Still accruing", "Owed", "Paid");
    printf("-------- ---------------------- -------------- ------------ ----------\n");
    for (int i = 0; i < lib->registry.member_count; i++) {
        const Member *member = &lib->registry.members[i];
        long accruing = 0;

        /* Fines on loans that are overdue and not returned yet. */
        for (int k = 0; k < lib->inventory.copy_count; k++) {
            const Copy *copy = &lib->inventory.copies[k];
            if (copy->state == COPY_BORROWED &&
                strcmp(copy->current_holder_id, member->member_id) == 0) {
                accruing += overdue_fine(copy->due_date, now);
            }
        }

        if (args->argc > 0 ? strcmp(member->member_id, args->argv[0]) != 0
                           : (accruing == 0 && member->fines_owed == 0 && member->fines_paid == 0)) {
            continue;
        }
        printf("%-8s %-22.22s %14s %12s %10s\n", member->member_id, member->full_name,
               format_coins(accruing, a), format_coins(member->fines_owed, b),
               format_coins(member->fines_paid, c));
        rows++;
    }
    if (rows == 0) {
        printf("(no fines)\n");
    }
    printf("'Owed' is fixed by a late return and paid in tokens with settle_fine <member_id>.\n");
    return CMD_CONTINUE;
}

CmdStatus cmd_settle_fine(Library *lib, Args *args)
{
    const char *member_id = cli_arg(args, 0, "Member ID: ");
    RequestOptions options;
    long amount = 0;           /* 0 = everything owed */

    if (member_id == NULL || !cli_member_id_ok(member_id) || !cli_request_options(lib, args, &options)) {
        return CMD_CONTINUE;
    }
    if (args->argc > 1 && !cli_parse_coins(args->argv[1], &amount)) {
        printf("ERROR: Enter a positive amount such as 3 or 2.50.\n");
        return CMD_CONTINUE;
    }
    lending_settle_fine(lib, member_id, amount, &options);
    return CMD_CONTINUE;
}
