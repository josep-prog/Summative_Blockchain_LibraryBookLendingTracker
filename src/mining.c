#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mining.h"

#define ALL_ATTEMPTS ((unsigned long)-1)

typedef struct {
    char id[ACCOUNT_ID_SIZE];
    unsigned long hash_rate;      /* attempts per round */
    unsigned long attempts;       /* total attempts */
    int blocks_found;
    long reward;                  /* hundredths of a coin */
} PoolMiner;

/* A block being mined, and the pool positions of the requests inside it. */
typedef struct {
    Block block;
    int picked[BLOCK_CAPACITY];
} Candidate;

static double elapsed_ms(clock_t start)
{
    return (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
}

void print_pending_pool(const Library *lib)
{
    const PendingPool *pool = &lib->pool;
    int suspicious = pending_count_status(pool, REQ_SUSPICIOUS);

    printf("\nPending pool: %d request%s (%d pending, %d suspicious), in mining priority order\n",
           pool->count, pool->count == 1 ? "" : "s", pool->count - suspicious, suspicious);
    if (pool->count == 0) {
        return;
    }

    printf("%-3s %-11s %-9s %-10s %-8s %-8s %-11s %-5s %-16s %-9s\n", "#", "Request", "Action",
           "Copy/Book", "Member", "Priority", "Status", "Nonce", "Requested", "Signature");
    printf("--- ----------- --------- ---------- -------- -------- ----------- ----- "
           "---------------- ---------\n");

    for (int i = 0; i < pool->count; i++) {
        const PendingRequest *request = &pool->items[i];
        char when[20];

        printf("%-3d %-11s %-9s %-10s %-8s %-8s %-11s %-5lu %-16s %-9s\n", i + 1,
               request->request_id, request->action_requested,
               request->copy_id[0] != '\0' ? request->copy_id :
               request->book_id[0] != '\0' ? request->book_id : "-",
               request->member_id[0] != '\0' ? request->member_id : "-",
               request->priority == PRIORITY_OVERDUE ? "OVERDUE" : "normal",
               request_status_name(request->status), request->record.nonce,
               format_time(request->record.timestamp, when),
               record_verify(&request->record, NULL) ? "VALID" : "INVALID");
        if (request->status == REQ_SUSPICIOUS) {
            printf("      flagged %s: %s\n", request->flag, request->flag_detail);
        }
    }
}

/* Requests of this signer already chosen for the block. */
static int picked_by_signer(const Library *lib, const Candidate *candidate, int count,
                            const char *signer_id)
{
    int matches = 0;

    for (int i = 0; i < count; i++) {
        if (strcmp(lib->pool.items[candidate->picked[i]].record.signer_id, signer_id) == 0) {
            matches++;
        }
    }
    return matches;
}

static int already_picked(const Candidate *candidate, int count, int index)
{
    for (int i = 0; i < count; i++) {
        if (candidate->picked[i] == index) {
            return 1;
        }
    }
    return 0;
}

/*
 * Builds the next block from the top requests of the pool. A request is taken
 * only when its nonce is the signer's next one, so one signer's requests are
 * confirmed in the order they were signed. Requests that no longer pass the
 * checks are dropped. Returns the number of records, 0 if nothing can be mined.
 */
static int build_candidate(Library *lib, const char *sealed_by, Candidate *candidate)
{
    int count;

    /* Dropping a request changes the pool, so the picks are then chosen again from here. */
restart:
    count = 0;
    pending_sort(&lib->pool);

    while (count < BLOCK_CAPACITY) {
        int found = -1;

        for (int i = 0; i < lib->pool.count && found == -1; i++) {
            const PendingRequest *request = &lib->pool.items[i];
            /* SUSPICIOUS requests are never mined. */
            if (request->status != REQ_PENDING || already_picked(candidate, count, i)) {
                continue;
            }

            unsigned long confirmed = ledger_nonce(&lib->ledger, request->record.signer_id);
            if (request->record.nonce < confirmed) {
                lending_drop_request(lib, i, "its nonce was already used (replay)");
                goto restart;
            }
            /* Next in line: the confirmed nonce plus this signer's requests already picked. */
            if (request->record.nonce == confirmed + (unsigned long)picked_by_signer(
                    lib, candidate, count, request->record.signer_id)) {
                found = i;
            }
        }
        if (found == -1) {
            break;
        }

        /* Verify the signature and the lending rules again before inclusion. */
        const LendingRecord *record = &lib->pool.items[found].record;
        const char *reason;
        char why[220];

        if (!record_verify(record, &reason)) {
            lending_drop_request(lib, found, reason);
            goto restart;
        }
        if (!request_check(lib, record, 1, why, sizeof(why))) {
            lending_drop_request(lib, found, why);
            goto restart;
        }

        /* Tokens already promised to earlier records of the same block. */
        long spent = 0;
        for (int i = 0; i < count; i++) {
            const LendingRecord *earlier = &lib->pool.items[candidate->picked[i]].record;
            if (strcmp(earlier->member_id, record->member_id) == 0) {
                spent += record_spend(earlier);
            }
        }
        if (record_spend(record) > 0 &&
            ledger_balance(&lib->ledger, record->member_id) < spent + record_spend(record)) {
            lending_drop_request(lib, found, "insufficient tokens for amount plus fee");
            goto restart;
        }

        candidate->picked[count++] = found;
    }

    if (count == 0) {
        return 0;
    }

    const Block *tip = &lib->chain.blocks[lib->chain.count - 1];
    Block *block = &candidate->block;
    time_t now = library_now(lib);

    memset(block, 0, sizeof(*block));
    block->index = tip->index + 1;
    /* Strictly later than the previous block. */
    block->timestamp = now > tip->timestamp ? now : tip->timestamp + 1;
    strcpy(block->previous_hash, tip->hash);
    snprintf(block->sealed_by, sizeof(block->sealed_by), "%s", sealed_by);
    block->difficulty = lib->difficulty;
    block->record_count = count;
    for (int i = 0; i < count; i++) {
        block->records[i] = lib->pool.items[candidate->picked[i]].record;
    }
    block_seal_records(block);       /* Merkle root and reward summary */

    printf("\nBlock #%d candidate: %d request%s taken from the top of the pool:", block->index,
           count, count == 1 ? "" : "s");
    for (int i = 0; i < count; i++) {
        printf(" %s", block->records[i].record_id);
    }
    printf("\n");
    return count;
}

/* Adds a mined block, applies its records, clears them from the pool and saves. */
static int confirm_block(Library *lib, const Candidate *candidate)
{
    const Block *mined = &candidate->block;

    if (!chain_append(&lib->chain, mined)) {
        printf("ERROR: out of memory - block not added.\n");
        return 0;
    }
    for (int i = 0; i < mined->record_count; i++) {
        lib->pool.items[candidate->picked[i]].status = REQ_CONFIRMED;
    }

    print_block(mined, 0);

    printf("Confirmed %d record%s:\n", mined->record_count, mined->record_count == 1 ? "" : "s");
    for (int i = 0; i < mined->record_count; i++) {
        lending_apply(lib, &mined->records[i]);
    }

    int removed = pending_remove_confirmed(&lib->pool);
    printf("%d confirmed request%s removed from the pending pool (%d left).\n", removed,
           removed == 1 ? "" : "s", lib->pool.count);

    library_save(lib);

    if (lib->ledger.model == MODEL_UTXO) {
        ledger_print_utxo_set(&lib->ledger);
    }
    return 1;
}

/* Says why mining cannot start, or returns 1. */
static int something_to_mine(const Library *lib)
{
    int suspicious = pending_count_status(&lib->pool, REQ_SUSPICIOUS);

    if (pending_count_status(&lib->pool, REQ_PENDING) > 0) {
        return 1;
    }
    printf("Nothing to mine: the pending pool has no PENDING requests");
    if (suspicious > 0) {
        printf(" (%d held as SUSPICIOUS - see fraud_review)", suspicious);
    }
    printf(".\n");
    return 0;
}

/* out_of_hashes: a cloud rental ended before the pool was empty. */
static void report_left_over(const Library *lib, int out_of_hashes)
{
    int pending = pending_count_status(&lib->pool, REQ_PENDING);
    int suspicious = pending_count_status(&lib->pool, REQ_SUSPICIOUS);

    if (pending > 0 && out_of_hashes) {
        printf("%d PENDING request%s still wait%s in the pool.\n", pending,
               pending == 1 ? "" : "s", pending == 1 ? "s" : "");
    } else if (pending > 0) {
        printf("%d PENDING request%s could not be mined yet: an earlier request of the same "
               "signer is still held.\n", pending, pending == 1 ? "" : "s");
    }
    if (suspicious > 0) {
        printf("%d SUSPICIOUS request%s stayed in the pool - see fraud_review.\n", suspicious,
               suspicious == 1 ? "" : "s");
    }
}

static void pay_miner(Library *lib, const char *account, long amount)
{
    char err[160];

    if (amount > 0 && !ledger_mint(&lib->ledger, NULL, account, amount, 0, err, sizeof(err))) {
        printf("WARNING: mining reward for %s not paid: %s.\n", account, err);
    }
}

/* Solo mining */

int mine_solo(Library *lib, const char *miner_id)
{
    Candidate candidate;
    int confirmed = 0;
    unsigned long total_attempts = 0;
    char amount[32];

    if (!something_to_mine(lib)) {
        return 0;
    }

    printf("\n=== SOLO MINING by %s (difficulty %d: hash must start with %d zero%s) ===\n",
           miner_id, lib->difficulty, lib->difficulty, lib->difficulty == 1 ? "" : "s");

    while (build_candidate(lib, miner_id, &candidate) > 0) {
        unsigned long attempts;
        clock_t start = clock();

        /* Proof of work: change the nonce until the hash is valid. */
        mine_attempts(&candidate.block, ALL_ATTEMPTS, &attempts);

        printf("Mined block #%d after %lu hash attempts (nonce %lu, %.1f ms)\n",
               candidate.block.index, attempts, candidate.block.nonce, elapsed_ms(start));
        if (!confirm_block(lib, &candidate)) {
            break;
        }
        confirmed++;
        total_attempts += attempts;
    }

    if (confirmed == 0) {
        /* Every PENDING request was dropped or waits behind a held one: say so. */
        report_left_over(lib, 0);
        library_save(lib);
        return 0;
    }

    long reward = (long)confirmed * BLOCK_REWARD * COIN;
    pay_miner(lib, miner_id, reward);

    printf("\n--- Solo mining summary ---\n");
    printf("Blocks confirmed      : %d\n", confirmed);
    printf("Total hash attempts   : %lu (average %.0f per block)\n",
           total_attempts, (double)total_attempts / confirmed);
    printf("Mining reward to %-6s: %s coins (%d x %d), on top of the member token rewards\n",
           miner_id, format_coins(reward, amount), confirmed, BLOCK_REWARD);
    report_left_over(lib, 0);
    library_save(lib);
    return confirmed;
}

/* Pool mining */

int mine_pool(Library *lib, int miner_count)
{
    Candidate candidate;
    int confirmed = 0;
    char amount[32];

    if (!something_to_mine(lib)) {
        return 0;
    }

    PoolMiner *miners = calloc((size_t)miner_count, sizeof(PoolMiner));
    if (miners == NULL) {
        printf("ERROR: out of memory.\n");
        return 0;
    }

    printf("\n=== POOL MINING with %d miners (difficulty %d) ===\n", miner_count, lib->difficulty);
    for (int m = 0; m < miner_count; m++) {
        snprintf(miners[m].id, sizeof(miners[m].id), "POOL-M%d", m + 1);
        miners[m].hash_rate = POOL_MIN_HASH_RATE +
                              (unsigned long)rand() % (POOL_MAX_HASH_RATE - POOL_MIN_HASH_RATE + 1);
        printf("  %s hash rate: %lu attempts/round\n", miners[m].id, miners[m].hash_rate);
    }

    while (build_candidate(lib, "POOL", &candidate) > 0) {
        int finder = -1;
        int rounds = 0;
        unsigned long next_nonce = 0;
        Block winning = candidate.block;

        /* All miners hash at the same time on different nonces; the earliest valid hash wins. */
        while (finder == -1) {
            unsigned long used[MAX_POOL_MINERS];
            double win_time = 2.0;     /* later than any attempt in this round */
            rounds++;

            for (int m = 0; m < miner_count; m++) {
                Block attempt = candidate.block;
                strcpy(attempt.sealed_by, miners[m].id);
                attempt.nonce = next_nonce;
                next_nonce += miners[m].hash_rate;

                if (mine_attempts(&attempt, miners[m].hash_rate, &used[m])) {
                    /* How far through the round this miner was (0 to 1); the smallest wins. */
                    double found_at = (double)used[m] / (double)miners[m].hash_rate;
                    if (found_at < win_time) {
                        win_time = found_at;
                        finder = m;
                        winning = attempt;
                    }
                }
            }

            for (int m = 0; m < miner_count; m++) {
                unsigned long by_win = (unsigned long)(win_time * (double)miners[m].hash_rate);
                miners[m].attempts += (m == finder || used[m] < by_win) ? used[m] : by_win;
            }
        }
        candidate.block = winning;
        miners[finder].blocks_found++;

        printf("Block #%d found by %s in round %d (nonce %lu)\n",
               winning.index, miners[finder].id, rounds, winning.nonce);
        if (!confirm_block(lib, &candidate)) {
            break;
        }
        confirmed++;
    }

    if (confirmed == 0) {
        free(miners);
        report_left_over(lib, 0);
        library_save(lib);
        return 0;
    }

    unsigned long total_attempts = 0;
    for (int m = 0; m < miner_count; m++) total_attempts += miners[m].attempts;

    long gross = (long)confirmed * BLOCK_REWARD * COIN;
    long pool_fee = gross * POOL_FEE_PERCENT / 100;
    long distributable = gross - pool_fee;
    long paid = 0;
    int top = 0;

    /* share = attempts / total attempts * reward, rounded down */
    for (int m = 0; m < miner_count; m++) {
        miners[m].reward = (long)((long long)distributable * (long long)miners[m].attempts /
                                  (long long)total_attempts);
        paid += miners[m].reward;
        if (miners[m].attempts > miners[top].attempts) top = m;
    }
    miners[top].reward += distributable - paid;   /* remainder goes to the top miner */

    pay_miner(lib, POOL_OPERATOR_ACCOUNT, pool_fee);
    for (int m = 0; m < miner_count; m++) {
        pay_miner(lib, miners[m].id, miners[m].reward);
    }

    printf("\n--- Pool reward sharing ---\n");
    printf("Blocks confirmed : %d\n", confirmed);
    printf("Gross reward     : %s coins\n", format_coins(gross, amount));
    printf("Pool fee (%d%%)    : %s coins -> %s\n", POOL_FEE_PERCENT,
           format_coins(pool_fee, amount), POOL_OPERATOR_ACCOUNT);
    printf("To distribute    : %s coins\n\n", format_coins(distributable, amount));

    printf("+----------+-----------+------------+--------+---------+-------------+\n");
    printf("| Miner ID | Hash rate |   Attempts | Blocks | Share %% |      Reward |\n");
    printf("+----------+-----------+------------+--------+---------+-------------+\n");
    for (int m = 0; m < miner_count; m++) {
        printf("| %-8s | %9lu | %10lu | %6d | %6.2f%% | %11s |\n",
               miners[m].id, miners[m].hash_rate, miners[m].attempts, miners[m].blocks_found,
               100.0 * (double)miners[m].attempts / (double)total_attempts,
               format_coins(miners[m].reward, amount));
    }
    printf("+----------+-----------+------------+--------+---------+-------------+\n");
    printf("| TOTAL    |           | %10lu | %6d | 100.00%% | %11s |\n",
           total_attempts, confirmed, format_coins(distributable, amount));
    printf("+----------+-----------+------------+--------+---------+-------------+\n");

    free(miners);
    report_left_over(lib, 0);
    library_save(lib);
    return confirmed;
}

/* Cloud mining */

int mine_cloud(Library *lib, const char *renter_id, int rounds)
{
    Candidate candidate;
    int have_block = 0;            /* block still being mined */
    int confirmed = 0;
    int first_loss_round = 0;
    long rental_fee = (long)CLOUD_RENTAL_FEE * COIN;
    long rental_paid = 0;          /* what the renter could pay */
    long round_gross[MAX_RENTAL_ROUNDS];
    int round_blocks[MAX_RENTAL_ROUNDS];
    char a[32], b[32], c[32], d[32];

    if (!something_to_mine(lib)) {
        return 0;
    }

    printf("\n=== CLOUD MINING: %s rents %lu hashes/round for %d round%s at %s coins/round ===\n",
           renter_id, (unsigned long)CLOUD_HASHES_PER_ROUND, rounds, rounds == 1 ? "" : "s",
           format_coins(rental_fee, a));
    printf("Difficulty %d, %d request%s in the pool.\n", lib->difficulty, lib->pool.count,
           lib->pool.count == 1 ? "" : "s");

    for (int r = 0; r < rounds; r++) {
        unsigned long budget = CLOUD_HASHES_PER_ROUND;
        round_blocks[r] = 0;

        /* The rig mines pending requests until this round's hashes run out. */
        while (budget > 0) {
            /* A block not found in one round is kept and mined further in the next. */
            if (!have_block) {
                if (build_candidate(lib, "CLOUD-RIG", &candidate) == 0) break;
                have_block = 1;
            }
            unsigned long used;
            int found = mine_attempts(&candidate.block, budget, &used);
            budget -= used;

            if (found) {
                printf("[Round %d] Rented rig mined block #%d (nonce %lu)\n", r + 1,
                       candidate.block.index, candidate.block.nonce);
                have_block = 0;
                if (!confirm_block(lib, &candidate)) break;
                round_blocks[r]++;
                confirmed++;
            }
        }

        /* Each round the mining reward is credited, then the fixed rental fee deducted. */
        round_gross[r] = (long)round_blocks[r] * BLOCK_REWARD * COIN;
        pay_miner(lib, renter_id, round_gross[r]);
        rental_paid += ledger_charge(&lib->ledger, renter_id, CLOUD_PROVIDER_ACCOUNT, rental_fee);
    }

    if (have_block) {
        printf("\nRental ended mid-block: block #%d was not found in the hashes rented.\n",
               candidate.block.index);
    }

    printf("\n--- Cloud mining earnings ---\n");
    printf("+-------+--------+-----------+------------+-----------+--------------+\n");
    printf("| Round | Blocks |    Reward | Rental fee |       Net | Cumul. net   |\n");
    printf("+-------+--------+-----------+------------+-----------+--------------+\n");

    long cumulative_rewards = 0, cumulative_fees = 0;
    for (int r = 0; r < rounds; r++) {
        cumulative_rewards += round_gross[r];
        cumulative_fees += rental_fee;

        printf("| %5d | %6d | %9s | %10s | %9s | %12s |%s\n", r + 1, round_blocks[r],
               format_coins(round_gross[r], a), format_coins(rental_fee, b),
               format_coins(round_gross[r] - rental_fee, c),
               format_coins(cumulative_rewards - cumulative_fees, d),
               cumulative_fees > cumulative_rewards ? "  <- fees exceed rewards" : "");

        if (cumulative_fees > cumulative_rewards && first_loss_round == 0) {
            first_loss_round = r + 1;
        }
    }
    printf("+-------+--------+-----------+------------+-----------+--------------+\n");

    printf("Gross earnings  : %s coins (%d block%s x %d)\n", format_coins(cumulative_rewards, a),
           confirmed, confirmed == 1 ? "" : "s", BLOCK_REWARD);
    printf("Total fees paid : %s coins (%d round%s x %s)\n", format_coins(cumulative_fees, b),
           rounds, rounds == 1 ? "" : "s", format_coins(rental_fee, c));
    printf("Net profit      : %s coins\n", format_coins(cumulative_rewards - cumulative_fees, d));
    printf("Rental deducted from %s : %s coins -> %s\n", renter_id,
           format_coins(rental_paid, a), CLOUD_PROVIDER_ACCOUNT);
    if (rental_paid < cumulative_fees) {
        printf("Rental still owed : %s coins (%s ran out of tokens; a balance cannot go negative)\n",
               format_coins(cumulative_fees - rental_paid, b), renter_id);
    }
    printf("%s balance is now %s coins\n", renter_id,
           format_coins(ledger_balance(&lib->ledger, renter_id), c));

    if (cumulative_rewards < cumulative_fees) {
        printf("\nWARNING: this rental is UNPROFITABLE - cumulative fees exceeded cumulative "
               "rewards from round %d.\n", first_loss_round);
    } else if (first_loss_round != 0) {
        /* It was losing money at some round, but later blocks covered the fees. */
        printf("\nWARNING: this rental was UNPROFITABLE at round %d - cumulative fees exceeded "
               "cumulative rewards there, although it ended in profit.\n", first_loss_round);
    }

    report_left_over(lib, 1);
    library_save(lib);
    return confirmed;
}

/* Benchmark */

void mining_benchmark(void)
{
    Block block;

    printf("\nProof-of-work effort by difficulty (same test block each time):\n");
    printf("%-10s %-10s %12s %12s  %s\n", "Difficulty", "Target", "Attempts", "Time (ms)", "Hash");
    printf("---------- ---------- ------------ ------------  ----------------\n");

    for (int d = MIN_DIFFICULTY; d <= MAX_DIFFICULTY; d++) {
        unsigned long attempts;
        char target[12] = "";

        memset(&block, 0, sizeof(block));
        block.index = 1;
        block.timestamp = 1700000000;
        strcpy(block.sealed_by, "BENCHMARK");
        memset(block.previous_hash, '0', 64);
        block_seal_records(&block);
        block.difficulty = d;

        for (int i = 0; i < d; i++) target[i] = '0';
        strcat(target, "...");

        clock_t start = clock();
        mine_attempts(&block, ALL_ATTEMPTS, &attempts);
        printf("%-10d %-10s %12lu %12.1f  %.16s...\n", d, target, attempts, elapsed_ms(start), block.hash);
    }
    printf("Each extra zero multiplies the expected work by 16 (one hex digit).\n");
}
