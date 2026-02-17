/**
 * php_engine.h — PhpEngine singleton: process-wide PHP module lifecycle
 *
 * Responsibilities:
 * - One-time php_module_startup / php_module_shutdown
 * - TSRM initialization
 * - Provides engine state queries
 * - Thread-safe via mutex
 */
#ifndef NATIVEPHP_PHP_ENGINE_H
#define NATIVEPHP_PHP_ENGINE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

    /**
     * Initialize the PHP engine (module-level).
     * Must be called once from the main thread before any worker starts.
     *
     * @param ini_path     Path to php.ini (or NULL for defaults)
     * @param ini_entries  Additional INI overrides as "key=val\n..." (or NULL)
     * @param app_base_path  Base path of the Laravel app (for include_path etc.)
     * @return 0 on success, -1 on failure
     */
    int php_engine_init(const char *ini_path,
                        const char *ini_entries,
                        const char *app_base_path);

    /**
     * Shutdown the PHP engine. Call once at app exit.
     * All workers must be stopped before calling this.
     */
    void php_engine_shutdown(void);

    /**
     * Query if the engine is initialized.
     * @return 1 if initialized, 0 otherwise
     */
    int php_engine_is_initialized(void);

    /**
     * Get the app base path set during init.
     * @return Read-only string pointer (valid while engine is active)
     */
    const char *php_engine_get_app_path(void);

    /**
     * Verify ZTS is properly enabled at runtime.
     * @return 1 if ZTS, 0 if NTS (should not happen with build enforcement)
     */
    int php_engine_verify_zts(void);

    /**
     * Fix opcache.so's _tsrm_ls_cache for the calling thread.
     *
     * On Android API < 29, emulated TLS (emutls) gives each .so its own
     * copy of __thread variables.  PHP's TSRMG_STATIC macros (used by
     * OPcache's ZCG()) read the per-DSO copy.  When a new thread starts,
     * only the calling DSO's _tsrm_ls_cache is initialized via
     * TSRMLS_CACHE_UPDATE().  opcache.so's copy remains NULL, causing
     * SIGSEGV in accel_activate().
     *
     * This function uses Android's emutls internals to directly set
     * opcache.so's _tsrm_ls_cache for the calling thread.
     *
     * Must be called AFTER ts_resource(0) + TSRMLS_CACHE_UPDATE().
     * Safe to call when OPcache is not loaded (no-op).
     */
    void fix_opcache_tls_cache(void);

    /**
     * Fix OPcache per-thread state on worker threads.
     *
     * Must be called AFTER php_request_startup() on each worker thread request.
     * Forces opcache.enable=1 with STARTUP stage and re-invokes accel_activate()
     * to set ZCG(accelerator_enabled)=true for this thread's request.
     */
    void fix_opcache_request_state(void);

#ifdef __cplusplus
}
#endif

#endif /* NATIVEPHP_PHP_ENGINE_H */
