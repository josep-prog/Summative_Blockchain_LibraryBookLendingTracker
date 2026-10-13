#ifndef CRYPTO_H
#define CRYPTO_H

#include <openssl/evp.h>
#include <stddef.h>


#define KEY_CURVE       "secp256k1"
#define SIGNATURE_MAX   72
#define PUBKEY_BYTES    65
#define PUBKEY_HEX_SIZE (PUBKEY_BYTES * 2 + 1)
#define HASH_BYTES      32
#define HASH_HEX_SIZE   (HASH_BYTES * 2 + 1)

#define MIN_PASSPHRASE_LENGTH 4

EVP_PKEY *generate_key_pair(void);


int save_private_key(EVP_PKEY *key_pair, const char *filename, const char *passphrase);
/* Returns NULL if the file is missing, damaged or the passphrase is wrong. */
EVP_PKEY *load_private_key(const char *filename, const char *passphrase);

int save_public_key(EVP_PKEY *key, const char *filename);
EVP_PKEY *load_public_key(const char *filename);

/* The public key as hex text: this is the identity written on the chain. */
int public_key_to_hex(EVP_PKEY *key, char out_hex[PUBKEY_HEX_SIZE]);
EVP_PKEY *public_key_from_hex(const char *hex);

int sign_data(
    EVP_PKEY *private_key,
    const unsigned char *data,
    size_t data_len,
    unsigned char *signature,
    size_t *signature_len
);

int verify_signature(
    EVP_PKEY *public_key,
    const unsigned char *data,
    size_t data_len,
    const unsigned char *signature,
    size_t signature_len
);

/* PBKDF2-HMAC-SHA256 of the PIN, salted with the librarian ID. */
int hash_pin(const char *librarian_id, const char *pin, char out_hex[HASH_HEX_SIZE]);
int verify_pin(const char *librarian_id, const char *pin, const char *stored_hex);

void sha256_raw(const void *data, size_t data_len, unsigned char out[HASH_BYTES]);
/* SHA-256 as 64 hex characters. */
void sha256_hex(const void *data, size_t data_len, char out_hex[HASH_HEX_SIZE]);

void bytes_to_hex(const unsigned char *bytes, size_t len, char *out_hex);
/* Returns 0 if the text is not hex or does not fit in max_len bytes. */
int hex_to_bytes(const char *hex, unsigned char *out, size_t max_len, size_t *out_len);

#endif
