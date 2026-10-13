#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/crypto.h>

#include "keystore.h"

#define PATH_SIZE 192

static void key_paths(const KeyStore *store, const char *id,
                      char private_path[PATH_SIZE], char public_path[PATH_SIZE])
{
    snprintf(private_path, PATH_SIZE, "%.127s/%.19s.key.pem", store->dir, id);
    snprintf(public_path, PATH_SIZE, "%.127s/%.19s.pub.pem", store->dir, id);
}

int keystore_open(KeyStore *store, const char *dir, const char *passphrase)
{
    memset(store, 0, sizeof(*store));
    snprintf(store->dir, sizeof(store->dir), "%s", dir);
    snprintf(store->passphrase, sizeof(store->passphrase), "%s", passphrase);

    return mkdir(dir, 0700) == 0 || errno == EEXIST;
}

void keystore_close(KeyStore *store)
{
    for (int i = 0; i < store->count; i++) {
        EVP_PKEY_free(store->entries[i].public_key);
    }
    free(store->entries);
    OPENSSL_cleanse(store->passphrase, sizeof(store->passphrase));
    memset(store, 0, sizeof(*store));
}

const KeyEntry *keystore_find(const KeyStore *store, const char *id)
{
    for (int i = 0; i < store->count; i++) {
        if (strcmp(store->entries[i].id, id) == 0) {
            return &store->entries[i];
        }
    }
    return NULL;
}

/* Loads this identity's public key, creating a new key pair first if it has none. */
int keystore_ensure(KeyStore *store, const char *id, int *created)
{
    char private_path[PATH_SIZE], public_path[PATH_SIZE];

    if (created != NULL) *created = 0;
    if (keystore_find(store, id) != NULL) {
        return 1;
    }
    if (strlen(id) == 0 || strlen(id) >= KEY_ID_SIZE) {
        return 0;
    }
    key_paths(store, id, private_path, public_path);

    EVP_PKEY *public_key = load_public_key(public_path);

    if (public_key == NULL) {
        /* No public key yet: derive it from the private key, or make a new pair. */
        EVP_PKEY *pair;

        if (access(private_path, F_OK) == 0) {
            pair = load_private_key(private_path, store->passphrase);
        } else {
            pair = generate_key_pair();
            if (pair != NULL && !save_private_key(pair, private_path, store->passphrase)) {
                EVP_PKEY_free(pair);
                pair = NULL;
            }
            if (pair != NULL && created != NULL) *created = 1;
        }
        if (pair == NULL) {
            return 0;
        }

        int saved = save_public_key(pair, public_path);
        EVP_PKEY_free(pair);
        if (!saved || (public_key = load_public_key(public_path)) == NULL) {
            return 0;
        }
    }

    if (store->count == store->capacity) {
        int new_capacity = store->capacity == 0 ? 16 : store->capacity * 2;
        KeyEntry *grown = realloc(store->entries, (size_t)new_capacity * sizeof(KeyEntry));
        if (grown == NULL) {
            EVP_PKEY_free(public_key);
            return 0;
        }
        store->entries = grown;
        store->capacity = new_capacity;
    }

    KeyEntry *entry = &store->entries[store->count];
    memset(entry, 0, sizeof(*entry));
    strcpy(entry->id, id);
    entry->public_key = public_key;
    if (!public_key_to_hex(public_key, entry->public_hex)) {
        EVP_PKEY_free(public_key);
        return 0;
    }
    store->count++;
    return 1;
}

int keystore_sign(const KeyStore *store, const char *id,
                  const unsigned char *data, size_t data_len,
                  unsigned char *signature, unsigned int *signature_len)
{
    char private_path[PATH_SIZE], public_path[PATH_SIZE];
    const KeyEntry *entry = keystore_find(store, id);

    if (entry == NULL) {
        return 0;
    }
    key_paths(store, id, private_path, public_path);

    EVP_PKEY *private_key = load_private_key(private_path, store->passphrase);
    if (private_key == NULL) {
        return 0;
    }

    /* The private key must belong to the public key that identifies this signer. */
    size_t len = 0;
    int ok = EVP_PKEY_eq(private_key, entry->public_key) == 1 &&
             sign_data(private_key, data, data_len, signature, &len);

    EVP_PKEY_free(private_key);
    *signature_len = (unsigned int)len;
    return ok;
}
