#!/bin/sh
# Nova package-manager end-to-end test:
#   publish -> install (with version constraints) -> import -> run ->
#   list/search/registry/remove, plus registry-fallback imports.
# Self-checking: output is diffed against tests/expected/pkg_test.out.
set -u

cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"
NOVA_BIN="${NOVA:-./nova}"
if [ ! -x "$NOVA_BIN" ]; then
    echo "error: $NOVA_BIN not found; run 'make' first" >&2
    exit 2
fi
case "$NOVA_BIN" in
    /*) ;;
    *)  NOVA_BIN="$ROOT/$NOVA_BIN" ;;      # the test cd's into temp dirs
esac

WORK="$(mktemp -d)"
REG="$WORK/registry"
export NOVA_REGISTRY="$REG"

ACTUAL="$WORK/actual.out"
: > "$ACTUAL"

run() {   # run <description> <cmd> [args...]; appends stdout+stderr
    desc="$1"; shift
    echo "## $desc" >> "$ACTUAL"
    "$@" >> "$ACTUAL" 2>&1
    echo "rc=$?" >> "$ACTUAL"
}

# --- publish sample packages -------------------------------------------------
run publish-strx   "$NOVA_BIN" pkg publish examples/packages/strx
run publish-mathx  "$NOVA_BIN" pkg publish examples/packages/mathx

# an older mathx release, so version constraints have something to pick
mkdir "$WORK/mathx-old"
cat > "$WORK/mathx-old/nova.json" <<'EOF'
{
  "name": "mathx",
  "version": "1.0.0",
  "entry": "main.nv",
  "description": "Math helpers: mean, median, variance, clamp, lerp",
  "dependencies": {}
}
EOF
cp examples/packages/mathx/main.nv "$WORK/mathx-old/main.nv"
run publish-mathx-old "$NOVA_BIN" pkg publish "$WORK/mathx-old"
run publish-dup       "$NOVA_BIN" pkg publish examples/packages/mathx

# --- project with dependencies ----------------------------------------------
mkdir "$WORK/app"
cat > "$WORK/app/nova.json" <<'EOF'
{
  "name": "toolkit",
  "version": "0.2.0",
  "entry": "app.nv",
  "description": "test project",
  "dependencies": { "mathx": "^1.0.0", "strx": "*" }
}
EOF
cat > "$WORK/app/app.nv" <<'EOF'
import "mathx";
import "strx";
echo "slug=", $strx->slug("Fresh Install"), "\n";
echo "mean=", $mathx->mean([1, 2, 3, 4]), " clamp=", $mathx->clamp(150, 0, 99), "\n";
EOF
cd "$WORK/app" || exit 1
run install-all "$NOVA_BIN" pkg install
run run-app     "$NOVA_BIN" run app.nv
run list        "$NOVA_BIN" pkg list

# re-install: should report "up to date"
run install-again "$NOVA_BIN" pkg install mathx

# version constraint picks the older line for ~1.0.0 in a second project
mkdir "$WORK/oldapp"
cat > "$WORK/oldapp/nova.json" <<'EOF'
{
  "name": "oldapp",
  "version": "0.1.0",
  "entry": "app.nv",
  "description": "",
  "dependencies": { "mathx": "~1.0.0" }
}
EOF
echo 'import "mathx"; echo "old mean=", $mathx->mean([10, 20]), "\n";' > "$WORK/oldapp/app.nv"
cd "$WORK/oldapp" || exit 1
run install-old "$NOVA_BIN" pkg install
run run-old     "$NOVA_BIN" run app.nv

# --- search + registry display ----------------------------------------------
run search-helpers "$NOVA_BIN" pkg search helpers
run search-math  "$NOVA_BIN" pkg search MATH
run search-none  "$NOVA_BIN" pkg search zzzz
run registry-show "$NOVA_BIN" pkg registry

# --- registry fallback: import without installing ---------------------------
mkdir "$WORK/direct"
echo 'import "strx"; echo $strx->shout("registry fallback"), "\n";' > "$WORK/direct/app.nv"
cd "$WORK/direct" || exit 1
run run-fallback "$NOVA_BIN" run app.nv

# --- remove ------------------------------------------------------------------
cd "$WORK/app" || exit 1
run remove-strx "$NOVA_BIN" pkg remove strx
run list-after  "$NOVA_BIN" pkg list
run remove-missing "$NOVA_BIN" pkg remove nope

# --- unknown command ---------------------------------------------------------
run pkg-bogus "$NOVA_BIN" pkg frobnicate

cd "$ROOT" || exit 1

# mask the temp dir path so the expected file is stable
sed "s#$REG#<REGISTRY>#g; s#$WORK#<TMP>#g" "$ACTUAL" > "$ACTUAL.masked"

# Regenerate the golden file explicitly: PKG_WRITE_EXPECTED=1 sh tests/pkg_test.sh
EXPECTED="${PKG_EXPECTED:-tests/expected/pkg_test.out}"
if [ "${PKG_WRITE_EXPECTED:-0}" = "1" ]; then
    cp "$ACTUAL.masked" "$EXPECTED"
    echo "updated $EXPECTED"
    exit 0
fi
if diff -u "$EXPECTED" "$ACTUAL.masked" > "$WORK/pkg_diff.txt" 2>&1; then
    echo "PASS: pkg_test (package manager flow)"
    exit 0
else
    echo "FAIL: pkg_test (package manager flow)"
    head -40 "$WORK/pkg_diff.txt"
    exit 1
fi
