#include <stdio.h>
#include <string.h>
#include <time.h>

#include "library.h"
#include "storage.h"

time_t library_now(const Library *lib)
{
    return time(NULL) + (time_t)lib->clock_offset;
}

int library_is_admin(const Library *lib)
{
    return lib->user != NULL && strcmp(lib->user->role, "ADMIN") == 0;
}

void library_free(Library *lib)
{
    chain_free(&lib->chain);
    pending_free(&lib->pool);
    inventory_free(&lib->inventory);
    ledger_free(&lib->ledger);
}

/* Every member and librarian is an account; members start at 0 tokens. */
static int open_accounts(Library *lib)
{
    for (int i = 0; i < lib->registry.member_count; i++) {
        if (!ledger_open_account(&lib->ledger, lib->registry.members[i].member_id)) {
            return 0;
        }
    }
    for (int i = 0; i < lib->registry.librarian_count; i++) {
        if (!ledger_open_account(&lib->ledger, lib->registry.librarians[i].librarian_id)) {
            return 0;
        }
    }
    return ledger_open_account(&lib->ledger, FEE_ACCOUNT);
}

int library_boot(Library *lib, LedgerModel model, int model_given)
{
    char err[200] = "";
    int seal_ok = 1;
    int difficulty = lib->difficulty;      /* set by the caller for a new chain */

    /* Start from empty state, so this also works as a reload. */
    library_free(lib);
    chain_init(&lib->chain);
    pending_init(&lib->pool);
    inventory_init(&lib->inventory);
    ledger_init(&lib->ledger, model);
    lib->clock_offset = 0;
    lib->next_request_seq = 1;
    lib->locked = 0;
    lib->seal_ok = 1;
    for (int i = 0; i < lib->registry.member_count; i++) {
        lib->registry.members[i].fines_owed = 0;
        lib->registry.members[i].fines_paid = 0;
    }

    if (inventory_load_copies(&lib->inventory, lib->copies_file, &lib->registry) == 0) {
        return 0;
    }

    LoadStatus status = storage_load(lib, &seal_ok, err, sizeof(err));
    if (status == LOAD_CORRUPT) {
        printf("ERROR: '%s' exists but could not be read (%s).\n", lib->data_file, err);
        printf("Restore a good copy, or delete it to start a new chain.\n");
        return 0;
    }
    if (!open_accounts(lib)) {
        printf("ERROR: out of memory.\n");
        return 0;
    }

    if (status == LOAD_MISSING) {
        Block genesis;

        lib->difficulty = difficulty;
        create_genesis_block(&genesis, lib->difficulty, library_now(lib));
        if (!chain_append(&lib->chain, &genesis)) {
            printf("ERROR: out of memory.\n");
            return 0;
        }
        printf("No saved chain found: started a new blockchain (genesis mined at difficulty %d).\n",
               lib->difficulty);
        library_save(lib);
        return 1;
    }

    printf("Restored %d block%s, %d cop%s, %d pending request%s and the %s ledger from %s.\n",
           lib->chain.count, lib->chain.count == 1 ? "" : "s",
           lib->inventory.copy_count, lib->inventory.copy_count == 1 ? "y" : "ies",
           lib->pool.count, lib->pool.count == 1 ? "" : "s",
           ledger_model_name(lib->ledger.model), lib->data_file);

    if (model_given && model != lib->ledger.model) {
        printf("Transaction model switched from %s to %s: every balance was carried over.\n",
               ledger_model_name(lib->ledger.model), ledger_model_name(model));
        if (!ledger_convert(&lib->ledger, model)) {
            printf("ERROR: out of memory.\n");
            return 0;
        }
    }

    lib->seal_ok = seal_ok;
    if (!seal_ok) {
        printf("WARNING: file seal check FAILED: %s.\n", err);
    }

    /* The loaded chain must pass full validation before anything else is allowed. */
    library_validate(lib);
    return 1;
}

int library_validate(Library *lib)
{
    ChainFault fault;
    int valid = validate_chain(&lib->chain, &lib->registry, &lib->keys, &fault);

    if (valid) {
        printf("Blockchain is VALID - %d block%s: hashes, links, proof of work, Merkle roots, "
               "signatures and nonces all check out.\n",
               lib->chain.count, lib->chain.count == 1 ? "" : "s");
    } else {
        printf("Blockchain is INVALID - tampering detected!\n");
        if (fault.record >= 0) {
            printf("  Block #%d, record %d (%s): %s.\n", fault.block, fault.record + 1,
                   lib->chain.blocks[fault.block].records[fault.record].record_id, fault.reason);
        } else {
            printf("  Block #%d: %s.\n", fault.block, fault.reason);
        }
    }
    if (!lib->seal_ok) {
        printf("The saved file's seal is INVALID - state outside the chain cannot be trusted.\n");
    }

    lib->locked = !valid || !lib->seal_ok;
    if (lib->locked) {
        printf("The session is LOCKED: only viewing commands work, and nothing will be saved.\n");
        printf("Restore a good copy of %s and run chain_load.\n", lib->data_file);
    }
    return !lib->locked;
}

int library_save(Library *lib)
{
    char err[200];
    ChainFault fault;

    if (lib->locked) {
        return 0;
    }
    /* Only a valid chain is saved. */
    if (!validate_chain(&lib->chain, &lib->registry, &lib->keys, &fault)) {
        printf("WARNING: chain is invalid (block #%d: %s), so it was NOT saved to %s.\n",
               fault.block, fault.reason, lib->data_file);
        lib->locked = 1;
        return 0;
    }
    if (!storage_save(lib, err, sizeof(err))) {
        printf("WARNING: could not save to %s: %s.\n", lib->data_file, err);
        return 0;
    }
    return 1;
}
