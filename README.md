# Blockchain-Based Library Book Lending Tracker

## Final Summative: the complete system

A lending ledger for the ALU library, written in C. Every borrow, return, reservation, renewal, overdue flag, fine payment and token transfer is a **signed record**. Records wait in a **pending pool**, are confirmed in **mined blocks**, and cannot be changed afterwards without detection.

Demo video: <https://www.youtube.com/watch?v=ma2JMULPHJY>

This version combines Formative 1 (chain, registries, SHA-256, signatures, validation, CLI) and Formative 2 (token rewards, pending pool, proof of work, UTXO and account models, solo/pool/cloud mining) and adds the summative extensions.

### What changed from Formative 2

| Before (Formative 2) | Now (Summative) |
| :---- | :---- |
| One lending event per block | Up to **4 records per block** (fixed-size batching), protected by a **Merkle root** built from scratch |
| One librarian key signed everything | **Every member and librarian has an ECDSA key pair**; the public key is stored in each record |
| A title was on loan or not | **Copy-level inventory** with a state machine: AVAILABLE, BORROWED, RESERVED, LOST |
| No queueing | **Reservation queue** per title (FIFO linked list), consumed automatically on return |
| OVERDUE was a marker | **Due dates, renewals, fines per day late**, fine settlement in tokens |
| Pool was first-in-first-out | Pool is **sorted by priority**; requests can be held as **SUSPICIOUS** |
| Replays stopped only for rewards | **Per-signer nonce** on every request |
| No misuse checks | **Four fraud heuristics** and a manual review workflow |
| Only the chain was saved; pool, transfers and mining rewards were lost on exit | **Everything is saved** in one sealed file and restored at start-up |
| Numbered menu | **Named commands** (`borrow_book BK001 ALU001`), which also keep a menu number |

### Source files

| File | Purpose |
| :---- | :---- |
| [src/config.h](src/config.h) | All tunable rules: loan period, fine, renewal limit, block size, difficulty range, rewards. |
| [src/crypto.c](src/crypto.c) | SHA-256, ECDSA (secp256k1) sign/verify, key files, public key as hex, PIN hashing. |
| [src/keystore.c](src/keystore.c) | One key pair per identity in `data/keys/`; private keys encrypted, loaded only to sign. |
| [src/registry.c](src/registry.c) | Book, member and librarian registries loaded from file; ID format checks. |
| [src/inventory.c](src/inventory.c) | Copies, the availability state machine, reservation queues. |
| [src/merkle.c](src/merkle.c) | Merkle tree construction. |
| [src/blockchain.c](src/blockchain.c) | Records, blocks, hashing, proof of work, full chain validation. |
| [src/pending.c](src/pending.c) | The pending pool and its priority sort. |
| [src/ledger.c](src/ledger.c) | UTXO and account models, fees, change, history linked list, model conversion. |
| [src/lending.c](src/lending.c) | Builds, checks and signs requests; applies confirmed records. |
| [src/fraud.c](src/fraud.c) | Misuse heuristics. |
| [src/mining.c](src/mining.c) | Block building from the pool; solo, pool and cloud mining. |
| [src/storage.c](src/storage.c) | Save and load of the whole state, with a signed seal. |
| [src/library.c](src/library.c) | Start-up, validation gate, save. |
| [src/main.c](src/main.c) | Login and command-line options. |
| [src/cli.c](src/cli.c) | The command shell: input, argument parsing, the command table, `help`. |
| [src/cli_catalog.c](src/cli_catalog.c) | Member, book and copy commands. |
| [src/cli_lending.c](src/cli_lending.c) | Lending, reservation, overdue and fine commands. |
| [src/cli_tokens.c](src/cli_tokens.c) | Token transfer, balance and history commands. |
| [src/cli_chain.c](src/cli_chain.c) | Pool, mining, chain, fraud review, audit and demo commands. |

### Compilation

Dependencies: `gcc`, `make` and OpenSSL 3 (`sudo apt install gcc make libssl-dev`). No other libraries.

```bash
make clean && make
```

which runs

```bash
gcc -Wall -Wextra -std=c11 -o library src/main.c src/cli.c src/cli_catalog.c src/cli_lending.c src/cli_tokens.c src/cli_chain.c src/library.c src/lending.c src/fraud.c src/mining.c src/blockchain.c src/merkle.c src/pending.c src/inventory.c src/registry.c src/ledger.c src/storage.c src/keystore.c src/crypto.c -lssl -lcrypto
```

It compiles with no warnings.

### Running

```bash
./library [--model utxo|account] [--difficulty 1-4]
```

The program asks for a librarian login and the keystore passphrase (or reads it from `LIBRARY_KEY_PASSPHRASE`), then opens a command prompt. The first run creates the keys with whatever passphrase you give; later runs must use the same one. A mistyped passphrase can be tried again, three times in all.

At the prompt, `help` lists every command with a number. A command can be run by its name (`utxo_view`), by its number (`19`), or by the start of its name when no other command starts that way (`utxo`). A missing argument is asked for, and a misspelt name gets a suggestion instead of being run.

`make fresh` deletes the saved chain and the keys, so the next run starts a new chain with new keys.

| Librarian ID | Name | Role | PIN |
| :---- | :---- | :---- | :---- |
| LIB001 | Joseph Nishimwe | ADMIN | 1234 |
| LIB002 | Librarian | LIBRARIAN | 5678 |

Books `BK001`-`BK006`, members `ALU001`-`ALU006`, copies in [data/copies.txt](data/copies.txt) (`copy_id,book_id,condition`). `BK003` has a single copy, which is convenient for testing reservations.

`./library --hash-pin LIB003 4321` prints the PIN hash for a new line in `data/librarians.txt`.

To start over, delete the generated state: `rm -rf data/library.dat data/keys`.

### Switching between the UTXO and account-based models

| How | Example |
| :---- | :---- |
| Command-line option | `./library --model utxo` or `./library --model account` |
| Start-up menu | With no `--model` on a new chain, the program asks `1. UTXO model  2. Account-based model` |

The model applies to every token transaction of the session. With no `--model`, a saved chain keeps the model it was saved with. Starting a saved chain with the other model converts every balance (a balance becomes one unspent output, or the sum of the outputs becomes the balance). The prompt always shows the active model.

`token_transfer`, `token_balance` and `utxo_view` belong to the UTXO model; `account_transfer`, `account_balance` and `account_nonce` to the account model. Using one in the wrong session prints which command to use instead.

### Setting the mining difficulty

Difficulty is the number of leading `0` characters a block hash must have (1 to 4, default 2).

| How | Example |
| :---- | :---- |
| Command-line option | `./library --difficulty 3` |
| Command | `set_difficulty 3` |
| Inspect | `difficulty_status`, and `benchmark` to compare attempts and time at 1, 2, 3 and 4 |

Each block stores the difficulty it was mined at, so validation still works after a change.

### Commands

A missing argument is asked for. Names with spaces go in double quotes.

```
Membership      register_member <member_id> <full_name> <course_code>
                view_member [member_id]          member_nonce <member_id>
Books, copies   register_book <book_id> <title> <author>
                view_book [book_id]              add_copy <book_id> [NEW|GOOD|WORN]
                view_copy [copy_id]              mark_lost <copy_id> <reason_code>
Lending         borrow_book <book_id> <member_id> [due=YYYY-MM-DD]
                return_book <copy_id> [NEW|GOOD|WORN] [reason=CODE]
                renew_book <copy_id>             reserve_book <book_id> <member_id>
                reservation_status [book_id]
Fines           check_overdue                    view_fines [member_id]
                settle_fine <member_id> [amount]
Tokens          token_transfer <from> <to> <amount>         (UTXO)
                token_balance [account]   utxo_view         (UTXO)
                account_transfer <from> <to> <amount> [nonce]   (account)
                account_balance [account] account_nonce <account>   (account)
                transaction_history <member_id>
Chain, mining   pool_view   mine_solo   mine_pool [miners]   mine_cloud <rounds>
                chain_view [block_index]   validate_chain   chain_save   chain_load
                difficulty_status   set_difficulty <1-4>   benchmark
Fraud, audit    fraud_review   approve_suspicious <id>   reject_suspicious <id>
                lending_history   member_history <member_id>   book_history <book_id>
Simulation      advance_time <days>   tamper_demo [record|hash|signature]      (ADMIN)
```

Any request also accepts `nonce=<n>` and `id=<request_id>`, used to test replay protection and duplicate detection.

### How the system works

**Records and blocks.** A lending record holds `record_id`, `book_id`, `copy_id`, `book_title`, `member_id`, `member_name`, `action`, `due_date`, `fine_amount`, `nonce` and `signature`, plus the signer's ID and public key, the request time, and the reward fields. A block holds `index`, `timestamp`, `record_count`, `previous_hash`, `merkle_root`, `hash`, `sealed_by`, `token_reward`, `transaction_id`, the difficulty and the proof-of-work nonce, followed by up to 4 records. The block hash covers every header field; the records are covered through the Merkle root.

**Merkle tree.** Each record is hashed with SHA-256 (signed fields plus signature) to make a leaf. Pairs of nodes are hashed together level by level; an odd node is paired with itself. The root is stored in the block header, recomputed whenever a block is validated, and compared. `chain_view <index>` prints every level of a block's tree.

**From request to confirmed record.**

1. A command builds a record and checks it against the registries, the copy's state and the lending rules.
2. The nonce must be the signer's next one.
3. The record is signed with the signer's private key and the signature is verified.
4. The fraud heuristics run. A flagged request enters the pool as SUSPICIOUS, otherwise as PENDING.
5. A miner takes the top 4 PENDING requests in priority order, checks each again, builds the block, and searches for a nonce.
6. Only when the block is mined are its records applied: the copy changes state, queues move, rewards are paid, fines are assessed, and each signer's nonce advances. The requests leave the pool and the state is saved.

**Copy inventory and state machine.** Legal transitions are a table in `inventory.c`:

| From | To | When |
| :---- | :---- | :---- |
| AVAILABLE | BORROWED | a borrow is confirmed |
| BORROWED | AVAILABLE or RESERVED | a return is confirmed; RESERVED if someone is queued |
| RESERVED | BORROWED | the member it is held for borrows it |
| AVAILABLE | RESERVED | a queued member is offered a copy that is free |
| AVAILABLE, BORROWED | LOST | `mark_lost` with a reason code |

A copy takes one state-changing request at a time: while its borrow is in the pool, nobody else can borrow or return it.

**Reservations.** `reserve_book` is accepted only when no copy is free. When a return is confirmed, the copy is held for the member at the head of the queue and only they can borrow it. A loan cannot be renewed while the title has a queue.

**Due dates, overdue, fines, renewals.** A loan is due after 14 days (or the `due=` date, at most 30 days away). `renew_book` adds 7 days, at most twice, never on an overdue loan. Before every command, the program scans the BORROWED copies and raises one signed OVERDUE record for each loan past its due date. The fine is 2 coins per day or part of a day late; it is fixed in the RETURNED record and paid with `settle_fine`, which transfers tokens to the `LIBRARY` account.

**Token rewards.** A confirmed on-time return mints 10 coins for the member, a late one 5; an overdue loan that is not returned creates no transaction. A 1-coin fee is deducted from every reward and transfer and goes to `LIBRARY`. A block's `token_reward` is the total of its returns and its `transaction_id` is the SHA-256 of their reward transaction IDs.

**UTXO model.** Tokens exist as unspent outputs. A transfer spends the sender's oldest outputs until they cover amount + fee, returns the excess as a change output, and is rejected if the inputs are insufficient. Spent outputs leave the set, so they cannot be spent twice; tokens promised to requests already in the pool are also counted. The full UTXO set is printed after every confirmed block.

**Account model.** Each account has a balance and a nonce. A transfer debits amount + fee, credits the recipient, and is rejected if the balance is too low or the nonce is not the expected one. Each account's transactions are kept in a linked list (`transaction_history`).

**Pending pool priority.** Overdue-related requests (OVERDUE flags, late returns, fine settlements) come first; within the same priority the oldest request comes first. The pool array is kept sorted with a stable insertion sort.

**Mining.** `mine_solo`: the logged-in librarian mines every block and earns 50 coins per block. `mine_pool`: 2-8 simulated miners with random hash rates; a 2% pool fee is deducted and the rest is shared by attempts, shown in a table. `mine_cloud <rounds>`: 600 hashes are rented per round for a fixed 25 coins; the table shows reward, fee, net and cumulative net per round, and a warning is printed if cumulative fees ever exceed cumulative rewards.

### Security

**ECDSA signatures.** Keys are generated on the secp256k1 curve through OpenSSL's EVP interface (the older `EC_KEY` functions are deprecated in OpenSSL 3 and would not compile cleanly with `-Wall`). Each identity has `data/keys/<id>.key.pem`, encrypted with AES-256 under the keystore passphrase and readable only by its owner, and `data/keys/<id>.pub.pem`. A private key is decrypted only for the moment it signs. The signature covers every field of the record. Verification uses the public key stored in the record, and also checks that it is the key registered for that signer.

**Nonce replay protection.** Every signer has a nonce starting at 0. A request must carry the signer's next nonce: the confirmed nonce plus the signer's requests still in the pool. The confirmed nonce advances only when a record is mined. Validation requires each signer's nonces to run 0, 1, 2, ... along the chain. If a request is dropped, the signer's later requests are dropped too, because their nonces can no longer become valid.

**Fraud detection.**

| Heuristic | Condition |
| :---- | :---- |
| EXCESSIVE_BORROWS | the member would hold more than 3 copies, counting borrows already in the pool |
| RAPID_REBORROW | the same member borrows the same title within 24 hours of returning it |
| DUPLICATE_REQUEST | the request ID already exists on the chain or in the pool |
| CONDITION_MANIPULATION | a return downgrades the copy more than one level with no reason code |

A flagged request stays in the pool as SUSPICIOUS, is listed by `fraud_review`, and is never mined until `approve_suspicious`. `reject_suspicious` discards it. A duplicate request ID can only be rejected.

**Chain validation** (`validate_chain`) checks, for every block: index order, recomputed hash, proof of work, previous-hash linkage, strictly increasing timestamps, the Merkle root, and the reward summary; and for every record: the ECDSA signature, the signer's registered key, that the book and member exist in the registries, a unique record ID, and the nonce sequence.

**Validation gate.** The loaded chain is validated before the prompt appears. If it fails, the session is LOCKED: viewing commands work, nothing else does, and nothing is saved.

### Disk persistence

Everything is in `data/library.dat`, plain text, one record per line with `|` between fields. The first field names the line:

```
LIBRARY-LEDGER|version
META|difficulty|block_reward|model|clock_offset|next_request_seq|mint_sequence|chain_height
BLOCK|index|timestamp|record_count|previous_hash|merkle_root|sealed_by|token_reward|transaction_id|difficulty|nonce|hash
RECORD|<record>                      (record_count of these follow their BLOCK)
COPY|copy_id|book_id|condition|state|current_holder_id|due_date|renewals|overdue_flagged
QUEUE|book_id|member_id|reservation_date
POOL|status|priority|flag|flag_detail|<record>
FINES|member_id|fines_owed|fines_paid
ACCOUNT|id|balance|nonce
UTXO|tx_id|output_index|owner|amount
HISTORY|account|tx_id|sender|recipient|amount|fee|nonce|has_nonce
SEAL|sha256_of_all_lines_above|librarian_id|ecdsa_signature_hex

<record> = record_id|timestamp|action|book_id|copy_id|book_title|member_id|member_name|peer_id|
           due_date|fine_amount|amount|condition|reason|token_reward|tx_id|nonce|signer_id|
           signer_pubkey|signature_hex
```

The file is saved after every command that changes state, after every mined block and on exit. It is written to a temporary file and renamed, so a crash cannot leave half a file. The **SEAL** line is the SHA-256 of the rest of the file signed by the librarian: the chain protects lending records, and the seal extends that to balances, inventory and the pool, which are not on the chain. If the seal does not match at start-up the session is locked.

### Design choices and assumptions

- **Three extra actions.** Besides BORROWED, RETURNED, RESERVED, RENEWED and OVERDUE, records can be `LOST` (the librarian override), `SETTLED` (a fine payment) and `TRANSFER` (a token transfer), so that these also pass through the pool and mining as the brief requires for token transactions.
- **Who signs.** Members sign their own borrow, return, renew, reserve, settle and transfer requests. The logged-in librarian signs OVERDUE and LOST records. The library desk holds the members' encrypted keys (a custodial wallet).
- **One nonce per identity** covers both lending requests and token transfers, so `member_nonce` and `account_nonce` show the same counter.
- **Amounts are in hundredths of a coin** (`long`), so pool shares and the 2% fee are exact.
- **`advance_time`** moves the library's clock forward (ADMIN only, saved with the state) so that due dates, overdue fines and the 24-hour re-borrow window can be tested without waiting. It only moves forward, so block timestamps keep increasing.
- **A hold does not expire.** A copy held for a queued member stays RESERVED until that member borrows it.
- **Keys, registries and the data file belong together.** Removing a member from `members.txt` or deleting `data/keys` makes the saved chain fail validation, by design.

### How to test

Set `export LIBRARY_KEY_PASSPHRASE=demo-pass` once. Each script logs in as `LIB001` / `1234`. Run `make fresh` first for a clean start.

| Test | Commands after login | Expected |
| :---- | :---- | :---- |
| Borrow, invalid IDs | `borrow_book BK001 ALU001`, `borrow_book BK999 ALU001`, `borrow_book BK001 ALU777` | First enters the pool; the others report the book / member is not in the registry |
| Solo mining | `pool_view`, `mine_solo` | Block with attempts and nonce; copy becomes BORROWED |
| On-time return | `return_book BK001-C1`, `mine_solo`, `token_balance` | 10-coin reward, member holds 9.00 after the fee |
| Reservation and automatic offer | `borrow_book BK003 ALU002`, `mine_solo`, `reserve_book BK003 ALU003`, `mine_solo`, `return_book BK003-C1`, `mine_solo`, `reservation_status` | On return, BK003-C1 becomes RESERVED for ALU003 |
| Late return and fine | `borrow_book BK004 ALU005`, `mine_solo`, `advance_time 17`, `check_overdue`, `return_book BK004-C1`, `pool_view`, `mine_solo`, `view_fines` | OVERDUE record raised; the return is 3 days and a few seconds late, which counts as 4 started days: 5-coin reward, 8.00 fine; both requests at the top of the pool |
| Fine settlement | `settle_fine ALU005`, `mine_solo` | Rejected while the balance is under fine + fee; succeeds once the member has enough |
| Renewal | `borrow_book BK002 ALU001`, `mine_solo`, `renew_book BK002-C1`, `mine_solo`, `view_copy BK002-C1` | Due date +7 days, renewals 1 of 2 |
| Replay / bad nonce | `borrow_book BK005 ALU004 nonce=7` | REJECTED: bad nonce |
| Excessive borrows | four `borrow_book` for the same member | The fourth is SUSPICIOUS |
| Rapid re-borrow | return a copy, mine, borrow the same title again | SUSPICIOUS |
| Duplicate request | `borrow_book BK004 ALU004 id=REQ-000001` | SUSPICIOUS; can only be rejected |
| Condition manipulation | `return_book <NEW copy> WORN` | SUSPICIOUS; with `reason=WATER_DAMAGE` it is accepted |
| Review workflow | `fraud_review`, `approve_suspicious <id>` or `reject_suspicious <id>`, `mine_solo` | Held requests are skipped by mining until approved |
| Pool mining | several requests, then `mine_pool 4` | Reward table: miner, attempts, share %, reward, 2% fee |
| Cloud mining | one request, then `mine_cloud 3` | Per-round table; UNPROFITABLE warning at round 3 |
| Difficulty | `set_difficulty 4`, `mine_solo`, `benchmark` | Tens of thousands of attempts; about 16x per extra zero |
| UTXO change | `--model utxo`: `token_transfer ALU001 ALU002 3`, `mine_solo`, `utxo_view` | Transaction with a change output; a second transfer beyond the balance is rejected |
| Account nonce | `--model account`: `account_transfer ALU001 ALU002 3 0` after an earlier request | REJECTED: bad nonce |
| Tamper: record | `tamper_demo record` | INVALID: Merkle root does not match; session locked; `chain_load` restores |
| Tamper: block | `tamper_demo hash` | INVALID: stored hash does not match |
| Tamper: forged record | `tamper_demo signature` | Merkle root and proof of work recomputed by the attacker; INVALID: digital signature is invalid |
| Tamper on disk | edit a number in `data/library.dat`, restart | Seal check fails; session locked |
| Persistence | `exit`, start again | Chain, inventory, queues, pool, balances and nonces restored and validated |

`make test` (or `tests/run_tests.sh`) runs the error, edge-case, tampering and persistence checks automatically. It works in a temporary copy of the registries, so the saved chain and keys are not touched.

A scripted run looks like this:

```bash
printf 'LIB001\n1234\nborrow_book BK001 ALU001\npool_view\nmine_solo\nreturn_book BK001-C1\nmine_solo\ntoken_balance\nexit\n' | ./library --model utxo
```

These files are created when the program runs and are listed in `.gitignore`:

| File | Purpose |
| :---- | :---- |
| `library` | The compiled program. |
| `data/library.dat` | The saved state. |
| `data/keys/` | Private and public keys of every member and librarian. |
