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
extern "C" {
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

#ifdef __cplusplus
}
#endif

#endif /* NATIVEPHP_PHP_ENGINE_H */
