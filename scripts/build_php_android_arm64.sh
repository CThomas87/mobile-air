#!/usr/bin/env bash
# Build Android arm64 PHP for NativePHP mobile using the same layout as the
# existing installer pipeline (jniLibs/arm64-v8a + optional jniLibs.zip).
#
# Guarantees for this project:
# - PHP 8.4.x
# - libphp.so + opcache.so rebuilt together from the same PHP tag/build dir
# - libphp.so with embed SAPI
# - ZTS symbol set required by php_wrapper
# - opcache.so built and staged beside libphp.so
#
# Usage:
#   chmod +x scripts/build_php_android_arm64.sh
#   scripts/build_php_android_arm64.sh
#
# Optional env overrides:
#   PHP_VERSION=php-8.4.5    (default; any php-8.4.x tag)
#   API=24
#   ANDROID_NDK_HOME=/opt/android-ndk-r27
#   WORK_DIR=$HOME/build/php-android
#   PROJECT_ROOT=/path/to/mobile-air
#   JOBS=12
#   EXTRA_CONFIGURE_FLAGS="--disable-all"
#   EMIT_JNILIBS_ZIP=1
#   INSTALL_TO_PROJECT=1
#   SKIP_BUILD=1             (reuse existing build; verify + install only)

set -euo pipefail

# ── CRLF self-healing ──
# Git on Windows can inject \\r; strip them so bash doesn't choke.
if [[ "$(head -c 50 "${BASH_SOURCE[0]}" | od -c | grep -c '\\r')" -gt 0 ]]; then
  tmpf=$(mktemp)
  tr -d '\\r' < "${BASH_SOURCE[0]}" > "$tmpf"
  exec bash "$tmpf" "$@"
fi

# ── Skip-build support ──
# Set SKIP_BUILD=1 to reuse existing artifacts (verify + install only).
SKIP_BUILD="${SKIP_BUILD:-0}"

PHP_VERSION="${PHP_VERSION:-php-8.4.5}"
API="${API:-24}"
JOBS="${JOBS:-$(nproc)}"
ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
WORK_DIR="${WORK_DIR:-$HOME/build/php-android}"
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
EMIT_JNILIBS_ZIP="${EMIT_JNILIBS_ZIP:-1}"
INSTALL_TO_PROJECT="${INSTALL_TO_PROJECT:-1}"

TARGET="aarch64-linux-android"
TOOLCHAIN=""
SYSROOT=""
SRC_DIR="${WORK_DIR}/php-src"
BUILD_DIR="${WORK_DIR}/build-${TARGET}"
INSTALL_DIR="${BUILD_DIR}/out"
ARTIFACTS_DIR="${WORK_DIR}/artifacts"
STAGE_ROOT="${WORK_DIR}/stage"
STAGE_JNILIBS_DIR="${STAGE_ROOT}/jniLibs/arm64-v8a"

# ── Auto-detect cross-compiled deps ──
# Pre-built third-party libs (iconv, curl, openssl, libxml2, sqlite3, etc.)
# used by the configure step.  Auto-probe common locations if not set.
if [[ -z "${DEPS_PREFIX:-}" ]]; then
  for _probe in \
    "${HOME}/php-deps/build-arm64-v8a" \
    "${WORK_DIR}/deps/build-arm64-v8a" \
    "/opt/php-deps/build-arm64-v8a"; do
    if [[ -d "${_probe}/lib" && -f "${_probe}/include/iconv.h" ]]; then
      DEPS_PREFIX="${_probe}"
      break
    fi
  done
fi
DEPS_PREFIX="${DEPS_PREFIX:-}"

JNI_LIB_DIR="${PROJECT_ROOT}/resources/androidstudio/app/src/main/jniLibs/arm64-v8a"
INCLUDE_DIR="${PROJECT_ROOT}/resources/androidstudio/app/src/main/cpp/include/php"
PHP_CONFIG_HEADER_DEST="${PROJECT_ROOT}/resources/androidstudio/app/src/main/cpp/include/php_config.h"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

log()  { echo -e "${GREEN}[build-php-android]${NC} $*"; }
warn() { echo -e "${YELLOW}[build-php-android] WARN:${NC} $*"; }
fatal(){ echo -e "${RED}[build-php-android] ERROR:${NC} $*"; exit 1; }

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || fatal "Missing required command: $1"
}

require_exe() {
  [[ -x "$1" ]] || fatal "Missing required executable: $1"
}

ensure_tools() {
  require_cmd git
  require_cmd make
  require_cmd autoconf
  require_cmd bison
  require_cmd re2c
  require_cmd pkg-config
  require_cmd sed
  require_cmd awk
  require_cmd grep
  require_cmd find

  if [[ "${EMIT_JNILIBS_ZIP}" == "1" ]]; then
    require_cmd zip
  fi

  [[ -n "${ANDROID_NDK_HOME}" ]] || fatal "ANDROID_NDK_HOME (or ANDROID_NDK_ROOT) is not set"
  [[ -d "${ANDROID_NDK_HOME}" ]] || fatal "ANDROID_NDK_HOME does not exist: ${ANDROID_NDK_HOME}"

  TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64"
  SYSROOT="${TOOLCHAIN}/sysroot"
  [[ -d "${TOOLCHAIN}" ]] || fatal "NDK toolchain dir missing: ${TOOLCHAIN}"

  require_exe "${TOOLCHAIN}/bin/${TARGET}${API}-clang"
  require_exe "${TOOLCHAIN}/bin/${TARGET}${API}-clang++"
  require_exe "${TOOLCHAIN}/bin/llvm-ar"
  require_exe "${TOOLCHAIN}/bin/llvm-ranlib"
  require_exe "${TOOLCHAIN}/bin/llvm-nm"
}

validate_version() {
  if [[ ! "${PHP_VERSION}" =~ ^php-8\.4\.[0-9]+$ ]]; then
    fatal "PHP_VERSION must be a PHP 8.4 tag (e.g. php-8.4.15). Got: ${PHP_VERSION}"
  fi
}

prepare_source() {
  mkdir -p "${WORK_DIR}"

  if [[ ! -d "${SRC_DIR}/.git" ]]; then
    log "Cloning php-src into ${SRC_DIR}"
    git clone https://github.com/php/php-src.git "${SRC_DIR}"
  fi

  pushd "${SRC_DIR}" >/dev/null
  git fetch --tags --force
  git checkout "${PHP_VERSION}"
  git reset --hard "${PHP_VERSION}"
  popd >/dev/null
}

patch_source_for_android() {
  # Android Bionic API < 29 uses emulated TLS (emutls) instead of native
  # ELF TLS.  TSRM.h selects TSRM_TLS_MODEL_INITIAL_EXEC when __PIC__ is
  # defined, which emits gottprel asm relocations incompatible with emutls.
  # Add __ANDROID__ to the exclusion list so DEFAULT model (return 0) is
  # used instead.  This disables a JIT optimization but is otherwise safe.
  local tsrm_h="${SRC_DIR}/TSRM/TSRM.h"
  if grep -q 'defined(__HAIKU__)' "${tsrm_h}" 2>/dev/null && \
     ! grep -q 'defined(__ANDROID__)' "${tsrm_h}" 2>/dev/null; then
    log "Patching TSRM.h: adding __ANDROID__ to TLS model exclusion list"
    sed -i 's/defined(__HAIKU__)/defined(__HAIKU__) || defined(__ANDROID__)/' "${tsrm_h}"
  fi
}

configure_env() {
  export CC="${TOOLCHAIN}/bin/${TARGET}${API}-clang"
  export CXX="${TOOLCHAIN}/bin/${TARGET}${API}-clang++"
  export AR="${TOOLCHAIN}/bin/llvm-ar"
  export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
  export STRIP="${TOOLCHAIN}/bin/llvm-strip"
  export LD="${TOOLCHAIN}/bin/ld.lld"

  export CFLAGS="-fPIC"
  export CXXFLAGS="-fPIC"
  export CPPFLAGS="--sysroot=${SYSROOT}"
  export LDFLAGS="--sysroot=${SYSROOT}"

  # When cross-compiling with pre-built third-party libraries (iconv, curl,
  # ssl, etc.), the compiler/linker needs explicit -I/-L paths since the
  # sysroot isolates search directories.
  if [[ -n "${DEPS_PREFIX}" && -d "${DEPS_PREFIX}" ]]; then
    export CFLAGS="${CFLAGS} -I${DEPS_PREFIX}/include"
    export CXXFLAGS="${CXXFLAGS} -I${DEPS_PREFIX}/include"
    export CPPFLAGS="${CPPFLAGS} -I${DEPS_PREFIX}/include"
    export LDFLAGS="${LDFLAGS} -L${DEPS_PREFIX}/lib"
    export PKG_CONFIG_PATH="${DEPS_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    log "Cross-compile deps prefix: ${DEPS_PREFIX}"
  else
    warn "DEPS_PREFIX not set and not auto-detected — configure may fail on iconv, sqlite3, etc."
  fi
}

build_php() {
  rm -rf "${BUILD_DIR}"
  mkdir -p "${BUILD_DIR}"

  pushd "${SRC_DIR}" >/dev/null
  ./buildconf --force
  popd >/dev/null

  pushd "${BUILD_DIR}" >/dev/null

  local extra_flags="${EXTRA_CONFIGURE_FLAGS:-}"

  # Build configure flags for available deps.
  local dep_flags=""
  if [[ -n "${DEPS_PREFIX}" && -d "${DEPS_PREFIX}" ]]; then
    [[ -f "${DEPS_PREFIX}/include/iconv.h" ]]    && dep_flags+=" --with-iconv=${DEPS_PREFIX}"
    [[ -f "${DEPS_PREFIX}/include/sqlite3.h" ]]   && dep_flags+=" --with-sqlite3=${DEPS_PREFIX}"
    [[ -d "${DEPS_PREFIX}/include/libxml2" || -f "${DEPS_PREFIX}/include/libxml/tree.h" ]] \
      && dep_flags+=" --with-libxml"
    [[ -f "${DEPS_PREFIX}/include/sodium.h" ]]    && dep_flags+=" --with-sodium=${DEPS_PREFIX}"
    [[ -d "${DEPS_PREFIX}/include/curl" ]]        && dep_flags+=" --with-curl=${DEPS_PREFIX}"
    [[ -d "${DEPS_PREFIX}/include/openssl" ]]     && dep_flags+=" --with-openssl=${DEPS_PREFIX}"
    [[ -f "${DEPS_PREFIX}/include/zlib.h" ]]      && dep_flags+=" --with-zlib"
    [[ -f "${DEPS_PREFIX}/include/zip.h" ]]       && dep_flags+=" --with-zip"
    [[ -f "${DEPS_PREFIX}/include/oniguruma.h" ]] && dep_flags+=" --enable-mbstring"
    log "Auto-detected dep flags:${dep_flags}"
  fi

  log "Configuring PHP (${PHP_VERSION}) for ${TARGET} API ${API}"

  "${SRC_DIR}/configure" \
    --host="${TARGET}" \
    --build="$(uname -m)-pc-linux-gnu" \
    --prefix="${INSTALL_DIR}" \
    --enable-zts \
    --enable-embed=shared \
    --enable-opcache=shared \
    --disable-cli \
    --disable-cgi \
    --disable-phpdbg \
    --without-pear \
    --with-layout=GNU \
    ${dep_flags} \
    ${extra_flags}

  # Android Bionic (NDK r27, API 24) does NOT provide the full BIND
  # res_n* resolver APIs (res_ninit, res_nsearch, res_nclose) nor the
  # complete struct __res_state definition.  PHP's configure link-check
  # passes because the symbol exists in libc, but compilation then fails
  # because the NDK headers only forward-declare the struct.
  #
  # The configure script explicitly `unset`s ac_cv_func_res_nsearch
  # before checking, so we cannot override it via cache variables.
  # Instead, patch the generated headers after configure to remove
  # HAVE_RES_NSEARCH and ensure the fallback HAVE_RES_SEARCH is set.
  local cfg_h="${BUILD_DIR}/main/php_config.h"
  if grep -q '^#define HAVE_RES_NSEARCH 1' "${cfg_h}" 2>/dev/null; then
    log "Patching php_config.h: removing HAVE_RES_NSEARCH (unsupported on Android Bionic)"
    sed -i 's/^#define HAVE_RES_NSEARCH 1/\/* #undef HAVE_RES_NSEARCH -- Android Bionic lacks res_n* APIs *\//' "${cfg_h}"
  fi
  # dn_skipname / dn_expand also pass configure link-checks but their
  # declarations are missing from the NDK headers, causing implicit
  # function declaration errors at compile time.
  if grep -q '^#define HAVE_DN_SKIPNAME 1' "${cfg_h}" 2>/dev/null; then
    log "Patching php_config.h: removing HAVE_DN_SKIPNAME (undeclared in NDK headers)"
    sed -i 's/^#define HAVE_DN_SKIPNAME 1/\/* #undef HAVE_DN_SKIPNAME -- missing from Android NDK headers *\//' "${cfg_h}"
  fi
  if grep -q '^#define HAVE_DN_EXPAND 1' "${cfg_h}" 2>/dev/null; then
    log "Patching php_config.h: removing HAVE_DN_EXPAND (undeclared in NDK headers)"
    sed -i 's/^#define HAVE_DN_EXPAND 1/\/* #undef HAVE_DN_EXPAND -- missing from Android NDK headers *\//' "${cfg_h}"
  fi
  # getdtablesize passes link-check but is not declared in NDK headers.
  if grep -q '^#define HAVE_GETDTABLESIZE 1' "${cfg_h}" 2>/dev/null; then
    log "Patching php_config.h: removing HAVE_GETDTABLESIZE (undeclared in NDK headers)"
    sed -i 's/^#define HAVE_GETDTABLESIZE 1/\/* #undef HAVE_GETDTABLESIZE -- missing from Android NDK headers *\//' "${cfg_h}"
  fi
  # Android Bionic provides res_search() but lacks dn_expand/dn_skipname,
  # so HAVE_DNS_SEARCH_FUNC gets set (via php_dns.h) which registers
  # dns_get_record/dns_get_mx in the function table, but the implementation
  # (guarded by HAVE_FULL_DNS_FUNCS) is excluded → undefined symbols.
  # Remove HAVE_RES_SEARCH to break the chain entirely.
  if grep -q '^#define HAVE_RES_SEARCH 1' "${cfg_h}" 2>/dev/null; then
    log "Patching php_config.h: removing HAVE_RES_SEARCH (prevents DNS function registration mismatch)"
    sed -i 's/^#define HAVE_RES_SEARCH 1/\/* #undef HAVE_RES_SEARCH -- prevents HAVE_DNS_SEARCH_FUNC cascade *\//' "${cfg_h}"
  fi
  # Android Bionic lacks getdtablesize() declaration even though unistd.h
  # exists.  PHP calls it unconditionally under #ifdef HAVE_UNISTD_H.
  # We cannot use -D in CFLAGS because the parentheses break libtool's
  # bash -c invocation.  Inject the macro into php_config.h instead.
  if ! grep -q 'getdtablesize' "${cfg_h}" 2>/dev/null; then
    log "Patching php_config.h: adding getdtablesize() -> sysconf(_SC_OPEN_MAX) macro"
    echo '#define getdtablesize() sysconf(_SC_OPEN_MAX)' >> "${cfg_h}"
  fi
  # glob()/globfree() were introduced in Bionic API 28.  Our android_compat.c
  # provides a full implementation using fnmatch()+opendir(), so enable the
  # feature macro that PHP's ext/standard/dir.c checks.
  # We also inject the function prototypes because <glob.h> hides them
  # behind #if __ANDROID_API__ >= 28.
  if grep -q '/\* #undef HAVE_GLOB \*/' "${cfg_h}" 2>/dev/null; then
    log "Patching php_config.h: enabling HAVE_GLOB (shim provided in android_compat.c)"
    sed -i 's|/\* #undef HAVE_GLOB \*/|#define HAVE_GLOB 1  /* Android compat shim */|' "${cfg_h}"
    cat >> "${cfg_h}" <<'GLOB_PROTO'

/* Function prototypes for our glob/globfree shim (android_compat.c).
   The NDK <glob.h> hides these behind __ANDROID_API__ >= 28.
   Wrapped in extern "C" so the prototypes have C linkage even when
   this header is included from C++ translation units. */
#include <glob.h>
#ifndef GLOB_BRACE
#define GLOB_BRACE 0x0080
#endif
#ifdef __cplusplus
extern "C" {
#endif
int glob(const char *, int, int (*)(const char *, int), glob_t *);
void globfree(glob_t *);
#ifdef __cplusplus
}
#endif
GLOB_PROTO
  fi
  # Removing HAVE_DN_SKIPNAME and HAVE_DN_EXPAND causes the dns_get_record
  # and dns_get_mx implementations to be excluded, but HAVE_FULL_DNS_FUNCS
  # still registers them in the function table → undefined symbol at link.
  if grep -q '^#define HAVE_FULL_DNS_FUNCS 1' "${cfg_h}" 2>/dev/null; then
    log "Patching php_config.h: removing HAVE_FULL_DNS_FUNCS (DNS helpers unavailable)"
    sed -i 's/^#define HAVE_FULL_DNS_FUNCS 1/\/* #undef HAVE_FULL_DNS_FUNCS -- dn_skipname\/dn_expand unavailable *\//' "${cfg_h}"
  fi
  # Also remove from confdefs.h used during the build.
  local confdefs="${BUILD_DIR}/confdefs.h"
  if [[ -f "${confdefs}" ]]; then
    sed -i 's/^#define HAVE_RES_NSEARCH 1/\/* #undef HAVE_RES_NSEARCH *\//' "${confdefs}" 2>/dev/null || true
    sed -i 's/^#define HAVE_RES_SEARCH 1/\/* #undef HAVE_RES_SEARCH *\//' "${confdefs}" 2>/dev/null || true
    sed -i 's/^#define HAVE_DN_SKIPNAME 1/\/* #undef HAVE_DN_SKIPNAME *\//' "${confdefs}" 2>/dev/null || true
    sed -i 's/^#define HAVE_DN_EXPAND 1/\/* #undef HAVE_DN_EXPAND *\//' "${confdefs}" 2>/dev/null || true
    sed -i 's/^#define HAVE_GETDTABLESIZE 1/\/* #undef HAVE_GETDTABLESIZE *\//' "${confdefs}" 2>/dev/null || true
    sed -i 's/^#define HAVE_FULL_DNS_FUNCS 1/\/* #undef HAVE_FULL_DNS_FUNCS *\//' "${confdefs}" 2>/dev/null || true
  fi

  # ── Force all symbols visible ──
  # PHP 8.4's configure adds -fvisibility=hidden by default.  While ZEND_API,
  # PHPAPI, and EMBED_SAPI_API annotate key symbols with visibility("default"),
  # Android's libtool/linker combo can still drop them from the .dynsym table
  # in some configurations.  Removing -fvisibility=hidden guarantees that
  # execute_ex, php_embed_init, zend_execute_ex, and all other symbols the
  # native wrapper (php_engine.c) and extensions (opcache.so) need are always
  # present in the dynamic symbol table.
  # Trade-off: libphp.so grows ~1-2MB.  Acceptable for mobile.
  log "Patching Makefile: removing -fvisibility=hidden (ensures execute_ex export)"
  sed -i 's/-fvisibility=hidden//g' "${BUILD_DIR}/Makefile"

  # Compile the Android compatibility shim (getrandom, nl_langinfo stubs)
  # and inject into the link step via EXTRA_LIBS.
  local compat_src="${PROJECT_ROOT}/scripts/android_compat.c"
  local compat_obj="${BUILD_DIR}/android_compat.o"
  if [[ -f "${compat_src}" ]]; then
    log "Compiling Android compat shim -> android_compat.o"
    "${CC}" ${CFLAGS} -c "${compat_src}" -o "${compat_obj}"
    log "Patching Makefile: adding android_compat.o to EXTRA_LIBS"
    sed -i "s|^EXTRA_LIBS = |EXTRA_LIBS = ${compat_obj} |" "${BUILD_DIR}/Makefile"
  fi

  log "Compiling PHP"
  make -j"${JOBS}"

  log "Installing PHP to ${INSTALL_DIR}"
  make install

  popd >/dev/null
}

find_outputs() {
  local libphp_candidate="${INSTALL_DIR}/lib/libphp.so"
  [[ -f "${libphp_candidate}" ]] || fatal "libphp.so not found at ${libphp_candidate}"

  local opcache_candidate
  opcache_candidate="$(find "${INSTALL_DIR}" -type f -name 'opcache.so' | head -n1 || true)"
  [[ -n "${opcache_candidate}" ]] || fatal "opcache.so not found under ${INSTALL_DIR}. Ensure --enable-opcache=shared worked."

  # Enforce that opcache comes from THIS install/build directory, not a stray path.
  case "${opcache_candidate}" in
    "${INSTALL_DIR}"/*) ;;
    *) fatal "opcache.so path is outside current INSTALL_DIR: ${opcache_candidate}" ;;
  esac

  echo "${libphp_candidate}|${opcache_candidate}"
}

verify_symbols() {
  local libphp="$1"
  local nm="${TOOLCHAIN}/bin/llvm-nm"

  log "Verifying required symbols in libphp.so"
  log "  target: ${libphp}  ($(stat -c%s "${libphp}" 2>/dev/null || echo '?') bytes)"

  # Cache the full dynamic-symbol dump once.
  local syms_file
  syms_file=$(mktemp)
  "${nm}" -D --defined-only "${libphp}" > "${syms_file}"

  # Helper: check a symbol is present; on failure dump relevant context.
  _require_sym() {
    local sym="$1" msg="$2"
    if ! grep -q " ${sym}$" "${syms_file}"; then
      warn "Missing symbol: ${sym}"
      warn "Nearby exported symbols matching '${sym%%_*}':"
      grep -i "${sym%%_*}" "${syms_file}" | head -20 || true
      warn "Total exported symbols: $(wc -l < "${syms_file}")"
      rm -f "${syms_file}"
      fatal "${msg}"
    fi
  }

  _require_sym php_embed_init  "php_embed_init missing (embed SAPI not built correctly)"
  _require_sym tsrm_get_ls_cache "tsrm_get_ls_cache missing (ZTS runtime missing)"
  _require_sym ts_resource_ex  "ts_resource_ex missing (ZTS runtime missing)"
  _require_sym ts_free_thread  "ts_free_thread missing (ZTS runtime missing)"
  _require_sym executor_globals_offset "executor_globals_offset missing (likely NTS libphp.so)"
  _require_sym sapi_globals_offset     "sapi_globals_offset missing (likely NTS libphp.so)"
  _require_sym core_globals_offset     "core_globals_offset missing (likely NTS libphp.so)"

  # Critical for OPcache module load compatibility.
  _require_sym execute_ex      "execute_ex missing in libphp.so (OPcache will fail to load)"
  _require_sym zend_execute_ex "zend_execute_ex missing in libphp.so (OPcache will fail to load)"

  rm -f "${syms_file}"

  local cfg_h="${BUILD_DIR}/main/php_config.h"
  [[ -f "${cfg_h}" ]] || fatal "php_config.h not found at ${cfg_h}"
  grep -q '^#define ZTS 1' "${cfg_h}" || fatal "php_config.h does not define ZTS 1"

  log "Symbol and header verification passed"
}

patch_opcache_dt_needed() {
  local opcache="$1"

  # ── Belt-and-suspenders: add DT_NEEDED libphp.so to opcache.so ──
  #
  # PHP builds opcache.so without a DT_NEEDED entry for libphp.so — it
  # relies on the host process having PHP symbols in global scope.  On
  # Android, libphp.so is loaded via System.loadLibrary() which uses
  # RTLD_LOCAL, so those symbols are invisible to dlopen'd extensions.
  #
  # The php_preloader library (loaded before "php") handles this at runtime
  # by loading libphp.so with RTLD_GLOBAL first.  However, adding an
  # explicit DT_NEEDED provides an additional safety net: the Bionic linker
  # will directly resolve opcache.so's 426+ undefined symbols from
  # libphp.so via the dependency chain, regardless of RTLD_GLOBAL scope.
  #
  # Also remove the stale RUNPATH that points to the build machine's
  # cross-compile prefix (e.g. /home/chris/php-deps/build-arm64-v8a/lib).

  if ! command -v patchelf >/dev/null 2>&1; then
    warn "patchelf not found — skipping DT_NEEDED injection for opcache.so"
    warn "Install patchelf: sudo apt-get install patchelf"
    warn "Without DT_NEEDED, opcache relies solely on php_preloader RTLD_GLOBAL"
    return 0
  fi

  # Check if DT_NEEDED for libphp.so is already present
  if readelf -d "${opcache}" | grep -q 'NEEDED.*libphp\.so'; then
    log "opcache.so already has DT_NEEDED: libphp.so — no patch needed"
  else
    log "Adding DT_NEEDED libphp.so to opcache.so via patchelf"
    patchelf --add-needed libphp.so "${opcache}"

    if readelf -d "${opcache}" | grep -q 'NEEDED.*libphp\.so'; then
      log "opcache.so now has DT_NEEDED: libphp.so ✓"
    else
      warn "patchelf ran but DT_NEEDED for libphp.so not found — verify manually"
    fi
  fi

  # Remove stale RUNPATH from build machine (e.g. /home/user/php-deps/...)
  local runpath
  runpath="$(readelf -d "${opcache}" 2>/dev/null | grep -oP '(?<=RUNPATH\s{1,30})\S+' || true)"
  if [[ -n "${runpath}" ]]; then
    log "Removing stale RUNPATH from opcache.so: ${runpath}"
    patchelf --remove-rpath "${opcache}"
  fi
}

verify_pair_origin() {
  local libphp="$1"
  local opcache="$2"

  local php_commit
  php_commit="$(git -C "${SRC_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)"

  log "Verified binary pair origin"
  echo "  php_tag:      ${PHP_VERSION}"
  echo "  php_commit:   ${php_commit}"
  echo "  build_dir:    ${BUILD_DIR}"
  echo "  install_dir:  ${INSTALL_DIR}"
  echo "  libphp.so:    ${libphp}"
  echo "  opcache.so:   ${opcache}"
}

install_to_project() {
  local libphp="$1"
  local opcache="$2"

  mkdir -p "${JNI_LIB_DIR}"
  mkdir -p "${INCLUDE_DIR}"

  log "Replacing runtime pair in ${JNI_LIB_DIR} (libphp.so + opcache.so)"
  log "Copying libphp.so -> ${JNI_LIB_DIR}"
  cp -f "${libphp}" "${JNI_LIB_DIR}/libphp.so"

  log "Copying opcache.so -> ${JNI_LIB_DIR}"
  cp -f "${opcache}" "${JNI_LIB_DIR}/opcache.so"

  # Keep headers in sync with binary build to avoid ZTS/NTS mismatch.
  local installed_include_root="${INSTALL_DIR}/include/php"
  [[ -d "${installed_include_root}" ]] || fatal "Installed headers not found at ${installed_include_root}"

  log "Syncing PHP headers -> ${INCLUDE_DIR}"
  rm -rf "${INCLUDE_DIR}"
  mkdir -p "$(dirname "${INCLUDE_DIR}")"
  cp -R "${installed_include_root}" "${INCLUDE_DIR}"

  # Also copy generated config header used by CMake ZTS check.
  cp -f "${BUILD_DIR}/main/php_config.h" "${PHP_CONFIG_HEADER_DEST}"

  # The make install step placed an un-patched php_config.h into
  # include/php/main/php_config.h.  Overwrite it with the patched copy so
  # that both copies (include/php_config.h AND include/php/main/php_config.h)
  # carry the same extern-"C" glob/globfree guards, HAVE_GLOB, etc.
  cp -f "${BUILD_DIR}/main/php_config.h" "${INCLUDE_DIR}/main/php_config.h"

  log "Installed artifacts into project"
}

stage_pipeline_layout() {
  local libphp="$1"
  local opcache="$2"

  rm -rf "${STAGE_ROOT}"
  mkdir -p "${STAGE_JNILIBS_DIR}"

  # Preserve all existing runtime dependencies from the project jniLibs
  # (libcurl, libssl, etc.) and only replace PHP artifacts.
  if [[ -d "${JNI_LIB_DIR}" ]]; then
    cp -a "${JNI_LIB_DIR}/." "${STAGE_JNILIBS_DIR}/"
  fi

  cp -f "${libphp}" "${STAGE_JNILIBS_DIR}/libphp.so"
  cp -f "${opcache}" "${STAGE_JNILIBS_DIR}/opcache.so"

  log "Staged pipeline layout at ${STAGE_ROOT}/jniLibs/arm64-v8a"
}

emit_jnilibs_zip() {
  mkdir -p "${ARTIFACTS_DIR}"
  local zip_path="${ARTIFACTS_DIR}/jniLibs.zip"

  rm -f "${zip_path}"
  (
    cd "${STAGE_ROOT}"
    zip -rq "${zip_path}" jniLibs
  )

  log "Created pipeline artifact: ${zip_path}"
}

final_report() {
  local libphp="$1"
  local opcache="$2"

  echo
  log "Build complete"
  echo "  PHP version:     ${PHP_VERSION}"
  echo "  API level:       ${API}"
  echo "  libphp.so:       ${libphp}"
  echo "  opcache.so:      ${opcache}"
  echo "  installed lib:   ${JNI_LIB_DIR}/libphp.so"
  echo "  installed ext:   ${JNI_LIB_DIR}/opcache.so"
  echo "  staged jniLibs:  ${STAGE_JNILIBS_DIR}"
  if [[ "${EMIT_JNILIBS_ZIP}" == "1" ]]; then
    echo "  jniLibs.zip:     ${ARTIFACTS_DIR}/jniLibs.zip"
  fi
  echo
  echo "Next steps:"
  echo "  1) Run: ./scripts/verify_zts.sh android"
  echo "  2) Build Android app"
  echo "  3) Confirm runtime log shows opcache available"
}

main() {
  validate_version
  ensure_tools

  if [[ "${SKIP_BUILD}" == "1" ]]; then
    log "SKIP_BUILD=1 — reusing existing build artifacts"
    if [[ ! -d "${BUILD_DIR}" ]]; then
      fatal "SKIP_BUILD=1 but BUILD_DIR does not exist: ${BUILD_DIR}"
    fi
  else
    prepare_source
    patch_source_for_android
    configure_env
    build_php
  fi

  IFS='|' read -r libphp opcache < <(find_outputs)
  verify_symbols "${libphp}"
  verify_pair_origin "${libphp}" "${opcache}"
  patch_opcache_dt_needed "${opcache}"
  stage_pipeline_layout "${libphp}" "${opcache}"

  if [[ "${INSTALL_TO_PROJECT}" == "1" ]]; then
    install_to_project "${libphp}" "${opcache}"
  else
    warn "INSTALL_TO_PROJECT=0; skipped copying artifacts into repository"
  fi

  if [[ "${EMIT_JNILIBS_ZIP}" == "1" ]]; then
    emit_jnilibs_zip
  fi

  final_report "${libphp}" "${opcache}"
}

main "$@"
