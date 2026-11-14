#!/bin/sh
# Nova test runner — compares actual output against expected output.
# Usage: ./tests/run_tests.sh        (uses ./nova)
#        NOVA=./nova_debug ./tests/run_tests.sh
set -u

cd "$(dirname "$0")/.." || exit 1
NOVA_BIN="${NOVA:-./nova}"

if [ ! -x "$NOVA_BIN" ]; then
    echo "error: $NOVA_BIN not found; run 'make' first" >&2
    exit 2
fi

pass=0
fail=0
failed_names=""

for t in tests/t*_*.nv; do
    name=$(basename "$t" .nv)
    expected="tests/expected/$name.out"
    actual="/tmp/nova_test_$name.out"

    NOVA_TEST_VAR=1 "$NOVA_BIN" run "$t" > "$actual" 2>&1
    rc=$?

    if [ ! -f "$expected" ]; then
        echo "MISSING EXPECTED: $expected"
        fail=$((fail + 1))
        failed_names="$failed_names $name"
        continue
    fi

    if [ $rc -eq 0 ] && diff -u "$expected" "$actual" > "/tmp/nova_diff_$name.txt" 2>&1; then
        pass=$((pass + 1))
        echo "PASS: $name"
    else
        fail=$((fail + 1))
        failed_names="$failed_names $name"
        echo "FAIL: $name (exit=$rc)"
        head -15 "/tmp/nova_diff_$name.txt"
    fi
done

# package-manager end-to-end flow (publish -> install -> import -> remove)
echo "----------------------------------------"
if sh tests/pkg_test.sh; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    failed_names="$failed_names pkg_test"
fi

echo "----------------------------------------"
echo "passed: $pass  failed: $fail"
[ $fail -eq 0 ] || echo "failed:$failed_names"
exit $fail
