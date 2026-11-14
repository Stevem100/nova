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

    case "$name" in
        t17_cgi)
            # runs the script under a full CGI/1.1 environment
            GATEWAY_INTERFACE=CGI/1.1 \
            REQUEST_METHOD=POST \
            QUERY_STRING='q=hello+world&page=2' \
            CONTENT_TYPE=application/x-www-form-urlencoded \
            CONTENT_LENGTH=9 \
            HTTP_USER_AGENT=NovaTestSuite \
            HTTP_COOKIE='sid=abc123; theme=dark' \
            REMOTE_ADDR=203.0.113.9 \
            HTTP_HOST=test.nova.local \
            REQUEST_URI='/app/search?q=x' \
                sh -c 'printf "name=Zai" | "$1" run "$2"' _ "$NOVA_BIN" "$t" \
                > "$actual" 2>&1
            rc=$?
            ;;
        t19_packages)
            # imports all bundled packages through the sample registry
            NOVA_TEST_VAR=1 NOVA_REGISTRY="$PWD/.registry" \
                "$NOVA_BIN" run "$t" > "$actual" 2>&1
            rc=$?
            ;;
        t20_web_packages)
            # exercises every web-dev package through the sample registry
            NOVA_TEST_VAR=1 NOVA_REGISTRY="$PWD/.registry" \
                "$NOVA_BIN" run "$t" > "$actual" 2>&1
            rc=$?
            ;;
        t21_queue)
            # queue package needs the sample registry; state goes under /tmp
            NOVA_TEST_VAR=1 NOVA_REGISTRY="$PWD/.registry" \
                "$NOVA_BIN" run "$t" > "$actual" 2>&1
            rc=$?
            ;;
        t22_db_migrate)
            # db_migrate + jdb through the sample registry; state under /tmp
            NOVA_TEST_VAR=1 NOVA_REGISTRY="$PWD/.registry" \
                "$NOVA_BIN" run "$t" > "$actual" 2>&1
            rc=$?
            ;;
        t24_http_client)
            # imports http_client through the sample registry (offline test;
            # the live-socket cases run in the pentest suite)
            NOVA_TEST_VAR=1 NOVA_REGISTRY="$PWD/.registry" \
                "$NOVA_BIN" run "$t" > "$actual" 2>&1
            rc=$?
            ;;
        *)
            NOVA_TEST_VAR=1 "$NOVA_BIN" run "$t" > "$actual" 2>&1
            rc=$?
            ;;
    esac

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
