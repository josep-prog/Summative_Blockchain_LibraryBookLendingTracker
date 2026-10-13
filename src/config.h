#ifndef CONFIG_H
#define CONFIG_H

#define COIN 100L


#define DAY_SECONDS             86400L
#define LOAN_DAYS               14
#define MAX_LOAN_DAYS           30
#define RENEWAL_DAYS            7
#define MAX_RENEWALS            2
#define FINE_PER_DAY            (2 * COIN)

#define MAX_CONCURRENT_BORROWS  3
#define REBORROW_WINDOW_SECONDS DAY_SECONDS

/* Blocks are batched by a fixed size: up to BLOCK_CAPACITY records each. */
#define BLOCK_CAPACITY          4

/* Proof of work: difficulty = number of leading '0' in the block hash. */
#define MIN_DIFFICULTY          1
#define MAX_DIFFICULTY          4
#define DEFAULT_DIFFICULTY      2

/* Token rewards for a confirmed return, in whole coins. */
#define REWARD_ON_TIME          10
#define REWARD_LATE             5

/* Mining. */
#define BLOCK_REWARD            50
#define POOL_FEE_PERCENT        2
#define MIN_POOL_MINERS         2
#define MAX_POOL_MINERS         8
#define DEFAULT_POOL_MINERS     4
#define POOL_MIN_HASH_RATE      50
#define POOL_MAX_HASH_RATE      500
#define CLOUD_HASHES_PER_ROUND  600
#define CLOUD_RENTAL_FEE        25
#define MAX_RENTAL_ROUNDS       5

#endif
