#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "library.h"
#include "storage.h"
#include "cli.h"

#define BOOKS_FILE      "data/books.txt"
#define COPIES_FILE     "data/copies.txt"
#define MEMBERS_FILE    "data/members.txt"
#define LIBRARIANS_FILE "data/librarians.txt"
#define DATA_FILE       "data/library.dat"
#define KEYS_DIR        "data/keys"

#define MAX_LOGIN_ATTEMPTS 3

/* Returns the librarian's index, or -1 after too many tries. */
static int login(const Registry *registry)
{
    char id[64], pin[64];

    for (int attempt = 1; attempt <= MAX_LOGIN_ATTEMPTS; attempt++) {
        if (!cli_prompt_line("Librarian ID: ", id, sizeof(id)) ||
            !cli_prompt_secret("PIN: ", pin, sizeof(pin))) {
            return -1;
        }

        int index = find_librarian(registry, id);
        int ok = index != -1 && verify_pin(id, pin, registry->librarians[index].pin_hash);
        memset(pin, 0, sizeof(pin));

        if (ok) {
            return index;
        }
        printf("ERROR: Invalid librarian ID or PIN (%d attempt(s) left).\n",
               MAX_LOGIN_ATTEMPTS - attempt);
    }
    return -1;
}

/* Passphrase from LIBRARY_KEY_PASSPHRASE, or asked for. */
static int get_passphrase(char *buf, size_t size, int *typed)
{
    const char *env = getenv("LIBRARY_KEY_PASSPHRASE");
    *typed = (env == NULL);
    if (env != NULL) {
        snprintf(buf, size, "%s", env);
    } else if (!cli_prompt_secret("Keystore passphrase: ", buf, size)) {
        return 0;
    }

    if (strlen(buf) < MIN_PASSPHRASE_LENGTH) {
        printf("ERROR: The keystore passphrase must be at least %d characters.\n",
               MIN_PASSPHRASE_LENGTH);
        return 0;
    }
    return 1;
}

#define KEYS_READY   1
#define KEYS_FAILED  0
#define KEYS_LOCKED -1    /* the passphrase does not open the keystore */

/*
 * Makes sure every member and librarian has an ECDSA key pair, and that the
 * passphrase really opens the keystore.
 */
static int prepare_keys(Library *lib)
{
    const char *user = lib->user->librarian_id;
    unsigned char probe[] = "keystore check";
    unsigned char signature[SIGNATURE_MAX];
    unsigned int signature_len;
    int created = 0, generated = 0;

    /* The librarian's own key shows whether the passphrase is right. */
    if (!keystore_ensure(&lib->keys, user, &created) ||
        !keystore_sign(&lib->keys, user, probe, sizeof(probe), signature, &signature_len)) {
        return KEYS_LOCKED;
    }
    generated += created;

    for (int i = 0; i < lib->registry.member_count; i++) {
        if (!keystore_ensure(&lib->keys, lib->registry.members[i].member_id, &created)) {
            printf("ERROR: Could not load or create a key for %s.\n",
                   lib->registry.members[i].member_id);
            return KEYS_FAILED;
        }
        generated += created;
    }
    for (int i = 0; i < lib->registry.librarian_count; i++) {
        if (!keystore_ensure(&lib->keys, lib->registry.librarians[i].librarian_id, &created)) {
            printf("ERROR: Could not load or create a key for %s.\n",
                   lib->registry.librarians[i].librarian_id);
            return KEYS_FAILED;
        }
        generated += created;
    }

    printf("Keystore %s: %d identities", KEYS_DIR, lib->keys.count);
    if (generated > 0) {
        printf(", %d new ECDSA (%s) key pair%s generated", generated, KEY_CURVE,
               generated == 1 ? "" : "s");
    }
    printf(".\n");
    return KEYS_READY;
}

static int parse_model(const char *text, LedgerModel *model)
{
    if (strcmp(text, "utxo") == 0 || strcmp(text, "UTXO") == 0 || strcmp(text, "1") == 0) {
        *model = MODEL_UTXO;
        return 1;
    }
    if (strcmp(text, "account") == 0 || strcmp(text, "ACCOUNT") == 0 || strcmp(text, "2") == 0) {
        *model = MODEL_ACCOUNT;
        return 1;
    }
    return 0;
}

static void usage(const char *program)
{
    printf("Usage: %s [--model utxo|account] [--difficulty 1-4]\n", program);
    printf("       %s --hash-pin <librarian_id> <pin>\n", program);
}

/* Too large for the stack: it holds the registries. */
static Library library;

int main(int argc, char *argv[])
{
    /* Helper to create librarians.txt entries. */
    if (argc == 4 && strcmp(argv[1], "--hash-pin") == 0) {
        char hex[HASH_HEX_SIZE];
        if (!hash_pin(argv[2], argv[3], hex)) {
            printf("ERROR: Could not hash PIN.\n");
            return 1;
        }
        printf("%s\n", hex);
        return 0;
    }

    /* Session options. */
    int model_given = 0, difficulty_given = 0;
    LedgerModel model = MODEL_UTXO;
    int difficulty = DEFAULT_DIFFICULTY;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc && parse_model(argv[i + 1], &model)) {
            model_given = 1;
            i++;
        } else if (strcmp(argv[i], "--difficulty") == 0 && i + 1 < argc &&
                   cli_parse_int(argv[i + 1], MIN_DIFFICULTY, MAX_DIFFICULTY, &difficulty)) {
            difficulty_given = 1;
            i++;
        } else {
            printf("ERROR: unrecognised or invalid option '%s'.\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    Library *lib = &library;
    lib->books_file = BOOKS_FILE;
    lib->copies_file = COPIES_FILE;
    lib->members_file = MEMBERS_FILE;
    lib->data_file = DATA_FILE;
    lib->difficulty = difficulty;

    /* Registries: each file must exist and hold at least one valid record. */
    int books = load_books(BOOKS_FILE, &lib->registry);
    int members = load_members(MEMBERS_FILE, &lib->registry);
    int librarians = load_librarians(LIBRARIANS_FILE, &lib->registry);
    if (books == 0 || members == 0 || librarians == 0) {
        return 1;
    }
    printf("Loaded %d books, %d members and %d librarians.\n", books, members, librarians);

    int user_index = login(&lib->registry);
    if (user_index == -1) {
        printf("Access denied.\n");
        return 1;
    }
    lib->user = &lib->registry.librarians[user_index];
    printf("Welcome, %s (%s).\n", lib->user->full_name, lib->user->role);

    /* A mistyped passphrase can be tried again; a wrong one from the environment cannot. */
    int keys = KEYS_LOCKED;
    for (int attempt = 1; keys == KEYS_LOCKED; attempt++) {
        char passphrase[128];
        int typed;
        if (!get_passphrase(passphrase, sizeof(passphrase), &typed)) {
            return 1;
        }
        int opened = keystore_open(&lib->keys, KEYS_DIR, passphrase);
        memset(passphrase, 0, sizeof(passphrase));
        if (!opened) {
            printf("ERROR: Could not create the keystore directory %s.\n", KEYS_DIR);
            return 1;
        }

        keys = prepare_keys(lib);
        if (keys != KEYS_LOCKED) {
            break;
        }
        keystore_close(&lib->keys);
        if (!typed || attempt == MAX_LOGIN_ATTEMPTS) {
            printf("ERROR: Could not unlock %s's private key in %s - wrong passphrase or damaged "
                   "key file.\n", lib->user->librarian_id, KEYS_DIR);
            printf("To start again with new keys and an empty chain, run: make fresh\n");
            return 1;
        }
        printf("ERROR: Wrong keystore passphrase (%d attempt(s) left).\n",
               MAX_LOGIN_ATTEMPTS - attempt);
    }

    int exit_code = 1;
    if (keys != KEYS_READY) {
        goto cleanup;
    }

    /* One model for the whole session: asked for only when a new chain is started. */
    char input[64];
    while (!model_given && !storage_exists(DATA_FILE)) {
        printf("\nChoose the transaction model for this session:\n");
        printf("1. UTXO model\n2. Account-based model\n");
        if (!cli_prompt_line("Model: ", input, sizeof(input))) {
            goto cleanup;
        }
        model_given = parse_model(input, &model);
        if (!model_given) printf("Invalid choice.\n");
    }

    srand((unsigned)time(NULL));

    if (!library_boot(lib, model, model_given)) {
        goto cleanup;
    }
    if (difficulty_given) {
        lib->difficulty = difficulty;
    }
    printf("Transaction model: %s. Mining difficulty: %d.\n",
           ledger_model_name(lib->ledger.model), lib->difficulty);

    cli_run(lib);
    exit_code = 0;

cleanup:
    library_free(lib);
    keystore_close(&lib->keys);
    return exit_code;
}
