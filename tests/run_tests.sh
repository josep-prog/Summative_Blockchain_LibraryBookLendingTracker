#!/usr/bin/env bash
# Repeatable checks for errors, edge cases and state changes.
# Works in a temporary copy of the registries, so data/library.dat and data/keys are not touched.
#
#   make && tests/run_tests.sh        (add -v to print the transcript of a failed check)

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
VERBOSE=${1:-}
PASS=0
FAIL=0
OUT=""
STATUS=0

export LIBRARY_KEY_PASSPHRASE=test-pass

if [ ! -x "$ROOT/library" ]; then
    echo "Build the program first: make"
    exit 1
fi
cd "$WORK" || exit 1

# Empty chain, new keys, original registries.
fresh() {
    rm -rf data
    mkdir data
    cp "$ROOT"/data/books.txt "$ROOT"/data/copies.txt "$ROOT"/data/members.txt \
       "$ROOT"/data/librarians.txt data/
}

# session <model> <command>...   logs in as LIB001, runs the commands, then exits.
session() {
    local model=$1
    shift
    OUT=$(printf '%s\n' LIB001 1234 "$@" exit | timeout 120 "$ROOT/library" --model "$model" 2>&1)
    STATUS=$?
}

report() {
    if [ "$1" = ok ]; then
        PASS=$((PASS + 1))
        echo "  ok    $2"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL  $2"
        [ "$VERBOSE" = -v ] && echo "$OUT" | sed 's/^/        | /'
    fi
}

expect() {      # expect <description> <text that must be printed>
    if echo "$OUT" | grep -qF -- "$2"; then report ok "$1"; else report fail "$1"; fi
}

refute() {      # refute <description> <text that must not be printed>
    if echo "$OUT" | grep -qF -- "$2"; then report fail "$1"; else report ok "$1"; fi
}

expect_status() {
    if [ "$STATUS" -eq "$2" ]; then report ok "$1"; else report fail "$1 (exit $STATUS)"; fi
}

echo "1. Borrowing"
fresh
session utxo 'borrow_book BK001 ALU001' 'borrow_book BK999 ALU001' 'borrow_book BK001 ALU777' \
             mine_solo 'view_copy BK001-C1'
expect "valid borrow enters the pool"             "BORROWED request REQ-000001 signed by ALU001"
expect "unknown book is refused"                  "Book ID 'BK999' is not in the book registry"
expect "unknown member is refused"                "ALU777"
expect "copy is BORROWED once mined"              "State          : BORROWED"
refute "the PIN is not echoed"                    "PIN: 1234"

echo "2. Requests waiting in the pool"
session utxo 'return_book BK001-C1' 'return_book BK001-C1' 'renew_book BK001-C1' \
             'borrow_book BK003 ALU002' 'borrow_book BK003 ALU003' 'borrow_book BK002 ALU002 nonce=9'
expect "second request on the same copy is refused"  "already has a RETURNED request in the pool"
expect "a copy promised in the pool is not lent twice" "No copy of 'Sustainable Leadership' is available"
expect "out-of-order nonce is refused"            "REJECTED: bad nonce for ALU002: got 9, expected 1"

echo "3. Tokens promised to pending transfers"
session utxo mine_solo 'token_transfer ALU001 ALU002 5' 'token_transfer ALU001 ALU003 5' \
             'token_transfer ALU001 ALU001 1' 'token_transfer ALU001 ALU002 -3' \
             'token_transfer ALU001 ALU002 abc' 'token_transfer ALU001 ALU002 99999999999999999999'
expect "on-time return pays 10 coins less the fee" "10 coins - 1.00 fee credited to ALU001"
expect "pending transfer is counted before the next" "ALU001 has 3.00 available, needs 6.00"
expect "transfer to self is refused"              "cannot send tokens to itself"
expect "negative, text and huge amounts are refused" "ERROR: Enter a positive amount"

echo "4. A held request blocks the signer's later requests"
fresh
session utxo 'borrow_book BK001 ALU001' 'borrow_book BK002 ALU001' 'borrow_book BK003 ALU001' \
             mine_solo 'borrow_book BK004 ALU001' 'return_book BK001-C1' mine_solo \
             'reject_suspicious REQ-000004' pool_view
expect "fourth concurrent borrow is SUSPICIOUS"   "FLAGGED SUSPICIOUS [EXCESSIVE_BORROWS]"
expect "mining explains why nothing was mined"    "1 PENDING request could not be mined yet"
expect "rejecting it drops the later request too" "its nonce 4 follows the dropped request"
expect "the pool is empty afterwards"             "Pending pool: 0 requests"

echo "5. Late return and fine"
fresh
session utxo 'borrow_book BK004 ALU005' mine_solo 'advance_time 17' check_overdue \
             'renew_book BK004-C1' 'return_book BK004-C1' mine_solo 'view_fines ALU005' \
             'settle_fine ALU005'
expect "an overdue loan cannot be renewed"        "an overdue loan cannot be renewed"
# 17 days on a 14-day loan is 3 days and a few seconds: every started day counts, so usually 4.
expect "late return halves the reward"            "reward 5 coins"
expect "fine is assessed when the return is mined" "coins assessed on ALU005"
expect "fine cannot be settled without tokens"    "REJECTED"

echo "6. Input handling"
fresh
LONG=$(head -c 5000 /dev/zero | tr '\0' 'A')
session utxo '' '   ' "$LONG" 'borow_book BK001 ALU001' \
             'register_member ALU010 "Jean Paul Mugabo" BSE' 'register_member ALU010 "Again" BSE' \
             'register_member BAD "X" BSE' "register_member ALU011 \"$LONG\" BSE" \
             'set_difficulty 9' 'set_difficulty x' 'mine_pool 100' 'mine_cloud 0' \
             'borrow_book BK001 ALU001 due=2026-02-30' 'borrow_book BK001 ALU001 due=2020-01-01' \
             'advance_time -5' 'chain_view 99' validate_chain
expect_status "program ends normally after bad input" 0
expect "overlong line is not run as a command"    "Unknown command 'AAAA"
expect "misspelt command gets a suggestion"       "Did you mean borrow_book?"
expect "name with spaces is kept whole"           "Member ALU010 (Jean Paul Mugabo, BSE) registered"
expect "duplicate member is refused"              "Member ALU010 is already registered"
expect "bad member ID is refused"                 "'BAD' is not a valid member ID"
refute "overlong name is not registered"          "Member ALU011"
expect "difficulty out of range is refused"       "Difficulty must be from 1 to 4"
expect "pool size out of range is refused"        "must be from 2 to 8"
expect "rental rounds out of range are refused"   "must be from 1 to 5 rounds"
expect "30 February is refused"                   "'2026-02-30' is not a valid date"
expect "past due date is refused"                 "is in the past"
expect "negative days are refused"                "whole number of days from 1 to 365"
expect "block index out of range is refused"      "Block index must be from 0 to 0"
expect "chain is still valid"                     "Blockchain is VALID - 1 block"

echo "7. Tampering in memory"
fresh
session utxo 'borrow_book BK001 ALU001' mine_solo 'tamper_demo record' \
             'borrow_book BK002 ALU002' chain_load validate_chain
expect "changed record breaks the Merkle root"    "Merkle root does not match"
expect "session is locked"                        "ERROR: The session is locked"
expect "chain_load restores the clean chain"      "Blockchain is VALID - 2 blocks"
session utxo 'tamper_demo hash'
expect "changed block breaks its stored hash"     "stored hash does not match"
session utxo 'tamper_demo signature'
expect "re-mined forgery fails the signature check" "digital signature is invalid"

echo "8. Tampering on disk"
session utxo 'return_book BK001-C1'
cp data/library.dat good.dat

sed -i '0,/|BORROWED|BK001|/s//|BORROWED|BK002|/' data/library.dat
cp data/library.dat edited.dat
session utxo validate_chain 'borrow_book BK004 ALU004'
expect "edited record fails the seal"             "file seal check FAILED"
expect "edited record is reported by block"       "Block #1: Merkle root does not match"
expect "locked session refuses a borrow"          "ERROR: The session is locked"
if cmp -s data/library.dat edited.dat; then report ok "locked session does not rewrite the file"
else report fail "locked session does not rewrite the file"; fi

cp good.dat data/library.dat
sed -i '0,/|0\{64\}|/s//|1'"$(printf '0%.0s' $(seq 63))"'|/' data/library.dat
session utxo validate_chain
expect "genesis previous hash must be 64 zeros"   "Block #0:"
expect "chain is reported INVALID"                "Blockchain is INVALID"

cp good.dat data/library.dat
sed -i 's/^COPY|BK001-C1|BK001|NEW|BORROWED|ALU001/COPY|BK001-C1|BK001|NEW|AVAILABLE|/' data/library.dat
session utxo validate_chain
expect "edited inventory fails the seal"          "The saved file's seal is INVALID"

head -c 700 good.dat > data/library.dat
session utxo validate_chain
expect_status "truncated file stops the program" 1
expect "truncated file is reported"               "could not be read"

echo "9. Persistence"
cp good.dat data/library.dat
session utxo pool_view validate_chain
expect "pending request is restored"              "REQ-000002  RETURNED  BK001-C1"
expect "restored chain validates"                 "Blockchain is VALID - 2 blocks"

echo "10. Account model"
session account mine_solo account_balance 'account_transfer ALU001 ALU002 3 0' \
                'account_transfer ALU001 ALU002 3' mine_solo account_balance validate_chain
expect "reused nonce is refused"                  "REJECTED: bad nonce for ALU001: got 0, expected 2"
expect "transfer is confirmed with its fee"       "ALU001 -> ALU002  amount 3.00  fee 1.00  nonce 2"
expect "chain validates after the model switch"   "Blockchain is VALID - 4 blocks"

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
