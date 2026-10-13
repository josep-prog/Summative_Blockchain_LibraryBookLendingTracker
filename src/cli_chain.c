/* Commands for the pool, mining, the chain, fraud review, audit and the demo tools. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cli_internal.h"

/* Blockchain and mining operations */

CmdStatus cmd_pool_view(Library *lib, Args *args)
{
    (void)args;
    print_pending_pool(lib);
    return CMD_CONTINUE;
}

CmdStatus cmd_mine_solo(Library *lib, Args *args)
{
    (void)args;
    mine_solo(lib, lib->user->librarian_id);
    return CMD_CONTINUE;
}

CmdStatus cmd_mine_pool(Library *lib, Args *args)
{
    int miners = DEFAULT_POOL_MINERS;

    if (args->argc > 0 && !cli_parse_int(args->argv[0], MIN_POOL_MINERS, MAX_POOL_MINERS, &miners)) {
        printf("ERROR: The number of pool miners must be from %d to %d.\n", MIN_POOL_MINERS,
               MAX_POOL_MINERS);
        return CMD_CONTINUE;
    }
    mine_pool(lib, miners);
    return CMD_CONTINUE;
}

CmdStatus cmd_mine_cloud(Library *lib, Args *args)
{
    const char *rounds_text = cli_arg(args, 0, "Rental duration in rounds (1-5): ");
    int rounds;

    if (rounds_text == NULL) {
        return CMD_CONTINUE;
    }
    if (!cli_parse_int(rounds_text, 1, MAX_RENTAL_ROUNDS, &rounds)) {
        printf("ERROR: The rental duration must be from 1 to %d rounds.\n", MAX_RENTAL_ROUNDS);
        return CMD_CONTINUE;
    }
    mine_cloud(lib, lib->user->librarian_id, rounds);
    return CMD_CONTINUE;
}

CmdStatus cmd_chain_view(Library *lib, Args *args)
{
    if (args->argc > 0) {
        int index;
        if (!cli_parse_int(args->argv[0], 0, lib->chain.count - 1, &index)) {
            printf("ERROR: Block index must be from 0 to %d.\n", lib->chain.count - 1);
            return CMD_CONTINUE;
        }
        print_block(&lib->chain.blocks[index], 1);
        return CMD_CONTINUE;
    }

    printf("\nConfirmed chain (%d block%s). Use chain_view <index> to see a block's Merkle tree.",
           lib->chain.count, lib->chain.count == 1 ? "" : "s");
    for (int i = 0; i < lib->chain.count; i++) {
        print_block(&lib->chain.blocks[i], 0);
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_validate_chain(Library *lib, Args *args)
{
    (void)args;
    library_validate(lib);
    return CMD_CONTINUE;
}

CmdStatus cmd_chain_save(Library *lib, Args *args)
{
    (void)args;
    if (library_save(lib)) {
        printf("Saved %d block%s, %d cop%s, %d pending request%s and the %s ledger to %s "
               "(sealed by %s).\n", lib->chain.count, lib->chain.count == 1 ? "" : "s",
               lib->inventory.copy_count, lib->inventory.copy_count == 1 ? "y" : "ies",
               lib->pool.count, lib->pool.count == 1 ? "" : "s",
               ledger_model_name(lib->ledger.model), lib->data_file, lib->user->librarian_id);
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_chain_load(Library *lib, Args *args)
{
    (void)args;
    printf("Discarding the state in memory and reloading %s...\n", lib->data_file);
    if (!library_boot(lib, lib->ledger.model, 1)) {
        printf("The program cannot continue without a readable chain.\n");
        return CMD_QUIT;
    }
    return CMD_CONTINUE;
}

CmdStatus cmd_difficulty_status(Library *lib, Args *args)
{
    char target[MAX_DIFFICULTY + 1] = "";
    double expected = 1.0;

    (void)args;
    for (int i = 0; i < lib->difficulty; i++) {
        target[i] = '0';
        expected *= 16.0;
    }

    printf("\nMining difficulty : %d (a block hash must start with \"%s\")\n", lib->difficulty,
           target);
    printf("Expected effort   : about %.0f hash attempts per block\n", expected);
    printf("Block reward      : %d coins per mined block\n", BLOCK_REWARD);
    printf("Block size        : up to %d records per block\n", BLOCK_CAPACITY);

    printf("\nBlocks on the chain by difficulty:\n");
    for (int d = MIN_DIFFICULTY; d <= MAX_DIFFICULTY; d++) {
        int blocks = 0;
        for (int i = 0; i < lib->chain.count; i++) {
            if (lib->chain.blocks[i].difficulty == d) blocks++;
        }
        printf("  difficulty %d: %d block%s\n", d, blocks, blocks == 1 ? "" : "s");
    }
    printf("Change it with set_difficulty <1-4>; compare the effort with benchmark.\n");
    return CMD_CONTINUE;
}

CmdStatus cmd_set_difficulty(Library *lib, Args *args)
{
    const char *text = cli_arg(args, 0, "Difficulty (1-4): ");
    int difficulty;

    if (text == NULL) {
        return CMD_CONTINUE;
    }
    if (!cli_parse_int(text, MIN_DIFFICULTY, MAX_DIFFICULTY, &difficulty)) {
        printf("ERROR: Difficulty must be from %d to %d.\n", MIN_DIFFICULTY, MAX_DIFFICULTY);
        return CMD_CONTINUE;
    }
    lib->difficulty = difficulty;
    printf("New blocks must now start with %d zero%s.\n", difficulty, difficulty == 1 ? "" : "s");
    return CMD_CONTINUE;
}

CmdStatus cmd_benchmark(Library *lib, Args *args)
{
    (void)lib;
    (void)args;
    mining_benchmark();
    return CMD_CONTINUE;
}

/* Fraud and audit operations */

CmdStatus cmd_fraud_review(Library *lib, Args *args)
{
    int suspicious = pending_count_status(&lib->pool, REQ_SUSPICIOUS);

    (void)args;
    printf("\nFraud review: %d request%s held as SUSPICIOUS\n", suspicious,
           suspicious == 1 ? "" : "s");
    for (int i = 0; i < lib->pool.count; i++) {
        const PendingRequest *request = &lib->pool.items[i];
        if (request->status != REQ_SUSPICIOUS) {
            continue;
        }
        printf("\n  %s  [%s]\n", request->request_id, request->flag);
        printf("    Why    : %s\n", request->flag_detail);
        print_record(&request->record);
        printf("    Decide : approve_suspicious %s   or   reject_suspicious %s\n",
               request->request_id, request->request_id);
    }

    printf("\nHeuristics checked on every request:\n");
    printf("  EXCESSIVE_BORROWS       more than %d copies held at once\n", MAX_CONCURRENT_BORROWS);
    printf("  RAPID_REBORROW          same title borrowed again within %ld hours of returning it\n",
           REBORROW_WINDOW_SECONDS / 3600);
    printf("  DUPLICATE_REQUEST       request ID already on the chain or in the pool\n");
    printf("  CONDITION_MANIPULATION  condition down more than one level with no reason code\n");
    return CMD_CONTINUE;
}

CmdStatus cmd_approve_suspicious(Library *lib, Args *args)
{
    const char *id = cli_arg(args, 0, "Request ID: ");

    if (id == NULL) {
        return CMD_CONTINUE;
    }
    int index = pending_find(&lib->pool, id, 1);
    if (index == -1) {
        printf("ERROR: No SUSPICIOUS request '%s' in the pool.\n", id);
        return CMD_CONTINUE;
    }

    PendingRequest *request = &lib->pool.items[index];
    /* A second record with the same ID could never be valid on the chain. */
    if (strcmp(request->flag, "DUPLICATE_REQUEST") == 0) {
        printf("ERROR: A request ID may appear on the chain only once, so a duplicate cannot be "
               "approved. Use reject_suspicious %s.\n", id);
        return CMD_CONTINUE;
    }

    request->status = REQ_PENDING;
    snprintf(request->flag_detail, sizeof(request->flag_detail), "approved by %s",
             lib->user->librarian_id);
    printf("Request %s (%s, flagged %s) approved by %s: it is PENDING and can now be mined.\n",
           id, request->action_requested, request->flag, lib->user->librarian_id);
    return CMD_CONTINUE;
}

CmdStatus cmd_reject_suspicious(Library *lib, Args *args)
{
    const char *id = cli_arg(args, 0, "Request ID: ");
    char why[64];

    if (id == NULL) {
        return CMD_CONTINUE;
    }
    int index = pending_find(&lib->pool, id, 1);
    if (index == -1) {
        printf("ERROR: No SUSPICIOUS request '%s' in the pool.\n", id);
        return CMD_CONTINUE;
    }
    snprintf(why, sizeof(why), "rejected by %s after fraud review", lib->user->librarian_id);
    lending_drop_request(lib, index, why);
    return CMD_CONTINUE;
}

/* Lists confirmed records, optionally only those of one member or one book. */
static void print_history(const Library *lib, const char *member_id, const char *book_id)
{
    char when[20], due[20], fine[32];
    int rows = 0;

    printf("\n%-5s %-11s %-16s %-9s %-10s %-8s %-16s %7s %6s\n", "Block", "Record", "Time",
           "Action", "Copy/Book", "Member", "Due", "Fine", "Reward");
    printf("----- ----------- ---------------- --------- ---------- -------- ---------------- "
           "------- ------\n");

    for (int b = 1; b < lib->chain.count; b++) {
        const Block *block = &lib->chain.blocks[b];
        for (int r = 0; r < block->record_count; r++) {
            const LendingRecord *record = &block->records[r];

            if (member_id != NULL && strcmp(record->member_id, member_id) != 0 &&
                strcmp(record->peer_id, member_id) != 0) {
                continue;
            }
            if (book_id != NULL && strcmp(record->book_id, book_id) != 0) {
                continue;
            }
            printf("%-5d %-11s %-16s %-9s %-10s %-8s %-16s %7s %6d\n", block->index,
                   record->record_id, format_time(record->timestamp, when), record->action,
                   record->copy_id[0] != '\0' ? record->copy_id :
                   record->book_id[0] != '\0' ? record->book_id : "-",
                   record->member_id[0] != '\0' ? record->member_id : "-",
                   format_time(record->due_date, due), format_coins(record->fine_amount, fine),
                   record->token_reward);
            rows++;
        }
    }
    printf("%d confirmed record%s.\n", rows, rows == 1 ? "" : "s");
}

CmdStatus cmd_lending_history(Library *lib, Args *args)
{
    (void)args;
    print_history(lib, NULL, NULL);
    return CMD_CONTINUE;
}

CmdStatus cmd_member_history(Library *lib, Args *args)
{
    const char *id = cli_arg(args, 0, "Member ID: ");

    if (id == NULL || !cli_member_id_ok(id)) {
        return CMD_CONTINUE;
    }
    if (find_member(&lib->registry, id) == -1) {
        printf("ERROR: Member ID '%s' is not in the member registry.\n", id);
        return CMD_CONTINUE;
    }
    printf("\nLending history of %s (%s):", id, cli_member_name(lib, id));
    print_history(lib, id, NULL);
    return CMD_CONTINUE;
}

CmdStatus cmd_book_history(Library *lib, Args *args)
{
    const char *id = cli_arg(args, 0, "Book ID: ");

    if (id == NULL || !cli_book_id_ok(id)) {
        return CMD_CONTINUE;
    }
    int index = find_book(&lib->registry, id);
    if (index == -1) {
        printf("ERROR: Book ID '%s' is not in the book registry.\n", id);
        return CMD_CONTINUE;
    }
    printf("\nLending history of %s '%s':", id, lib->registry.books[index].title);
    print_history(lib, NULL, id);
    return CMD_CONTINUE;
}

/* Simulation and demo tools */

CmdStatus cmd_advance_time(Library *lib, Args *args)
{
    const char *text = cli_arg(args, 0, "Days to move the library clock forward (1-365): ");
    char when[20];
    int days;

    if (text == NULL) {
        return CMD_CONTINUE;
    }
    if (!cli_parse_int(text, 1, 365, &days)) {
        printf("ERROR: Enter a whole number of days from 1 to 365.\n");
        return CMD_CONTINUE;
    }

    /* The clock only ever moves forward, so block timestamps keep increasing. */
    lib->clock_offset += (long)days * DAY_SECONDS;
    printf("Library clock moved forward %d day%s: it is now %s.\n", days, days == 1 ? "" : "s",
           format_time(library_now(lib), when));
    lending_scan_overdue(lib);
    return CMD_CONTINUE;
}

/* Changes the chain in memory, three different ways, to show what catches each. */
CmdStatus cmd_tamper_demo(Library *lib, Args *args)
{
    const char *mode = args->argc > 0 ? args->argv[0] : "record";

    if (lib->chain.count < 2) {
        printf("Mine at least one lending block first so there is a block to tamper with.\n");
        return CMD_CONTINUE;
    }

    Block *last = &lib->chain.blocks[lib->chain.count - 1];

    if (strcmp(mode, "record") == 0) {
        printf("Changing the due date of record %s in block #1, in memory only, without "
               "re-hashing.\n", lib->chain.blocks[1].records[0].record_id);
        lib->chain.blocks[1].records[0].due_date += 30 * DAY_SECONDS;
    } else if (strcmp(mode, "hash") == 0) {
        printf("Changing who sealed block #1, in memory only, without re-mining it.\n");
        strcpy(lib->chain.blocks[1].sealed_by, "INTRUDER");
    } else if (strcmp(mode, "signature") == 0) {
        unsigned long attempts;
        printf("Rewriting record %s in the last block (#%d), then recomputing its Merkle root "
               "and re-mining the block, as an attacker without the member's private key "
               "would.\n", last->records[0].record_id, last->index);
        snprintf(last->records[0].member_name, sizeof(last->records[0].member_name),
                 "Someone Else");
        block_seal_records(last);
        last->nonce = 0;
        mine_attempts(last, (unsigned long)-1, &attempts);
        printf("Block #%d re-mined in %lu attempts: its hash and Merkle root are consistent "
               "again.\n", last->index, attempts);
    } else {
        printf("Usage: tamper_demo [record|hash|signature]\n");
        return CMD_CONTINUE;
    }

    library_validate(lib);
    printf("(Nothing was written to disk. Run chain_load to restore the clean chain.)\n");
    return CMD_CONTINUE;
}
