#!/usr/bin/env bash
# scripts/verify_zts.sh — Verify ZTS is enabled in shipped PHP binaries and headers
#
# Usage:
#   ./scripts/verify_zts.sh android  [path/to/libphp.so] [path/to/php_config.h]
#   ./scripts/verify_zts.sh ios      [path/to/libphp.a]  [path/to/php_config.h]
#
# Exit codes:
#   0 — ZTS verified
#   1 — ZTS NOT enabled (build must fail)
#   2 — Usage error

set -euo pipefail

PLATFORM="${1:-}"
LIB_PATH="${2:-}"
HEADER_PATH="${3:-}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

fail() {
    echo -e "${RED}FATAL: $1${NC}" >&2
    exit 1
}

warn() {
    echo -e "${YELLOW}WARNING: $1${NC}" >&2
}

ok() {
    echo -e "${GREEN}OK: $1${NC}"
}

if [[ -z "$PLATFORM" ]]; then
    echo "Usage: $0 <android|ios> [lib_path] [header_path]"
    exit 2
fi

# ─── Header check ───

check_header() {
    local header="$1"

    if [[ ! -f "$header" ]]; then
        fail "php_config.h not found at: $header"
    fi

    # Check for ZTS defined
    if grep -q '^\s*#define ZTS 1' "$header" 2>/dev/null; then
        ok "php_config.h has '#define ZTS 1'"
        return 0
    fi

    # Check for ZTS undefined (explicit failure)
    if grep -q '/\* #undef ZTS \*/' "$header" 2>/dev/null; then
        fail "php_config.h has '/* #undef ZTS */' — PHP built WITHOUT thread safety!"
    fi

    # Check for PTHREADS (secondary ZTS indicator)
    if grep -q '#define PTHREADS 1' "$header" 2>/dev/null; then
        ok "php_config.h has PTHREADS defined (ZTS indicator)"
        return 0
    fi

    warn "Could not definitively confirm ZTS in php_config.h"
    return 1
}

# ─── Binary check ───

check_binary_android() {
    local lib="$1"

    if [[ ! -f "$lib" ]]; then
        fail "libphp.so not found at: $lib"
    fi

    # Method 1: Check for TSRM symbols
    local tsrm_symbols=0
    if command -v nm &>/dev/null; then
        tsrm_symbols=$(nm -D "$lib" 2>/dev/null | grep -c 'tsrm_' || true)
    elif command -v readelf &>/dev/null; then
        tsrm_symbols=$(readelf -Ws "$lib" 2>/dev/null | grep -c 'tsrm_' || true)
    elif command -v objdump &>/dev/null; then
        tsrm_symbols=$(objdump -T "$lib" 2>/dev/null | grep -c 'tsrm_' || true)
    fi

    if [[ "$tsrm_symbols" -gt 0 ]]; then
        ok "libphp.so exports $tsrm_symbols TSRM symbols (ZTS confirmed)"
        return 0
    fi

    # Method 2: Check for tsrm_startup string in binary
    if strings "$lib" 2>/dev/null | grep -q 'tsrm_startup'; then
        ok "libphp.so contains tsrm_startup string (ZTS likely)"
        return 0
    fi

    # Method 3: Check for ZTS-specific function names
    local zts_funcs=0
    if command -v nm &>/dev/null; then
        zts_funcs=$(nm -D "$lib" 2>/dev/null | grep -cE '(tsrm_new_interpreter|tsrm_set_interpreter|ts_resource_ex)' || true)
    fi

    if [[ "$zts_funcs" -gt 0 ]]; then
        ok "libphp.so has ZTS-specific functions"
        return 0
    fi

    fail "libphp.so does NOT appear to be ZTS-enabled! No TSRM symbols found."
}

check_binary_ios() {
    local lib="$1"

    if [[ ! -f "$lib" ]]; then
        warn "iOS PHP library not found at: $lib (may be in framework)"
        return 0
    fi

    # Check for TSRM symbols
    local tsrm_symbols=0
    if command -v nm &>/dev/null; then
        tsrm_symbols=$(nm "$lib" 2>/dev/null | grep -c 'tsrm_' || true)
    fi

    if [[ "$tsrm_symbols" -gt 0 ]]; then
        ok "iOS PHP lib exports $tsrm_symbols TSRM symbols (ZTS confirmed)"
        return 0
    fi

    warn "Could not verify ZTS in iOS PHP library"
    return 1
}

# ─── Main ───

echo "=== NativePHP ZTS Verification ==="
echo "Platform: $PLATFORM"
echo ""

errors=0

case "$PLATFORM" in
    android)
        HEADER="${HEADER_PATH:-resources/androidstudio/app/src/main/cpp/include/php_config.h}"
        LIB="${LIB_PATH:-resources/androidstudio/app/src/main/jniLibs/arm64-v8a/libphp.so}"

        echo "--- Header Check ---"
        check_header "$HEADER" || ((errors++))
        echo ""

        echo "--- Binary Check ---"
        check_binary_android "$LIB" || ((errors++))
        ;;

    ios)
        HEADER="${HEADER_PATH:-}"
        LIB="${LIB_PATH:-}"

        if [[ -n "$HEADER" ]]; then
            echo "--- Header Check ---"
            check_header "$HEADER" || ((errors++))
            echo ""
        fi

        if [[ -n "$LIB" ]]; then
            echo "--- Binary Check ---"
            check_binary_ios "$LIB" || ((errors++))
        fi
        ;;

    *)
        echo "Unknown platform: $PLATFORM"
        exit 2
        ;;
esac

echo ""
if [[ "$errors" -gt 0 ]]; then
    fail "ZTS verification FAILED with $errors error(s). Build must not continue."
else
    ok "All ZTS checks passed!"
fi
