#ifndef MERKLE_H
#define MERKLE_H

#include <stdio.h>

#include "crypto.h"

/*
 * Merkle tree built from scratch over SHA-256 leaf hashes.
 * A parent is SHA-256(left || right); a level with an odd number of nodes pairs
 * its last node with itself. With no leaves, the root is SHA-256 of nothing.
 * If trace is not NULL, every level is printed to it, leaves first.
 * Returns 0 only if memory ran out.
 */
int merkle_root(const unsigned char leaves[][HASH_BYTES], int count,
                unsigned char root[HASH_BYTES], FILE *trace);

#endif
