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

#ifdef __ANDROID__
#include <android/log.h>
#define ENGINE_TAG "PhpEngine"
#define ENGINE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, ENGINE_TAG, __VA_ARGS__)
#define ENGINE_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ENGINE_TAG, __VA_ARGS__)
#else
#define ENGINE_LOGI(...)                 \
    do                                   \
    {                                    \
        fprintf(stdout, "[PhpEngine] "); \
        fprintf(stdout, __VA_ARGS__);    \
        fprintf(stdout, "\n");           \
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
static char s_ini_entries_buf[8192] = {0};

/* ─── Dummy ub_write for module-level init (no output expected) ─── */
static size_t engine_null_ub_write(const char *str, size_t str_length)
{
    /* Swallow any output during module startup */
    (void)str;
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
        }
    }

    /* Build INI entries */
    s_ini_entries_buf[0] = '\0';
    snprintf(s_ini_entries_buf, sizeof(s_ini_entries_buf),
             "output_buffering=0\n"
             "implicit_flush=0\n"
             "display_errors=1\n"
             "error_reporting=E_ALL\n"
             "memory_limit=512M\n"
             "max_execution_time=300\n"
             "register_argc_argv=1\n"
             /* ─── OPcache: shared bytecode cache across all threads ───
              * extension_dir must come first so PHP knows where to find opcache.
              * If opcache.so isn't bundled in the APK, the zend_extension directive
              * produces a harmless warning and PHP continues without opcache. */
             "extension_dir=%s\n"
             "zend_extension=opcache\n"
             "opcache.enable=1\n"
             "opcache.enable_cli=1\n"
             "opcache.memory_consumption=32\n"
             "opcache.interned_strings_buffer=8\n"
             "opcache.max_accelerated_files=4000\n"
             "opcache.validate_timestamps=0\n"
             "opcache.save_comments=1\n"
             "opcache.file_update_protection=0\n"
             "%s",
             ext_dir[0] ? ext_dir : "/dev/null",
             ini_entries ? ini_entries : "");

    /* Configure embed SAPI for module-level init */
    php_embed_module.ub_write = engine_null_ub_write;
    php_embed_module.phpinfo_as_text = 1;
    php_embed_module.php_ini_ignore = (ini_path == NULL) ? 1 : 0;
    php_embed_module.ini_entries = s_ini_entries_buf;
    php_embed_module.additional_functions = nativephp_bridge_functions;
    php_embed_module.header_handler = android_header_handler;
    php_embed_module.log_message = android_sapi_log_message;

    if (ini_path)
    {
        php_embed_module.php_ini_path_override = (char *)ini_path;
    }

    /*
     * php_embed_init does:
     *  1. tsrm_startup (ZTS)
     *  2. sapi_startup
     *  3. php_module_startup
     *  4. php_request_startup (for the calling thread)
     *
     * We call it once from the main thread. Workers will create their
     * own interpreter contexts via tsrm_new_interpreter_context().
     *
     * After init, we immediately do php_request_shutdown for the main
     * thread since it won't be running PHP scripts directly in this
     * context.
     */
    int argc = 1;
    char *argv_buf[] = {"php", NULL};
    char **argv = argv_buf;

    if (php_embed_init(argc, argv) != SUCCESS)
    {
        ENGINE_LOGE("php_embed_init FAILED");
        goto done;
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
