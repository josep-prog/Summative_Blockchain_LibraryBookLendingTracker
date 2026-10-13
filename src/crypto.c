#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <openssl/core_names.h>
#include <openssl/params.h>

#include "crypto.h"

EVP_PKEY *generate_key_pair(void)
{
    return EVP_EC_gen(KEY_CURVE);
}

int sign_data(
    EVP_PKEY *private_key,
    const unsigned char *data,
    size_t data_len,
    unsigned char *signature,
    size_t *signature_len
)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        return 0;
    }

    /* The final step is called twice: first to learn the signature's length, then to write it. */
    size_t needed = 0;
    int ok = EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, private_key) > 0 &&
             EVP_DigestSignUpdate(ctx, data, data_len) > 0 &&
             EVP_DigestSignFinal(ctx, NULL, &needed) > 0 &&
             needed <= SIGNATURE_MAX;

    if (ok) {
        *signature_len = needed;
        ok = EVP_DigestSignFinal(ctx, signature, signature_len) > 0;
    }

    EVP_MD_CTX_free(ctx);
    return ok;
}

int verify_signature(
    EVP_PKEY *public_key,
    const unsigned char *data,
    size_t data_len,
    const unsigned char *signature,
    size_t signature_len
)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL) {
        return 0;
    }

    int ok = EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, public_key) > 0 &&
             EVP_DigestVerifyUpdate(ctx, data, data_len) > 0 &&
             EVP_DigestVerifyFinal(ctx, signature, signature_len) == 1;

    EVP_MD_CTX_free(ctx);
    return ok;
}

int save_private_key(EVP_PKEY *key_pair, const char *filename, const char *passphrase)
{
    /* Owner-only from the start. */
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return 0;
    }
    fchmod(fd, 0600);   /* fix old permissions */

    FILE *file = fdopen(fd, "w");
    if (file == NULL) {
        close(fd);
        return 0;
    }

    int ok = PEM_write_PrivateKey(file, key_pair, EVP_aes_256_cbc(),
                                  (unsigned char *)passphrase, (int)strlen(passphrase),
                                  NULL, NULL);
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok == 1;
}

/* OpenSSL asks for the passphrase through this callback. */
static int passphrase_callback(char *buf, int size, int rwflag, void *userdata)
{
    (void)rwflag;
    const char *passphrase = userdata;

    int len = (int)strlen(passphrase);
    if (len > size) {
        len = size;
    }
    memcpy(buf, passphrase, (size_t)len);
    return len;
}

EVP_PKEY *load_private_key(const char *filename, const char *passphrase)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        return NULL;
    }

    EVP_PKEY *key = PEM_read_PrivateKey(file, NULL, passphrase_callback, (void *)passphrase);
    fclose(file);
    return key;
}

int save_public_key(EVP_PKEY *key, const char *filename)
{
    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        return 0;
    }

    int ok = PEM_write_PUBKEY(file, key);
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok == 1;
}

/* Returns NULL if the file is missing or is not a public key. */
EVP_PKEY *load_public_key(const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        return NULL;
    }

    EVP_PKEY *key = PEM_read_PUBKEY(file, NULL, NULL, NULL);
    fclose(file);
    return key;
}

int public_key_to_hex(EVP_PKEY *key, char out_hex[PUBKEY_HEX_SIZE])
{
    unsigned char point[PUBKEY_BYTES];
    size_t len = 0;

    if (EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY,
                                        point, sizeof(point), &len) != 1 || len == 0) {
        return 0;
    }
    bytes_to_hex(point, len, out_hex);
    return 1;
}

/* Rebuilds a verification key from the hex point stored in a record. */
EVP_PKEY *public_key_from_hex(const char *hex)
{
    unsigned char point[PUBKEY_BYTES];
    size_t len;
    char curve[] = KEY_CURVE;
    EVP_PKEY *key = NULL;

    if (!hex_to_bytes(hex, point, sizeof(point), &len) || len == 0) {
        return NULL;
    }

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (ctx == NULL) {
        return NULL;
    }

    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, curve, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point, len),
        OSSL_PARAM_construct_end()
    };

    if (EVP_PKEY_fromdata_init(ctx) <= 0 ||
        EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
        key = NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    return key;
}

#define PIN_HASH_ITERATIONS 100000

/* A slow hash on purpose, mixed with the librarian's ID, so stored PINs are hard to guess. */
int hash_pin(const char *librarian_id, const char *pin, char out_hex[HASH_HEX_SIZE])
{
    unsigned char digest[HASH_BYTES];

    if (PKCS5_PBKDF2_HMAC(pin, (int)strlen(pin),
                          (const unsigned char *)librarian_id, (int)strlen(librarian_id),
                          PIN_HASH_ITERATIONS, EVP_sha256(),
                          (int)sizeof(digest), digest) != 1) {
        return 0;
    }

    bytes_to_hex(digest, sizeof(digest), out_hex);
    return 1;
}

int verify_pin(const char *librarian_id, const char *pin, const char *stored_hex)
{
    char computed[HASH_HEX_SIZE];

    if (strlen(stored_hex) != 64 || !hash_pin(librarian_id, pin, computed)) {
        return 0;
    }
    /* Constant-time compare. */
    return CRYPTO_memcmp(computed, stored_hex, 64) == 0;
}

void sha256_raw(const void *data, size_t data_len, unsigned char out[HASH_BYTES])
{
    SHA256((const unsigned char *)data, data_len, out);
}

void sha256_hex(const void *data, size_t data_len, char out_hex[HASH_HEX_SIZE])
{
    unsigned char digest[HASH_BYTES];
    sha256_raw(data, data_len, digest);
    bytes_to_hex(digest, sizeof(digest), out_hex);
}

void bytes_to_hex(const unsigned char *bytes, size_t len, char *out_hex)
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        out_hex[i * 2]     = digits[bytes[i] >> 4];
        out_hex[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    out_hex[len * 2] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int hex_to_bytes(const char *hex, unsigned char *out, size_t max_len, size_t *out_len)
{
    size_t hex_len = strlen(hex);

    if (hex_len % 2 != 0 || hex_len / 2 > max_len) {
        return 0;
    }
    for (size_t i = 0; i < hex_len / 2; i++) {
        int high = hex_value(hex[i * 2]);
        int low  = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return 0;
        }
        out[i] = (unsigned char)(high * 16 + low);
    }
    *out_len = hex_len / 2;
    return 1;
}
