/**
 * php_preloader.c — RTLD_GLOBAL preloader for libphp.so
 *
 * On Android, System.loadLibrary() loads shared libraries with RTLD_LOCAL,
 * meaning their symbols are NOT visible to subsequently dlopen'd libraries.
 * This is a problem for PHP extensions like opcache.so that have undefined
 * symbols (execute_ex, zend_execute_ex, etc.) expecting resolution from
 * the host PHP binary (libphp.so).
 *
 * On Android API 36 (Android 16) and potentially earlier versions, re-opening
 * an already-loaded RTLD_LOCAL library with dlopen(path, RTLD_GLOBAL) does NOT
 * reliably promote its symbols to global scope on the Bionic linker.  The
 * dlsym(RTLD_DEFAULT, ...) check is also misleading because RTLD_DEFAULT
 * searches ALL loaded objects (including RTLD_LOCAL ones), not just RTLD_GLOBAL.
 *
 * Solution: Load libphp.so with RTLD_GLOBAL *before* System.loadLibrary("php")
 * loads it with RTLD_LOCAL.  When System.loadLibrary("php") subsequently runs,
 * Bionic finds the already-loaded library and preserves its RTLD_GLOBAL flag.
 * Extensions like opcache.so can then resolve their undefined symbols from
 * global scope.
 *
 * Required Kotlin load order:
 *   1. System.loadLibrary("compat")
 *   2. System.loadLibrary("php_preloader")  ← THIS library
 *   3. System.loadLibrary("php")            ← finds already-loaded, keeps RTLD_GLOBAL
 *   4. System.loadLibrary("php_wrapper")
 */
#include <jni.h>
#include <dlfcn.h>
#include <string.h>
#include <android/log.h>

#define TAG "PhpPreloader"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/**
 * Static marker function used with dladdr() to discover the directory
 * containing this library (and by extension, libphp.so).
 * Using a local static avoids any PLT/GOT ambiguity with JNI_OnLoad.
 */
static void preloader_marker(void) { /* intentionally empty */ }

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)vm;
    (void)reserved;

    /*
     * Step 1: Discover our own library path via dladdr().
     * Since this library is loaded from the same APK native-lib directory
     * as libphp.so, we can derive the path by replacing our filename.
     */
    Dl_info di;
    if (!dladdr((void *)preloader_marker, &di) || !di.dli_fname)
    {
        LOGE("dladdr failed — cannot determine native library directory");
        return JNI_VERSION_1_6;
    }

    LOGI("Preloader library path: %s", di.dli_fname);

    /* Step 2: Derive the path to libphp.so in the same directory. */
    char php_path[1024];
    strncpy(php_path, di.dli_fname, sizeof(php_path) - 1);
    php_path[sizeof(php_path) - 1] = '\0';

    char *last_slash = strrchr(php_path, '/');
    if (!last_slash)
    {
        LOGE("Unexpected library path format (no '/'): %s", di.dli_fname);
        return JNI_VERSION_1_6;
    }

    /* Replace our filename with "libphp.so" */
    size_t dir_len = (size_t)(last_slash - php_path);
    if (dir_len + sizeof("/libphp.so") >= sizeof(php_path))
    {
        LOGE("Path too long to append /libphp.so");
        return JNI_VERSION_1_6;
    }
    snprintf(last_slash, sizeof(php_path) - dir_len, "/libphp.so");

    /*
     * Step 3: Load libphp.so with RTLD_NOW | RTLD_GLOBAL.
     *
     * This is the critical step.  By loading libphp.so here — BEFORE
     * System.loadLibrary("php") — the library enters global scope first.
     * Bionic's linker preserves RTLD_GLOBAL once set; the subsequent
     * System.loadLibrary("php") call will find the already-loaded soinfo
     * and reuse it without downgrading the flags.
     *
     * RTLD_NOW ensures all symbols are resolved immediately.  If libphp.so
     * has unresolvable dependencies, we fail fast with a clear error.
     */
    LOGI("Pre-loading libphp.so with RTLD_GLOBAL: %s", php_path);

    dlerror(); /* clear any stale error */
    void *handle = dlopen(php_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
    {
        const char *err = dlerror();
        LOGE("FATAL: Failed to pre-load libphp.so with RTLD_GLOBAL: %s",
             err ? err : "unknown");
        /*
         * Do not abort — fall through so the app can still attempt
         * System.loadLibrary("php") which will load with RTLD_LOCAL.
         * OPcache will be unavailable but the app remains functional.
         */
        return JNI_VERSION_1_6;
    }

    /*
     * Step 4: Verify key symbols are in global scope.
     *
     * NOTE: dlsym(RTLD_DEFAULT, ...) on Bionic searches ALL loaded objects
     * (including RTLD_LOCAL), so this is not a definitive proof of GLOBAL
     * scope.  However, a NULL result would definitively indicate failure.
     * The real proof comes later when opcache.so loads successfully.
     */
    void *exec_sym = dlsym(RTLD_DEFAULT, "execute_ex");
    void *zexec_sym = dlsym(RTLD_DEFAULT, "zend_execute_ex");

    LOGI("libphp.so loaded with RTLD_GLOBAL — execute_ex=%p, zend_execute_ex=%p",
         exec_sym, zexec_sym);

    if (!exec_sym)
    {
        LOGW("execute_ex not found via dlsym — libphp.so may be missing expected symbols");
    }

    /*
     * Intentionally do NOT call dlclose(handle).
     * The extra reference count is harmless and ensures the RTLD_GLOBAL
     * flag persists through subsequent System.loadLibrary("php") calls.
     */

    return JNI_VERSION_1_6;
}
