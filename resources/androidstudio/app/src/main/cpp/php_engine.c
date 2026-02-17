/**
 * php_engine.c — PhpEngine singleton implementation
 *
 * Manages the process-wide PHP module lifecycle for ZTS concurrent execution.
 * Instead of php_embed_init/php_embed_shutdown per request, we:
 * 1. Call php_embed_init ONCE (which does tsrm_startup + php_module_startup)
 * 2. Workers use tsrm_new_interpreter_context() for per-thread state
 * 3. Call php_module_shutdown + tsrm_shutdown_ex ONCE at exit
 */
#include "php_engine.h"
#include "php_request_context.h"
#include "zts_guard.h"

/* PHP headers */
#include "php_embed.h"
#include "TSRM.h"
#include "zend.h"
#include "zend_modules.h"
#include "zend_extensions.h"
#include "zend_string.h" /* zend_new_interned_string, zend_string_init_interned */

#include <dlfcn.h> /* dladdr — for auto-detecting extension directory */

/* Bridge functions registered as SAPI additional_functions (defined in php_bridge.c) */
extern const zend_function_entry nativephp_bridge_functions[];

/* Header handler for SAPI (defined in php_bridge.c) */
extern int android_header_handler(sapi_header_struct *sapi_header,
                                  sapi_header_op_enum op,
                                  sapi_headers_struct *sapi_headers);
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __ANDROID__
#include <android/log.h>
#define ENGINE_TAG "PhpEngine"
#define ENGINE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, ENGINE_TAG, __VA_ARGS__)
#define ENGINE_LOGW(...) __android_log_print(ANDROID_LOG_WARN, ENGINE_TAG, __VA_ARGS__)
#define ENGINE_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ENGINE_TAG, __VA_ARGS__)
#else
#define ENGINE_LOGI(...)                 \
    do                                   \
    {                                    \
        fprintf(stdout, "[PhpEngine] "); \
        fprintf(stdout, __VA_ARGS__);    \
        fprintf(stdout, "\n");           \
    } while (0)
#define ENGINE_LOGW(...)                       \
    do                                         \
    {                                          \
        fprintf(stderr, "[PhpEngine] WARN: "); \
        fprintf(stderr, __VA_ARGS__);          \
        fprintf(stderr, "\n");                 \
    } while (0)
#define ENGINE_LOGE(...)                        \
    do                                          \
    {                                           \
        fprintf(stderr, "[PhpEngine] ERROR: "); \
        fprintf(stderr, __VA_ARGS__);           \
        fprintf(stderr, "\n");                  \
    } while (0)
#endif

/* ─── Engine state (protected by mutex) ─── */
static pthread_mutex_t s_engine_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_engine_initialized = 0;
static char s_app_base_path[2048] = {0};

/*
 * opcache.so's own __emutls_get_address function pointer.
 *
 * CRITICAL: opcache.so may ship a statically-linked WEAK copy of
 * __emutls_get_address from the compiler runtime (libcompiler_rt).
 * This copy maintains a SEPARATE per-thread emutls array from libc's.
 * If we call libc's __emutls_get_address to set _tsrm_ls_cache, opcache.so's
 * code reads from its own array (which still has NULL) → SIGSEGV.
 *
 * We must use opcache.so's OWN __emutls_get_address for all TLS operations
 * that target opcache.so's _tsrm_ls_cache.
 */
#if defined(__ANDROID__) && defined(ZTS)
typedef void *(*emutls_get_address_fn)(void *);
static emutls_get_address_fn s_opcache_emutls_fn = NULL;
#endif
static char s_ini_entries_buf[8192] = {0};
static char s_opcache_path[1200] = {0};

/*
 * OPcache zend_extension activate callback.
 *
 * Stored during engine init so worker threads can re-invoke it after
 * forcing opcache.enable=1.  In file_cache_only mode with ZTS,
 * accel_globals_ctor() memsets per-thread globals to zero, including
 * ZCG(enabled).  The OnEnable INI callback only updates ZCG(enabled)
 * during PHP_INI_STAGE_STARTUP, but zend_ini_activate() calls it with
 * PHP_INI_STAGE_ACTIVATE → no-op.  So worker threads need to force the
 * INI value with STARTUP stage and then re-call accel_activate().
 */
static void (*s_opcache_activate)(void) = NULL;

/*
 * Correct INI directives hash table pointer from the main thread.
 *
 * After php_module_startup() calls zend_post_startup(), the internal
 * registered_zend_ini_directives may be replaced (interned/copied).
 * OPcache's REGISTER_INI_ENTRIES() adds entries to the NEW table.
 * But worker threads' executor_globals ctor (run during ts_resource(0))
 * may capture the OLD table pointer if TSRM was initialized before
 * OPcache registration.  We save the correct table after init and
 * force it on worker threads.
 */
static HashTable *s_correct_ini_directives = NULL;

/* ─── Init-phase ub_write: route PHP output to logcat during module startup ─── */
static size_t engine_init_ub_write(const char *str, size_t str_length)
{
    /* Log PHP output during init so extension loading errors are visible */
    if (str_length > 0)
    {
        char buf[4096];
        size_t len = str_length < sizeof(buf) - 1 ? str_length : sizeof(buf) - 1;
        memcpy(buf, str, len);
        buf[len] = '\0';
        __android_log_print(ANDROID_LOG_WARN, "PhpEngine", "PHP init output: %s", buf);
    }
    return str_length;
}

/**
 * SAPI log_message handler — routes PHP error_log() to Android logcat.
 * Without this, error_log() goes to stderr which is /dev/null on Android.
 */
static void android_sapi_log_message(const char *message, int syslog_type_int)
{
    (void)syslog_type_int;
    __android_log_print(ANDROID_LOG_ERROR, "PHP", "%s", message);
}

static void log_loaded_zend_extensions(void)
{
    zend_llist_position pos;
    zend_extension *extension = (zend_extension *)zend_llist_get_first_ex(&zend_extensions, &pos);

    if (!extension)
    {
        ENGINE_LOGI("No Zend extensions loaded");
        return;
    }

    while (extension)
    {
        ENGINE_LOGI("Loaded Zend extension: %s", extension->name ? extension->name : "(unknown)");
        extension = (zend_extension *)zend_llist_get_next_ex(&zend_extensions, &pos);
    }

    ENGINE_LOGI("Zend OPcache present: %s", zend_get_extension("Zend OPcache") ? "yes" : "no");
}

/* ─── emutls TLS fix for opcache.so on Android ───
 *
 * On Android API < 29, __thread variables use emulated TLS (emutls).
 * Each DSO (.so) gets its own _tsrm_ls_cache TLS variable.  PHP's
 * TSRMG_STATIC macros read the per-DSO copy.  When a new thread
 * starts, TSRMLS_CACHE_UPDATE() only updates the calling DSO's copy.
 * opcache.so's copy remains NULL for worker threads, causing SIGSEGV
 * at accel_activate+20 (ZCG(enabled) dereferences NULL).
 *
 * On regular Linux with ELF TLS, the dynamic linker interposes all
 * _tsrm_ls_cache references to the same TLS slot — so this issue
 * only manifests on Android with emutls.
 *
 * Fix: use __emutls_get_address() to locate opcache.so's TLS
 * variable and set it to the correct value from the pthread key.
 *
 * CRITICAL SUBTLETY: opcache.so may ship a statically-linked WEAK
 * copy of __emutls_get_address from libcompiler_rt.  This copy has
 * its own per-thread emutls array, separate from libc's.  We MUST
 * call opcache.so's copy (stored in s_opcache_emutls_fn) to set the
 * value in the correct per-thread array.
 */
#if defined(__ANDROID__) && defined(ZTS)
extern void *__emutls_get_address(void *) __attribute__((weak));

/* Helper: call the appropriate __emutls_get_address for a DSO.
 * For opcache.so, uses s_opcache_emutls_fn (its own statically-linked copy).
 * For other DSOs, falls back to the default (libc's) __emutls_get_address. */
static void *call_emutls_get_address(void *ctrl, void *dso_handle)
{
    /* Check if this DSO has its own __emutls_get_address */
    if (dso_handle)
    {
        emutls_get_address_fn dso_fn = (emutls_get_address_fn)
            dlsym(dso_handle, "__emutls_get_address");
        if (dso_fn)
        {
            return dso_fn(ctrl);
        }
    }
    /* Fallback to the default */
    if (__emutls_get_address)
    {
        return __emutls_get_address(ctrl);
    }
    return NULL;
}
#endif

void fix_opcache_tls_cache(void)
{
#if defined(__ANDROID__) && defined(ZTS)
    if (!__emutls_get_address)
    {
        ENGINE_LOGE("__emutls_get_address not available");
        return;
    }

    void *cache = tsrm_get_ls_cache();
    if (!cache)
    {
        ENGINE_LOGE("tsrm_get_ls_cache() NULL — TSRM not initialized?");
        return;
    }

    /*
     * On Android with emutls, each DSO (libphp.so, opcache.so, etc.)
     * may have its own __emutls_v._tsrm_ls_cache control struct.
     * Due to ELF symbol interposition, a DSO's code might use EITHER
     * its own control struct OR the first-defined one (libphp.so's).
     *
     * We fix EVERY DSO that has __emutls_v._tsrm_ls_cache by:
     * 1. Finding each DSO's control via dlsym(specific_handle, ...)
     * 2. Setting the thread-local value via __emutls_get_address
     *
     * We also use RTLD_DEFAULT to find the interposed (first-in-chain)
     * control and fix that too, covering the case where opcache.so uses
     * the interposed symbol from libphp.so.
     */

    /* Fix the default/interposed symbol first (covers all interposed DSOs) */
    void *ctrl_default = dlsym(RTLD_DEFAULT, "__emutls_v._tsrm_ls_cache");
    if (ctrl_default)
    {
        void **tls_addr = (void **)__emutls_get_address(ctrl_default);
        if (tls_addr)
        {
            ENGINE_LOGI("Fixing RTLD_DEFAULT _tsrm_ls_cache: was=%p, setting=%p",
                        *tls_addr, cache);
            *tls_addr = cache;
        }
    }
    else
    {
        ENGINE_LOGI("RTLD_DEFAULT: __emutls_v._tsrm_ls_cache not found");
    }

    /* Fix opcache.so's own copy — use full path because Android's
     * linker only matches by full path or soname, and a bare
     * "opcache.so" often fails with RTLD_NOLOAD on Android.
     *
     * CRITICAL: Use call_emutls_get_address() which calls opcache.so's
     * own __emutls_get_address.  opcache.so ships a statically-linked
     * WEAK copy from libcompiler_rt with its own per-thread array.
     * Using libc's __emutls_get_address writes to the WRONG array. */
    void *handle = dlopen(s_opcache_path[0] ? s_opcache_path : "opcache.so",
                          RTLD_NOLOAD | RTLD_LAZY);
    if (!handle)
    {
        ENGINE_LOGW("dlopen(RTLD_NOLOAD) failed for opcache: %s — trying full path",
                    dlerror() ? dlerror() : "unknown");
        if (s_opcache_path[0])
        {
            handle = dlopen(s_opcache_path, RTLD_LAZY);
        }
    }
    if (handle)
    {
        void *ctrl = dlsym(handle, "__emutls_v._tsrm_ls_cache");
        if (ctrl)
        {
            void **tls_addr = (void **)call_emutls_get_address(ctrl, handle);
            if (tls_addr)
            {
                ENGINE_LOGI("Fixing opcache.so _tsrm_ls_cache (via DSO emutls): "
                            "was=%p, setting=%p",
                            *tls_addr, cache);
                *tls_addr = cache;
            }
        }
        else
        {
            ENGINE_LOGE("opcache.so: __emutls_v._tsrm_ls_cache not found: %s",
                        dlerror() ? dlerror() : "unknown");
        }
        dlclose(handle);
    }

    /* Fix libphp.so's own copy */
    void *php_handle = dlopen("libphp.so", RTLD_NOLOAD | RTLD_LAZY);
    if (php_handle)
    {
        void *ctrl = dlsym(php_handle, "__emutls_v._tsrm_ls_cache");
        if (ctrl && ctrl != ctrl_default)
        {
            void **tls_addr = (void **)__emutls_get_address(ctrl);
            if (tls_addr)
            {
                ENGINE_LOGI("Fixing libphp.so _tsrm_ls_cache: was=%p, setting=%p",
                            *tls_addr, cache);
                *tls_addr = cache;
            }
        }
        else if (ctrl == ctrl_default)
        {
            ENGINE_LOGI("libphp.so _tsrm_ls_cache same as RTLD_DEFAULT (interposed)");
        }
        dlclose(php_handle);
    }

    ENGINE_LOGI("emutls TLS fix applied for tid=%lu, cache=%p",
                (unsigned long)pthread_self(), cache);
#endif
}

/**
 * Custom module startup callback.
 *
 * Replaces php_embed_startup() so we can:
 * 1. Run php_module_startup() for core PHP initialization
 * 2. Manually load OPcache with controlled TLS fix and startup sequence
 *
 * OPcache is NOT loaded via INI (zend_extension= is omitted) because:
 * - On Android emutls, each DSO has its own _tsrm_ls_cache TLS variable
 * - accel_startup() → zend_jit_init() → ts_allocate_id() runs a
 *   constructor that uses JIT_G() which dereferences opcache.so's
 *   _tsrm_ls_cache — if that's NULL (or incorrectly set), SIGSEGV
 * - Loading manually in Step 2 gives us full control over when the
 *   TLS fix runs relative to the startup() call
 *
 * We also override the SAPI name to "cli" because OPcache's accel_find_sapi()
 * doesn't recognize "embed".
 */

/*
 * Pass-through interned string functions for calling accel_startup()
 * after php_module_startup() has returned.
 *
 * After zend_post_startup() (inside php_module_startup), the interned string
 * function pointers switch to request-mode versions that access per-request
 * CG(interned_strings) memory.  No request is active, so these crash with
 * SIGSEGV (fault addr ~0x1f2297f24 from hash table lookup on zeroed memory).
 *
 * The permanent-mode functions (zend_new_interned_string_permanent, etc.)
 * are NOT exported from libphp.so's dynamic symbol table — they were inlined
 * or given hidden visibility during the PHP build.
 *
 * These pass-through functions are safe substitutes: they skip the hash table
 * lookup entirely and just return/create the string.  This is fine because
 * accel_startup() only interns a few module/function names — the minor lack
 * of string deduplication is irrelevant.
 */
static zend_string *ZEND_FASTCALL passthrough_new_interned_string(zend_string *str)
{
    if (ZSTR_IS_INTERNED(str))
    {
        return str;
    }
    /* Return as-is — caller will use it as a normal (non-interned) string */
    return str;
}

static zend_string *ZEND_FASTCALL passthrough_string_init_interned(
    const char *str, size_t size, bool permanent)
{
    return zend_string_init(str, size, permanent);
}

static int custom_module_startup(sapi_module_struct *sapi)
{
    /*
     * Step 1: Normal module startup.
     *
     * OPcache is NOT loaded here — zend_extension= is intentionally
     * omitted from ini_entries.  This avoids the emutls TLS crash:
     * accel_startup() → zend_jit_init() → ts_allocate_id() runs
     * zend_jit_globals_ctor → zend_jit_trace_init_caches which uses
     * JIT_G() → TSRMG_STATIC → opcache.so's _tsrm_ls_cache → SIGSEGV
     * if that per-DSO TLS variable isn't set correctly.
     *
     * The opcache.* INI settings ARE in ini_entries so they go into
     * PHP's configuration_hash.  OPcache's REGISTER_INI_ENTRIES() in
     * accel_startup() picks them up when we call startup() manually.
     *
     * Redirect stderr to logcat during startup so any extension loading
     * errors from zend_load_extension() (which uses fprintf(stderr, ...))
     * are visible instead of lost to /dev/null.
     */
    int old_stderr = -1;
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stderr_pipe) == 0)
    {
        old_stderr = dup(STDERR_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stderr_pipe[1]);
        stderr_pipe[1] = -1;
        /* Make read end non-blocking */
        int flags = fcntl(stderr_pipe[0], F_GETFL, 0);
        fcntl(stderr_pipe[0], F_SETFL, flags | O_NONBLOCK);
    }

    int rc = php_module_startup(sapi, NULL);

    /* Drain captured stderr and log to logcat */
    if (stderr_pipe[0] >= 0)
    {
        /* Restore stderr first */
        if (old_stderr >= 0)
        {
            dup2(old_stderr, STDERR_FILENO);
            close(old_stderr);
        }
        char stderr_buf[4096];
        ssize_t n;
        while ((n = read(stderr_pipe[0], stderr_buf, sizeof(stderr_buf) - 1)) > 0)
        {
            stderr_buf[n] = '\0';
            ENGINE_LOGE("php_module_startup STDERR: %s", stderr_buf);
        }
        close(stderr_pipe[0]);
    }

    if (rc != SUCCESS)
    {
        ENGINE_LOGE("php_module_startup FAILED");
        return rc;
    }

    ENGINE_LOGI("php_module_startup succeeded");
    log_loaded_zend_extensions();

    /*
     * Step 2: Manual OPcache loading with controlled TLS fix.
     *
     * zend_load_extension() only *registers* the extension — it does NOT
     * call startup() or post_startup().  Without those:
     *   - OPcache's INI entries are never registered (all read as NULL)
     *   - accel_startup() never runs (no SHM/file-cache init)
     *   - accel_post_startup() never runs (compiler hook never installed)
     *
     * We must manually invoke the full startup sequence:
     *   1. Pre-load with RTLD_GLOBAL → ensure emutls control exists
     *   2. zend_load_extension()     → register in zend_extensions list
     *   3. fix TLS                   → set opcache.so's _tsrm_ls_cache
     *   4. ext->startup(ext)         → accel_startup(): register INI,
     *                                   init file cache, call zend_jit_init()
     *   5. fix TLS again             → startup may call ts_allocate_id()
     *   6. zend_post_startup_cb()    → install compiler hook
     */
    if (s_opcache_path[0])
    {
        /* Pre-load opcache.so with RTLD_GLOBAL so its symbols (including
         * the emutls control struct) are fully resolved before we try
         * to set the TLS value.  The handle is intentionally leaked. */
        void *pre_handle = dlopen(s_opcache_path, RTLD_NOW | RTLD_GLOBAL);
        if (pre_handle)
        {
            ENGINE_LOGI("Pre-loaded opcache.so for emutls TLS fix");
        }
        else
        {
            ENGINE_LOGE("Pre-load opcache.so failed: %s",
                        dlerror() ? dlerror() : "unknown");
        }

        ENGINE_LOGI("Loading OPcache manually: %s", s_opcache_path);
        if (zend_load_extension(s_opcache_path) == SUCCESS)
        {
            ENGINE_LOGI("Manual zend_load_extension(opcache) succeeded");

            /* Re-fix TLS before calling startup(). ts_allocate_id() during
             * php_module_startup() may have reallocated the TSRM storage array,
             * invalidating the per-DSO _tsrm_ls_cache in opcache.so. */
            fix_opcache_tls_cache();

            zend_extension *opcache_ext = zend_get_extension("Zend OPcache");
            if (opcache_ext && opcache_ext->startup)
            {

                /*
                 * Temporarily replace interned string functions with
                 * pass-through versions that don't access the request-mode
                 * hash table.  See comment above custom_module_startup()
                 * for full explanation.
                 */
                typeof(zend_new_interned_string) saved_nis = zend_new_interned_string;
                typeof(zend_string_init_interned) saved_sii = zend_string_init_interned;
                zend_new_interned_string = passthrough_new_interned_string;
                zend_string_init_interned = passthrough_string_init_interned;
                ENGINE_LOGI("Switched interned strings to pass-through mode "
                            "for accel_startup()");

                ENGINE_LOGI("Calling OPcache startup() — registers INI entries, "
                            "applies values from configuration_hash, inits file cache");
                int startup_rc = opcache_ext->startup(opcache_ext);
                ENGINE_LOGI("OPcache startup() returned %d", startup_rc);

                /* Restore request-mode interned strings */
                zend_new_interned_string = saved_nis;
                zend_string_init_interned = saved_sii;
                ENGINE_LOGI("Restored interned string functions");

                /* Re-fix TLS after startup — accel_startup() may have called
                 * ts_allocate_id() to register ZCG globals. */
                fix_opcache_tls_cache();

                /* accel_startup() should have set zend_post_startup_cb to
                 * accel_post_startup.  Call it to install the compiler hook. */
                if (zend_post_startup_cb)
                {
                    ENGINE_LOGI("Calling accel_post_startup() via zend_post_startup_cb");
                    zend_result post_rc = zend_post_startup_cb();
                    ENGINE_LOGI("accel_post_startup() returned %d", post_rc);
                }
                else
                {
                    ENGINE_LOGW("zend_post_startup_cb is NULL after startup — "
                                "accel_find_sapi() may have rejected SAPI '%s'",
                                sapi_module.name);
                }
            }
        }
        else
        {
            ENGINE_LOGE("Manual zend_load_extension(opcache) FAILED");
        }
        log_loaded_zend_extensions();

        /* Verify OPcache INI values after startup() registered them. */
        {
            const char *enable_cli = zend_ini_string("opcache.enable_cli",
                                                     sizeof("opcache.enable_cli") - 1, 0);
            const char *enable = zend_ini_string("opcache.enable",
                                                 sizeof("opcache.enable") - 1, 0);
            const char *mem = zend_ini_string("opcache.memory_consumption",
                                              sizeof("opcache.memory_consumption") - 1, 0);
            const char *fc = zend_ini_string("opcache.file_cache",
                                             sizeof("opcache.file_cache") - 1, 0);
            ENGINE_LOGI("OPcache INI check: enable=%s enable_cli=%s memory=%s file_cache=%s",
                        enable ? enable : "(null)",
                        enable_cli ? enable_cli : "(null)",
                        mem ? mem : "(null)",
                        fc ? fc : "(null)");

            /* Save the correct hash table pointer for worker threads.
             * zend_post_startup() inside php_module_startup() replaces
             * registered_zend_ini_directives with an interned copy.
             * OPcache entries are added AFTER that, so only the main
             * thread's table has them. */
            s_correct_ini_directives = EG(ini_directives);
        }
    }

    /*
     * Step 3: Re-apply TLS fix after all extensions loaded.
     * ts_allocate_id() (called during accel_startup) reallocs the per-thread
     * storage array, but the tsrm_tls_entry struct pointer remains stable.
     * This re-fix is belt-and-suspenders; the ZCG() macro dereferences
     * through the struct so the realloc'd storage is always correctly accessed.
     */
    fix_opcache_tls_cache();

    /*
     * Log OPcache status after full initialization.
     *
     * With SAPI "cli" and opcache.enable_cli=1, accel_post_startup() should
     * have run fully: SHM allocated, compiler hook installed, accel_startup_ok=true.
     */
    {
        zend_extension *opcache_ext = zend_get_extension("Zend OPcache");
        if (opcache_ext)
        {
            ENGINE_LOGI("OPcache fully initialized: activate=%p, deactivate=%p",
                        (void *)opcache_ext->activate,
                        (void *)opcache_ext->deactivate);

            /* Store activate callback for worker thread re-invocation */
            s_opcache_activate = opcache_ext->activate;

            /*
             * Keep OPcache's per-request activate/deactivate enabled.
             *
             * accel_activate() sets ZCG(enabled)=true which is required for
             * persistent_compile_file() to actually cache bytecode.  Without it,
             * the compiler hook falls through to the original compile_file and
             * OPcache reports NOT_AVAILABLE.
             *
             * The earlier concern about accel_activate() using a direct/PC-relative
             * reference to the emutls control struct is resolved: our TLS fix using
             * opcache.so's own __emutls_get_address writes to opcache.so's per-thread
             * emutls array.  Both GOT-based and PC-relative references in opcache.so
             * resolve through the same emutls function and the same per-thread array,
             * so all TLS paths see the correct _tsrm_ls_cache value.
             *
             * Worker thread logs confirm this: opcache.so's TLS is already correct
             * (was=X, setting=X) when fix_opcache_tls_cache() runs on worker threads.
             */
        }
        else
        {
            ENGINE_LOGI("OPcache not available after startup");
        }
    }

    return SUCCESS;
}

/*
 * fix_opcache_request_state() — Fix OPcache per-thread state on worker threads.
 *
 * Must be called AFTER php_request_startup() on each worker thread request.
 *
 * Problem:
 *   In ZTS mode with file_cache_only=1, OPcache's accel_globals_ctor() memsets
 *   all per-thread globals to zero.  ZCG(enabled) starts as false.  During
 *   php_request_startup(), zend_ini_activate() calls the OnEnable callback with
 *   PHP_INI_STAGE_ACTIVATE, but OnEnable only processes STARTUP/SHUTDOWN stages
 *   → ZCG(enabled) stays false.  When accel_activate() runs (via
 *   zend_activate_modules or zend_activate_extensions), it checks ZCG(enabled)
 *   and returns early without enabling the accelerator.
 *
 * Fix:
 *   1. Force opcache.enable=1 with PHP_INI_STAGE_STARTUP so OnEnable updates
 *      ZCG(enabled) to true.
 *   2. Re-invoke accel_activate() so it sets ZCG(accelerator_enabled)=true
 *      and initializes the per-request file cache state.
 */
void fix_opcache_request_state(void)
{
    if (!s_opcache_activate || !s_correct_ini_directives)
        return;

    /*
     * Trigger OPcache INI OnUpdate callbacks for per-thread globals.
     *
     * Worker threads have a stale EG(ini_directives) (172 entries, no
     * OPcache).  We CANNOT replace it — other PHP code (error_reporting,
     * ini_set, etc.) modifies shared entries via efree, which would crash
     * because the entries' strings belong to a different thread's heap.
     *
     * Instead, iterate the CORRECT table (s_correct_ini_directives) from
     * the main thread and call on_modify for opcache.* entries only.
     * The callbacks write to per-thread TSRM globals (ZCG fields) using
     * mh_arg offsets — no shared entries are modified.
     *
     * Use PHP_INI_STAGE_STARTUP so OnEnable accepts the value and sets
     * ZCG(enabled) = true.
     */
    {
        zend_string *key;
        zend_ini_entry *ini_entry;
        int callbacks_called = 0;

        ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(s_correct_ini_directives, key, ini_entry)
        {
            if (key && ini_entry->on_modify &&
                ZSTR_LEN(key) > 8 &&
                memcmp(ZSTR_VAL(key), "opcache.", 8) == 0)
            {

                zend_string *value = ini_entry->value;
                if (!value)
                    continue;

                ini_entry->on_modify(
                    ini_entry, value,
                    ini_entry->mh_arg1,
                    ini_entry->mh_arg2,
                    ini_entry->mh_arg3,
                    PHP_INI_STAGE_STARTUP);
                callbacks_called++;
            }
        }
        ZEND_HASH_FOREACH_END();

        ENGINE_LOGI("fix_opcache: triggered %d OPcache INI on_modify callbacks",
                    callbacks_called);
    }

    /* Re-invoke accel_activate() — ZCG(enabled) should now be true */
    s_opcache_activate();
}

/* ─── Public API ─── */

int php_engine_init(const char *ini_path,
                    const char *ini_entries,
                    const char *app_base_path)
{
    int result = -1;

    pthread_mutex_lock(&s_engine_mutex);

    if (s_engine_initialized)
    {
        ENGINE_LOGI("Engine already initialized, skipping");
        result = 0;
        goto done;
    }

    /* Verify ZTS at runtime (belt + suspenders with compile-time check) */
#ifndef ZTS
    ENGINE_LOGE("FATAL: ZTS not defined at compile time. This binary is NTS.");
    goto done;
#endif

    ENGINE_LOGI("Initializing PHP engine (ZTS mode)...");

    /* ─── Derive ini_path from app_base_path when not provided ───
     *
     * The PhpWorkerService may start before LaravelEnvironment and
     * call nativeEngineInit("", "", appBasePath) — the JNI bridge
     * converts "" to NULL.  But the php.ini lives in context.filesDir
     * which is always <data_dir>/files, and app_base_path is always
     * <data_dir>/app_storage/laravel.
     *
     * Derive: strip "/app_storage/laravel" from app_base_path, append "/files".
     */
    static char derived_ini_path[2048] = {0};
    if (!ini_path && app_base_path)
    {
        const char *suffix = "/app_storage/laravel";
        size_t base_len = strlen(app_base_path);
        size_t suffix_len = strlen(suffix);
        if (base_len > suffix_len &&
            strcmp(app_base_path + base_len - suffix_len, suffix) == 0)
        {
            size_t prefix_len = base_len - suffix_len;
            if (prefix_len + 6 < sizeof(derived_ini_path))
            { /* 6 = strlen("/files") */
                memcpy(derived_ini_path, app_base_path, prefix_len);
                memcpy(derived_ini_path + prefix_len, "/files", 7); /* includes NUL */
                ini_path = derived_ini_path;
                ENGINE_LOGI("Derived ini_path from app_base_path: %s", ini_path);
            }
        }
    }

    ENGINE_LOGI("  ini_path: %s", ini_path ? ini_path : "(none)");
    ENGINE_LOGI("  app_base_path: %s", app_base_path ? app_base_path : "(none)");

    /* Store app path */
    if (app_base_path)
    {
        strncpy(s_app_base_path, app_base_path, sizeof(s_app_base_path) - 1);
        s_app_base_path[sizeof(s_app_base_path) - 1] = '\0';
    }

    /* ─── Auto-detect native library directory for extension_dir ───
     * Use dladdr() on a known PHP symbol to find where libphp.so is loaded,
     * then derive the directory.  This lets PHP find opcache.so if bundled. */
    char ext_dir[1024] = {0};
    {
        Dl_info di;
        if (dladdr((void *)php_embed_init, &di) && di.dli_fname)
        {
            strncpy(ext_dir, di.dli_fname, sizeof(ext_dir) - 1);
            /* Trim filename to get directory */
            char *last_slash = strrchr(ext_dir, '/');
            if (last_slash)
                *last_slash = '\0';
            ENGINE_LOGI("Auto-detected extension_dir: %s", ext_dir);

            /*
             * RTLD_GLOBAL promotion for libphp.so.
             *
             * The primary RTLD_GLOBAL loading is handled by php_preloader.c
             * (loaded via System.loadLibrary("php_preloader") BEFORE "php").
             * On Android API 36+, re-opening an already RTLD_LOCAL library
             * with dlopen(RTLD_GLOBAL) does NOT reliably promote its symbols.
             *
             * This code remains as a belt-and-suspenders fallback for devices
             * where the preloader approach might not apply, or for older API
             * levels where post-load promotion does work.
             */
            void *php_global = dlopen(di.dli_fname, RTLD_NOW | RTLD_GLOBAL);
            if (php_global)
            {
                ENGINE_LOGI("libphp.so RTLD_GLOBAL promotion attempted");
            }
            else
            {
                const char *dl_err = dlerror();
                ENGINE_LOGE("Failed to re-open libphp.so with RTLD_GLOBAL: %s",
                            dl_err ? dl_err : "unknown");
            }
        }

        if (!ext_dir[0])
        {
            ENGINE_LOGE("Could not auto-detect extension_dir via dladdr(php_embed_init)");
        }

        if (ext_dir[0])
        {
            snprintf(s_opcache_path, sizeof(s_opcache_path), "%s/opcache.so", ext_dir);

            if (access(s_opcache_path, R_OK) == 0)
            {
                ENGINE_LOGI("Found opcache candidate: %s", s_opcache_path);
            }
            else
            {
                ENGINE_LOGE("opcache.so not readable at expected path: %s", s_opcache_path);
                s_opcache_path[0] = '\0';
            }
        }
    }

    /* Build INI entries */
    s_ini_entries_buf[0] = '\0';

    /* ─── Auto-detect OPcache file cache directory ───
     * Use app_base_path/storage/framework/opcache/ as the file cache dir.
     * This persists compiled bytecode to disk across cold boots.
     * The directory is created by WorkerServiceProvider; if it doesn't
     * exist yet, OPcache silently falls back to SHM-only mode. */
    char opcache_file_cache_dir[2048] = {0};
    if (app_base_path)
    {
        snprintf(opcache_file_cache_dir, sizeof(opcache_file_cache_dir),
                 "%s/storage/framework/opcache", app_base_path);
    }

    snprintf(s_ini_entries_buf, sizeof(s_ini_entries_buf),
             "output_buffering=0\n"
             "implicit_flush=0\n"
             "display_errors=1\n"
             "log_errors=1\n"
             "error_reporting=E_ALL\n"
             "memory_limit=512M\n"
             "max_execution_time=300\n"
             "register_argc_argv=1\n"
             /* ─── Extension loading ───
              * extension_dir tells PHP where shared extensions live.
              *
              * zend_extension is intentionally OMITTED here.  OPcache is
              * loaded manually in custom_module_startup() Step 2 to avoid
              * the emutls TLS crash during php_module_startup() — see the
              * comment on custom_module_startup() for details.
              *
              * Paths MUST be quoted because Android APK paths contain ~
              * (e.g. /data/app/~~hash==/pkg/lib/arm64/) and the PHP INI
              * parser treats ~ as a bitwise NOT operator. */
             "extension_dir=\"%s\"\n"
             "opcache.enable=1\n"
             "opcache.enable_cli=1\n"
             "opcache.memory_consumption=32\n"
             "opcache.interned_strings_buffer=8\n"
             "opcache.max_accelerated_files=4000\n"
             "opcache.validate_timestamps=0\n"
             "opcache.save_comments=1\n"
             "opcache.file_update_protection=0\n"
             /* ─── OPcache file cache: persist bytecode to disk ───
              * When set, OPcache saves compiled scripts to this directory.
              * On cold boot, previously compiled files load from disk
              * instead of being recompiled, saving ~30-50% bootstrap time.
              * file_cache_only=1: Android lacks POSIX shared memory (shm_open),
              * so SHM allocation always fails. File-cache-only avoids the
              * failed mmap attempt and still provides full bytecode caching. */
             "opcache.file_cache=\"%s\"\n"
             "opcache.file_cache_only=1\n"
             "opcache.file_cache_consistency_checks=0\n"
             /* ─── Disable JIT on Android ───
              * JIT requires mmap(PROT_EXEC) for an anonymous memory region.
              * Android's SELinux W^X policy blocks this on most devices.
              * zend_jit_init() is called unconditionally when HAVE_JIT is
              * defined, but with buffer_size=0 and jit=disable, the JIT
              * code paths become no-ops after initialization. */
             "opcache.jit=disable\n"
             "opcache.jit_buffer_size=0\n"
             "%s",
             ext_dir[0] ? ext_dir : "/dev/null",
             opcache_file_cache_dir[0] ? opcache_file_cache_dir : "",
             ini_entries ? ini_entries : "");

    /* Configure embed SAPI for module-level init */
    php_embed_module.ub_write = engine_init_ub_write;
    php_embed_module.phpinfo_as_text = 1;
    php_embed_module.php_ini_ignore = (ini_path == NULL) ? 1 : 0;
    php_embed_module.ini_entries = s_ini_entries_buf;
    php_embed_module.additional_functions = nativephp_bridge_functions;
    php_embed_module.header_handler = android_header_handler;
    php_embed_module.log_message = android_sapi_log_message;

    /* Use our custom startup so we can fix emutls TLS after extensions load */
    php_embed_module.startup = custom_module_startup;

    /*
     * Override SAPI name so OPcache's accel_find_sapi() recognizes us.
     *
     * OPcache has a hardcoded list of allowed SAPI names.  The "embed" SAPI
     * is NOT in the list.  Without this override, accel_post_startup() bails
     * out early → accel_startup_ok stays false → compiler hook never installed
     * → zero bytecode caching.
     *
     * "cli" is allowed when opcache.enable_cli=1 (which we set in INI).
     * The embed SAPI is functionally equivalent to CLI for our purposes
     * (single-process, long-lived, no forking).
     */
    php_embed_module.name = "cli";

    if (ini_path)
    {
        php_embed_module.php_ini_path_override = (char *)ini_path;
    }

    /* ─── Prevent fork() deadlock: set terminal size BEFORE threads ───
     * Symfony Console's Terminal::getWidth() checks getenv('COLUMNS')
     * before calling initDimensions() → proc_open('stty') → fork().
     * fork() in a multithreaded process can deadlock the child when it
     * inherits locked mutexes from other threads.
     *
     * Setting COLUMNS/LINES here is safe: this runs on the main thread
     * before any worker threads are created, so there are no concurrent
     * getenv/setenv races. The PHP-level fix in common.php (Reflection
     * on Terminal::$width) is the primary defense; this is a backup. */
    setenv("COLUMNS", "80", 0);
    setenv("LINES", "50", 0);

    /*
     * php_embed_init does:
     *  1. tsrm_startup (ZTS)
     *  2. sapi_startup
     *  3. custom_module_startup (our callback → php_module_startup + OPcache fix)
     *  4. php_request_startup (for the calling thread)
     *
     * Step 3 uses our custom_module_startup which:
     *  a. Calls php_module_startup() to load all extensions from INI
     *  b. If OPcache didn't load from INI, loads it manually
     *  c. Fixes opcache.so's emutls TLS cache for the main thread
     *
     * After init, we immediately do php_request_shutdown for the main
     * thread since it won't be running PHP scripts directly in this
     * context.
     */
    int argc = 1;
    char *argv_buf[] = {"php", NULL};
    char **argv = argv_buf;

    ENGINE_LOGI("php_ini_ignore=%d, ini_path=%s",
                php_embed_module.php_ini_ignore,
                ini_path ? ini_path : "(none)");
    ENGINE_LOGI("ini_entries len=%zu, zend_extension included=%s",
                strlen(s_ini_entries_buf),
                s_opcache_path[0] ? "yes" : "no");
    /* Log a snippet of ini_entries around zend_extension for debugging */
    {
        const char *ze = strstr(s_ini_entries_buf, "zend_extension");
        if (ze)
        {
            char snippet[200];
            int before = (ze - s_ini_entries_buf) > 20 ? 20 : (int)(ze - s_ini_entries_buf);
            strncpy(snippet, ze - before, sizeof(snippet) - 1);
            snippet[sizeof(snippet) - 1] = '\0';
            /* Truncate at newline for readability */
            char *nl = strchr(snippet + before + 14, '\n');
            if (nl)
                *(nl + 1) = '\0';
            ENGINE_LOGI("ini_entries snippet: [%s]", snippet);
        }
    }

    if (php_embed_init(argc, argv) != SUCCESS)
    {
        ENGINE_LOGE("php_embed_init FAILED");
        goto done;
    }

    /* php_embed_init's step 4 (php_request_startup) already ran
     * successfully — meaning accel_activate() did NOT crash.
     * This confirms the emutls TLS fix worked. */
    ENGINE_LOGI("php_embed_init succeeded (request active on main thread)");
    ENGINE_LOGI("sapi_module.name = '%s'", sapi_module.name ? sapi_module.name : "(null)");
    log_loaded_zend_extensions();

    /* Quick PHP-level diagnostic: check if opcache functions exist on main thread.
     * ub_write is still engine_init_ub_write (logcat) at this point. */
    {
        zval retval;
        zend_eval_string("echo 'OPCACHE_DIAG_MAIN: fn_exists=' . (function_exists('opcache_get_status') ? 'YES' : 'NO') . ' exts=' . implode(',', get_loaded_extensions()) . \"\\n\";", &retval, "opcache_diag");
        zval_ptr_dtor(&retval);
    }

    /*
     * Shut down the request that php_embed_init started for the main thread.
     * The engine (module) stays alive. Workers will do their own
     * request_startup/shutdown per job.
     */
    php_request_shutdown(NULL);

    /*
     * Install TLS-routed output handler for worker threads.
     * php_request_ub_write routes output to the per-thread
     * PhpRequestContext buffer via thread-local storage.
     *
     * IMPORTANT: We must update sapi_module directly, not php_embed_module.
     * sapi_startup() copies php_embed_module into sapi_module by value,
     * so any changes to php_embed_module after init are NOT reflected
     * in the live sapi_module that PHP actually uses.
     */
    sapi_module.ub_write = php_request_ub_write;
    php_embed_module.ub_write = php_request_ub_write;
    sapi_module.log_message = android_sapi_log_message;

    s_engine_initialized = 1;
    result = 0;

    /* ─── Set static env vars that are the same for every request ───
     * These used to be set via setenv() in run_php_script_once() per request,
     * which was both redundant and a thread-safety hazard.  Setting them once
     * here is sufficient since they never change. */
    setenv("PHP_SELF", "/native.php", 1);
    setenv("HTTP_HOST", "127.0.0.1", 1);
    setenv("APP_URL", "http://127.0.0.1", 1);
    setenv("ASSET_URL", "http://127.0.0.1/_assets/", 1);
    setenv("NATIVEPHP_RUNNING", "true", 1);

    ENGINE_LOGI("PHP engine initialized successfully (ZTS=%d, version=%s)",
#ifdef ZTS
                1,
#else
                0,
#endif
                PHP_VERSION);

    /* ─── OPcache warmup ───
     * Run a throwaway PHP request that requires the Composer autoloader
     * and key framework files.  This populates OPcache's shared memory
     * segment BEFORE worker threads start, so all threads benefit from
     * cached bytecode on their very first real job.
     *
     * The emutls TLS fix was applied in custom_module_startup() so
     * php_request_startup() → accel_activate() is safe here.
     *
     * On failure this is a no-op — the warmup is purely an optimisation. */

    /* Check if OPcache's compiler hook was installed.
     * If accel_post_startup() ran fully (SAPI accepted, SHM allocated),
     * zend_compile_file should point to persistent_compile_file in opcache.so
     * rather than the default compiler in libphp.so. */
    {
        extern zend_op_array *(*zend_compile_file)(zend_file_handle *, int);
        Dl_info di_compile = {0};
        if (dladdr((void *)zend_compile_file, &di_compile) && di_compile.dli_fname)
        {
            ENGINE_LOGI("zend_compile_file → %s (+%p)",
                        di_compile.dli_fname,
                        (void *)((char *)zend_compile_file - (char *)di_compile.dli_fbase));
            /* If it points into opcache.so, the hook is installed */
            ENGINE_LOGI("OPcache compiler hook: %s",
                        strstr(di_compile.dli_fname, "opcache") ? "INSTALLED" : "NOT installed (still default)");
        }
        else
        {
            ENGINE_LOGI("zend_compile_file = %p (dladdr failed)", (void *)zend_compile_file);
        }
    }

    if (s_app_base_path[0] != '\0' && zend_get_extension("Zend OPcache"))
    {
        char warmup_code[4096];
        snprintf(warmup_code, sizeof(warmup_code),
                 "<?php\n"
                 "/* OPcache warmup — precompile autoloader + framework */\n"
                 "$autoload = '%s/vendor/autoload.php';\n"
                 "if (file_exists($autoload)) {\n"
                 "    require $autoload;\n"
                 "    /* Touch key framework classes so they enter OPcache SHM */\n"
                 "    if (file_exists('%s/vendor/laravel/framework/src/Illuminate/Foundation/Application.php')) {\n"
                 "        require_once '%s/vendor/laravel/framework/src/Illuminate/Foundation/Application.php';\n"
                 "    }\n"
                 "    if (file_exists('%s/vendor/laravel/framework/src/Illuminate/Container/Container.php')) {\n"
                 "        require_once '%s/vendor/laravel/framework/src/Illuminate/Container/Container.php';\n"
                 "    }\n"
                 "}\n"
                 "/* Log OPcache status after warmup */\n"
                 "$s = function_exists('opcache_get_status') ? opcache_get_status(false) : null;\n"
                 "if ($s) {\n"
                 "    $en = $s['opcache_enabled'] ? 'YES' : 'NO';\n"
                 "    $cached = $s['opcache_statistics']['num_cached_scripts'] ?? 0;\n"
                 "    $hits = $s['opcache_statistics']['hits'] ?? 0;\n"
                 "    $mem = isset($s['memory_usage']['used_memory']) ? round($s['memory_usage']['used_memory']/1048576,1) : '?';\n"
                 "    error_log(\"OPcache warmup status: enabled=$en cached=$cached hits=$hits mem={$mem}MB\");\n"
                 "} else {\n"
                 "    error_log('OPcache warmup: opcache_get_status=' . var_export($s, true) . ' fn_exists=' . (function_exists('opcache_get_status') ? 'YES' : 'NO'));\n"
                 "}\n",
                 s_app_base_path, s_app_base_path, s_app_base_path,
                 s_app_base_path, s_app_base_path);

        /* Execute warmup in a dedicated request cycle on the main thread.
         * Temporarily switch ub_write to logcat so PHP output is visible. */
        sapi_module.ub_write = engine_init_ub_write;

        php_request_startup();

        zend_try
        {
            zend_eval_string((char *)warmup_code, NULL, "opcache_warmup");
            ENGINE_LOGI("OPcache warmup completed");
        }
        zend_catch
        {
            ENGINE_LOGI("OPcache warmup skipped (non-fatal)");
        }
        zend_end_try();

        php_request_shutdown(NULL);

        /* Restore TLS-routed output handler after warmup */
        sapi_module.ub_write = php_request_ub_write;
    }

done:
    pthread_mutex_unlock(&s_engine_mutex);
    return result;
}

void php_engine_shutdown(void)
{
    pthread_mutex_lock(&s_engine_mutex);

    if (!s_engine_initialized)
    {
        ENGINE_LOGI("Engine not initialized, nothing to shutdown");
        pthread_mutex_unlock(&s_engine_mutex);
        return;
    }

    ENGINE_LOGI("Shutting down PHP engine...");

    /*
     * php_embed_shutdown does:
     *  1. php_request_shutdown (if a request is active)
     *  2. php_module_shutdown
     *  3. sapi_shutdown
     *  4. tsrm_shutdown
     *
     * We need to start a request first since php_embed_shutdown
     * expects to shut one down. Start a dummy request.
     */
    php_request_startup();
    php_embed_shutdown();

    s_engine_initialized = 0;
    s_app_base_path[0] = '\0';

    ENGINE_LOGI("PHP engine shutdown complete");

    pthread_mutex_unlock(&s_engine_mutex);
}

int php_engine_is_initialized(void)
{
    int result;
    pthread_mutex_lock(&s_engine_mutex);
    result = s_engine_initialized;
    pthread_mutex_unlock(&s_engine_mutex);
    return result;
}

const char *php_engine_get_app_path(void)
{
    /* Read-only after init; safe without lock if engine is alive */
    return s_app_base_path;
}

int php_engine_verify_zts(void)
{
#ifdef ZTS
    return 1;
#else
    return 0;
#endif
}
