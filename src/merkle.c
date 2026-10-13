#include <stdlib.h>
#include <string.h>

#include "merkle.h"

static void print_level(FILE *trace, int level, const unsigned char *nodes, int count)
{
    char hex[HASH_HEX_SIZE];

    for (int i = 0; i < count; i++) {
        bytes_to_hex(nodes + (size_t)i * HASH_BYTES, HASH_BYTES, hex);
        fprintf(trace, "      level %d node %d: %s\n", level, i, hex);
    }
}

/* Hashes the record hashes in pairs, level by level, until one hash (the root) is left. */
int merkle_root(const unsigned char leaves[][HASH_BYTES], int count,
                unsigned char root[HASH_BYTES], FILE *trace)
{
    if (count <= 0) {
        sha256_raw("", 0, root);
        return 1;
    }

    /* One working level, reduced in place until a single node is left. */
    unsigned char *level = malloc((size_t)count * HASH_BYTES);
    if (level == NULL) {
        return 0;
    }
    memcpy(level, leaves, (size_t)count * HASH_BYTES);

    int nodes = count;
    int depth = 0;
    if (trace != NULL) print_level(trace, depth, level, nodes);

    while (nodes > 1) {
        int parents = 0;

        for (int i = 0; i < nodes; i += 2) {
            unsigned char pair[2 * HASH_BYTES];
            const unsigned char *left = level + (size_t)i * HASH_BYTES;
            /* An odd node out is paired with itself. */
            const unsigned char *right = (i + 1 < nodes) ? left + HASH_BYTES : left;

            memcpy(pair, left, HASH_BYTES);
            memcpy(pair + HASH_BYTES, right, HASH_BYTES);
            sha256_raw(pair, sizeof(pair), level + (size_t)parents * HASH_BYTES);
            parents++;
        }

        nodes = parents;
        depth++;
        if (trace != NULL) print_level(trace, depth, level, nodes);
    }

    memcpy(root, level, HASH_BYTES);
    free(level);
    return 1;
}
