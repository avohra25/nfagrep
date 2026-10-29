#!/bin/sh
# End-to-end tests for the nfagrep binary. Usage: tests/cli_test.sh path/to/nfagrep
BIN=${1:-build/nfagrep}
pass=0
fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

printf 'apple pie\nBanana split\ncherry tart\napple crumble\n' > "$tmp/fruit.txt"
printf 'no fruit here\nonly an apple\n' > "$tmp/more.txt"

# expect NAME EXPECTED_STATUS EXPECTED_OUTPUT -- ARGS...
expect() {
    name=$1 want_status=$2 want_out=$3
    shift 4
    got_out=$("$BIN" "$@" 2>"$tmp/stderr")
    got_status=$?
    if [ "$got_status" = "$want_status" ] && [ "$got_out" = "$want_out" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s\n  status: want %s got %s\n  want: %s\n  got:  %s\n' \
            "$name" "$want_status" "$got_status" "$want_out" "$got_out"
    fi
}

F=$tmp/fruit.txt
expect "basic match"      0 "apple pie
apple crumble"                                   -- '^apple' "$F"
expect "no match status"  1 ""                   -- 'kiwi' "$F"
expect "ignore case"      0 "Banana split"       -- -i 'banana' "$F"
expect "invert"           0 "Banana split
cherry tart"                                     -- -v 'apple' "$F"
expect "count"            0 "2"                  -- -c 'apple' "$F"
expect "count zero"       1 "0"                  -- -c 'kiwi' "$F"
expect "line numbers"     0 "3:cherry tart"      -- -n 'tart$' "$F"
expect "only matching"    0 "pp
p
p
pp"                                              -- -o 'p+' "$F"
expect "only multiple"    0 "an
apple"                                           -- -o 'an|apple' "$tmp/more.txt"
expect "whole line"       0 "cherry tart"        -- -x '[a-z]+ tart' "$F"
expect "whole line miss"  1 ""                   -- -x 'cherry' "$F"
expect "two files"        0 "$F:apple pie
$F:apple crumble
$tmp/more.txt:only an apple"                     -- 'apple' "$F" "$tmp/more.txt"
expect "bad pattern"      2 ""                   -- '(oops' "$F"
expect "missing file"     2 ""                   -- 'x' "$tmp/nope.txt"
expect "usage"            2 ""                   --
expect "two -e patterns"  0 "Banana split
cherry tart"                                     -- -e 'nan' -e 'tart' "$F"
expect "-e with files"    0 "$F:cherry tart
$tmp/more.txt:no fruit here"                     -- -e 'cherry' -e 'fruit' "$F" "$tmp/more.txt"
expect "-e bad pattern"   2 ""                   -- -e 'ok' -e '[x' "$F"
expect "-e dash pattern"  0 "Banana split"       -- -e '-|split' "$F"
expect "word"             0 "apple pie"          -- -w 'pie' "$F"
expect "word miss"        1 ""                   -- -w 'app' "$F"
expect "word only"        0 "an"                 -- -ow 'an|a' "$tmp/more.txt"
expect "replace"          0 "pie of apple
crumble of apple"                                -- -r '$2 of $1' '^(apple) (\w+)' "$F"
expect "replace named"    0 "[split]"            -- -r '[${dish}]' -x 'Banana (?<dish>\w+)' "$F"
expect "replace all"      0 "Ba_a_a split"       -- -r '_' -i 'N' "$F"
expect "replace only"     0 "pie
crumble"                                         -- -o -r '$1' 'apple (\w+)' "$F"
expect "replace count"    0 "2"                  -- -c -r 'x' 'apple' "$F"
expect "bad replacement"  2 ""                   -- -r '$2' '(apple)' "$F"
expect "replace invert"   2 ""                   -- -v -r 'x' 'apple' "$F"

got=$(printf 'one\ntwo\nthree' | "$BIN" -n 'e$')
if [ "$got" = "1:one
3:three" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL stdin without trailing newline: $got"; fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
