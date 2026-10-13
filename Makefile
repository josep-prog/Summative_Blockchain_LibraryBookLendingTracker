# Builds the library binary; OpenSSL provides SHA-256, ECDSA and PBKDF2.

CC = gcc
CFLAGS = -Wall -Wextra -std=c11
LDLIBS = -lssl -lcrypto

SRC = src/main.c src/cli.c src/cli_catalog.c src/cli_lending.c src/cli_tokens.c \
      src/cli_chain.c src/library.c src/lending.c src/fraud.c src/mining.c \
      src/blockchain.c src/merkle.c src/pending.c src/inventory.c src/registry.c \
      src/ledger.c src/storage.c src/keystore.c src/crypto.c
BIN = library

$(BIN): $(SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)

# Forgets the saved chain and keys: the next run starts a new chain with new keys.
fresh:
	rm -rf data/library.dat data/library.dat.tmp data/keys

# Edge-case and tamper checks, run in a temporary copy of the registries.
test: $(BIN)
	tests/run_tests.sh
