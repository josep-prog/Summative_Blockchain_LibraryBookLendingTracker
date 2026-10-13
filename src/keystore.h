#ifndef KEYSTORE_H
#define KEYSTORE_H

#include "crypto.h"

#define KEY_ID_SIZE 20

typedef struct {
    char id[KEY_ID_SIZE];
    EVP_PKEY *public_key;
    char public_hex[PUBKEY_HEX_SIZE];
} KeyEntry;

typedef struct {
    char dir[128];
    char passphrase[128];
    KeyEntry *entries;   /* heap array */
    int count;
    int capacity;
} KeyStore;

/* Creates the directory if needed; returns 0 on failure. */
int keystore_open(KeyStore *store, const char *dir, const char *passphrase);
void keystore_close(KeyStore *store);

/* Loads the identity's public key, generating a new key pair the first time. */
int keystore_ensure(KeyStore *store, const char *id, int *created);

const KeyEntry *keystore_find(const KeyStore *store, const char *id);

/* Decrypts the private key, signs, and frees the key again. */
int keystore_sign(const KeyStore *store, const char *id,
                  const unsigned char *data, size_t data_len,
                  unsigned char *signature, unsigned int *signature_len);

#endif
