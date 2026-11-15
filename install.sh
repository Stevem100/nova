#!/bin/sh
# ---------------------------------------------------------------------------
# Nova installer — POSIX sh (Linux, macOS, BSD).
#
# One-line install (Linux / macOS):
#
#   curl -fsSL https://raw.githubusercontent.com/Stevem100/nova/main/install.sh | sh
#
# With arguments (note the "-s --" when piping):
#
#   curl -fsSL https://raw.githubusercontent.com/Stevem100/nova/main/install.sh | sh -s -- --prefix ~/.local --with-packages
#
# Or download first, then run:
#
#   sh install.sh [--prefix DIR] [--version X.Y.Z] [--with-packages]
#                 [--from-source [DIR]] [--uninstall]
#
# What it does:
#   * Linux x86_64 : downloads the official binary from GitHub Releases and
#                    verifies its SHA-256 against SHA256SUMS.txt before it is
#                    installed.
#   * macOS / BSD  : clones the source at the release tag and builds it
#                    locally (needs cc/clang + make — on macOS run
#                    `xcode-select --install` if a compiler is missing).
#   * --with-packages additionally seeds the default package registry
#                    (~/.nova/registry) with every package published in the
#                    release, so `import "strx"` and friends work offline.
#   * --uninstall removes the installed binary (the package registry is
#                    left untouched).
#
# Windows users: use install.ps1 (PowerShell) instead.
# ---------------------------------------------------------------------------
set -eu

REPO="Stevem100/nova"
DEFAULT_VERSION="1.2.0"

# ---------------------------------------------------------------- helpers --
msg()  { printf '%s\n' "$*"; }
step() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
err()  { printf 'error: %s\n' "$*" >&2; }
die()  { err "$*"; exit 1; }

usage() {
    cat <<'USAGE'
Usage: sh install.sh [options]

Options:
  --prefix DIR         install the binary to DIR/bin
                       (default: /usr/local if writable, else ~/.local)
  --version X.Y.Z      install a specific released version
                       (default: latest GitHub release)
  --with-packages      also seed ~/.nova/registry with the release packages
  --from-source [DIR]  build from source instead of downloading the binary;
                       uses DIR if given, otherwise clones the release tag
  --uninstall          remove the installed nova binary
  -h, --help           show this help
USAGE
}

# ------------------------------------------------------------------ args ---
PREFIX_ARG=""
VERSION_ARG=""
FROM_SOURCE=0
SOURCE_DIR=""
WITH_PACKAGES=0
UNINSTALL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            [ $# -ge 2 ] || die "--prefix needs a value"
            PREFIX_ARG=$2; shift 2 ;;
        --prefix=*)
            PREFIX_ARG=${1#*=}; shift ;;
        --version)
            [ $# -ge 2 ] || die "--version needs a value"
            VERSION_ARG=$2; shift 2 ;;
        --version=*)
            VERSION_ARG=${1#*=}; shift ;;
        --with-packages)
            WITH_PACKAGES=1; shift ;;
        --from-source)
            FROM_SOURCE=1
            if [ $# -ge 2 ] && [ "${2#-}" = "$2" ]; then
                SOURCE_DIR=$2; shift
            fi
            shift ;;
        --from-source=*)
            FROM_SOURCE=1; SOURCE_DIR=${1#*=}; shift ;;
        --uninstall)
            UNINSTALL=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            die "unknown option: $1 (try --help)" ;;
    esac
done

# ------------------------------------------------------------------ tools --
FETCH=""
if command -v curl >/dev/null 2>&1; then
    FETCH=curl
elif command -v wget >/dev/null 2>&1; then
    FETCH=wget
fi

fetch() {                                  # fetch URL DEST
    case "$FETCH" in
        curl) curl -fsSL "$1" -o "$2" ;;
        wget) wget -qO "$2" "$1" ;;
        *) die "need curl or wget to download files" ;;
    esac
}

sha256_of() {                              # sha256_of FILE -> hex
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 -r "$1" | cut -d' ' -f1
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"
    else
        die "no sha256 tool found (install sha256sum, shasum, openssl or python3)"
    fi
}

verify_checksum() {                        # verify_checksum FILE NAME SUMS
    want=$(awk -v n="$2" '$2 == n { print $1; exit }' "$3")
    [ -n "$want" ] || die "no checksum entry for $2 in SHA256SUMS.txt"
    have=$(sha256_of "$1")
    want=$(printf '%s' "$want" | tr 'A-Z' 'a-z')
    have=$(printf '%s' "$have" | tr 'A-Z' 'a-z')
    [ "$have" = "$want" ] || die "SHA-256 mismatch for $2
  expected: $want
  actual:   $have
The download is corrupt or was tampered with — do not install it."
}

TMP=""
cleanup() { if [ -n "$TMP" ] && [ -d "$TMP" ]; then rm -rf "$TMP"; fi; }
trap 'cleanup; exit 130' INT TERM
trap cleanup EXIT
TMP=$(mktemp -d "${TMPDIR:-/tmp}/nova-install.XXXXXX" 2>/dev/null || \
      { mkdir -p "${TMPDIR:-/tmp}/nova-install.$$" && echo "${TMPDIR:-/tmp}/nova-install.$$"; })

# ----------------------------------------------------------------- paths ---
if [ -z "${HOME:-}" ]; then
    die "HOME is not set — cannot determine install locations"
fi

if [ -n "$PREFIX_ARG" ]; then
    PREFIX=$PREFIX_ARG
elif [ -d /usr/local ] && [ -w /usr/local ]; then
    PREFIX=/usr/local
else
    PREFIX=$HOME/.local
    msg "note: /usr/local is not writable, installing to $PREFIX"
    msg "      (run under sudo with --prefix /usr/local for a system-wide install)"
fi
BIN_DIR=$PREFIX/bin

resolve_prefix_for_uninstall() {
    if [ -n "$PREFIX_ARG" ]; then
        PREFIX=$PREFIX_ARG
    elif [ -x /usr/local/bin/nova ]; then
        PREFIX=/usr/local
    elif [ -x "$HOME/.local/bin/nova" ]; then
        PREFIX=$HOME/.local
    else
        die "no installed nova found in /usr/local/bin or ~/.local/bin (use --prefix)"
    fi
}

# ------------------------------------------------------------------ flow ---
if [ "$UNINSTALL" = 1 ]; then
    resolve_prefix_for_uninstall
    if [ -f "$BIN_DIR/nova" ]; then
        rm -f "$BIN_DIR/nova"
        step "removed $BIN_DIR/nova"
    else
        warn "$BIN_DIR/nova does not exist"
    fi
    msg "note: package registry (~/.nova/registry) and projects were left untouched"
    exit 0
fi

# Windows / MSYS → use the PowerShell installer instead
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        die "on Windows use install.ps1 instead:
  irm https://raw.githubusercontent.com/$REPO/main/install.ps1 | iex" ;;
esac

# resolve version
VERSION=""
if [ -n "$VERSION_ARG" ]; then
    VERSION=$VERSION_ARG
else
    if [ -n "$FETCH" ]; then
        if fetch "https://api.github.com/repos/$REPO/releases/latest" "$TMP/latest.json" 2>/dev/null; then
            VERSION=$(sed -n 's/.*"tag_name": *"\(v[0-9][0-9.]*\)".*/\1/p' "$TMP/latest.json" | head -n 1 | tr -d v)
        fi
    fi
    [ -n "$VERSION" ] || { VERSION=$DEFAULT_VERSION; warn "could not query the latest release, defaulting to v$VERSION"; }
fi
RELEASE_URL="https://github.com/$REPO/releases/download/v$VERSION"
step "Nova installer — v$VERSION"

have_compiler() {
    command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1
}

install_from_source() {
    BUILD_DIR=""
    if [ -n "$SOURCE_DIR" ]; then
        [ -f "$SOURCE_DIR/Makefile" ] && [ -f "$SOURCE_DIR/src/nova.h" ] || \
            die "$SOURCE_DIR does not look like a Nova checkout (needs Makefile and src/nova.h)"
        BUILD_DIR=$SOURCE_DIR
        step "building from local checkout: $BUILD_DIR"
    else
        command -v git >/dev/null 2>&1 || die "--from-source needs git installed (or pass a checkout directory)"
        BUILD_DIR=$TMP/src
        step "cloning source (v$VERSION)..."
        git clone --quiet --depth 1 --branch "v$VERSION" "https://github.com/$REPO.git" "$BUILD_DIR" || \
            die "git clone failed — check your network or the version tag"
    fi

    have_compiler || die "no compiler found (cc/gcc/clang).
macOS: run 'xcode-select --install', then re-run this script."
    command -v make >/dev/null 2>&1 || die "make is required to build from source"

    step "compiling (this takes under a minute)..."
    make -C "$BUILD_DIR" -s || die "build failed"
    [ -f "$BUILD_DIR/nova" ] || die "build produced no ./nova binary"

    mkdir -p "$BIN_DIR" 2>/dev/null || die "cannot create $BIN_DIR"
    cp "$BUILD_DIR/nova" "$BIN_DIR/nova.tmp"
    chmod 0755 "$BIN_DIR/nova.tmp"
    mv "$BIN_DIR/nova.tmp" "$BIN_DIR/nova"
}

install_binary() {
    step "downloading nova-linux-x86_64..."
    fetch "$RELEASE_URL/nova-linux-x86_64" "$TMP/nova-linux-x86_64"
    fetch "$RELEASE_URL/SHA256SUMS.txt" "$TMP/SHA256SUMS.txt"
    verify_checksum "$TMP/nova-linux-x86_64" "nova-linux-x86_64" "$TMP/SHA256SUMS.txt"
    step "checksum verified"

    mkdir -p "$BIN_DIR" 2>/dev/null || die "cannot create $BIN_DIR"
    cp "$TMP/nova-linux-x86_64" "$BIN_DIR/nova.tmp"
    chmod 0755 "$BIN_DIR/nova.tmp"
    mv "$BIN_DIR/nova.tmp" "$BIN_DIR/nova"
}

seed_packages() {
    step "seeding the package registry (--with-packages)..."
    command -v unzip >/dev/null 2>&1 || command -v python3 >/dev/null 2>&1 || \
        { warn "need unzip or python3 to extract packages — skipping"; return 0; }

    fetch "$RELEASE_URL/nova-registry-$VERSION.zip" "$TMP/registry.zip"
    rm -rf "$TMP/reg"; mkdir -p "$TMP/reg"
    if command -v unzip >/dev/null 2>&1; then
        unzip -q "$TMP/registry.zip" -d "$TMP/reg"
    else
        python3 -c 'import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
            "$TMP/registry.zip" "$TMP/reg"
    fi

    REGISTRY=${NOVA_REGISTRY:-$HOME/.nova/registry}
    mkdir -p "$REGISTRY"
    # the release zip nests everything under .registry/ — find the content root
    CONTENT=$TMP/reg
    if [ -d "$TMP/reg/.registry" ]; then CONTENT=$TMP/reg/.registry; fi
    for entry in "$CONTENT"/* "$CONTENT"/.[!.]*; do
        [ -e "$entry" ] || continue
        name=$(basename "$entry")
        if [ ! -e "$REGISTRY/$name" ]; then
            cp -R "$entry" "$REGISTRY/$name"
        fi
    done
    step "packages seeded into $REGISTRY (Nova's default registry — no env var needed)"
}

# pick install path per platform
OS=$(uname -s)
ARCH=$(uname -m)
if [ "$FROM_SOURCE" = 1 ]; then
    install_from_source
elif [ "$OS" = "Linux" ] && [ "$ARCH" = "x86_64" ]; then
    install_binary
else
    msg "no prebuilt binary for $OS/$ARCH — building from source"
    install_from_source
fi

if [ "$WITH_PACKAGES" = 1 ]; then
    seed_packages
fi

# smoke test
INSTALLED_VERSION=$("$BIN_DIR/nova" --version) || die "installed binary failed to run"
step "installed: $INSTALLED_VERSION -> $BIN_DIR/nova"

case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) msg ""
       msg "add Nova to your PATH (add this line to ~/.profile or ~/.zshrc):"
       msg "  export PATH=\"$BIN_DIR:\$PATH\"" ;;
esac

msg ""
msg "getting started:"
msg "  nova                 # interactive REPL"
msg "  nova run script.nv   # run a file"
msg "  nova pkg search webx # browse the package registry"
msg "docs: https://github.com/$REPO#readme"
