#ifndef MINING_H
#define MINING_H

#include "library.h"

/*
 * Mining confirms pending requests. A miner takes the top BLOCK_CAPACITY
 * requests by priority, checks each one again, builds a block with their Merkle
 * root and searches for a nonce that meets the difficulty. Only then are the
 * records applied to the inventory and the ledger, and removed from the pool.
 */

void print_pending_pool(const Library *lib);

/* Each returns the number of blocks it confirmed. */
int mine_solo(Library *lib, const char *miner_id);
int mine_pool(Library *lib, int miner_count);
int mine_cloud(Library *lib, const char *renter_id, int rounds);

/* Mines a test block at difficulty 1..4 to compare effort. */
void mining_benchmark(void);

#endif
