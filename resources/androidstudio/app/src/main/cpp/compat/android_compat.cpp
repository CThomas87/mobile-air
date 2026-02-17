// android_compat.cpp
#include "android_compat.h"
#include <cstdio>
#include <unistd.h>
#include <sys/resource.h>
#include <syscall.h>
#include <android/log.h>
#include <jni.h>
#include <dlfcn.h>
#include <string.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Compat", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "Compat", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Compat", __VA_ARGS__)

__attribute__((visibility("default"))) extern "C" int getdtablesize(void)
{
    __android_log_print(ANDROID_LOG_INFO, "Compat", "getdtablesize called");
    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0)
    {
        return rlim.rlim_cur;
    }
    return 1024;
}

__attribute__((visibility("default"))) extern "C" ssize_t copy_file_range(int fd_in, off64_t *off_in,
                                                                          int fd_out, off64_t *off_out,
                                                                          size_t len, unsigned int flags)
{
    return syscall(__NR_copy_file_range, fd_in, off_in,
                   fd_out, off_out, len, flags);
}

/*
 * ─── RTLD_GLOBAL preloader for libphp.so ───
 *
 * Android's System.loadLibrary() uses RTLD_LOCAL, so symbols from libphp.so
 * are NOT visible to subsequently dlopen'd extensions (e.g. opcache.so needs
 * execute_ex, zend_execute_ex, etc.).
 *
 * On Android API 36+ (Android 16), re-opening an already-loaded RTLD_LOCAL
 * library with dlopen(path, RTLD_GLOBAL) does NOT reliably promote its symbols.
 *
 * Solution: Load libphp.so with RTLD_GLOBAL here in libcompat.so's JNI_OnLoad
 * — which runs BEFORE System.loadLibrary("php").  When System.loadLibrary
 * subsequently loads libphp.so, Bionic finds it already loaded with RTLD_GLOBAL
 * and preserves that flag.
 */
static void compat_marker(void) { /* for dladdr discovery */ }

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)vm;
    (void)reserved;

    Dl_info di;
    if (!dladdr((void *)compat_marker, &di) || !di.dli_fname)
    {
        LOGE("dladdr failed — cannot determine native library directory for PHP preload");
        return JNI_VERSION_1_6;
    }

    LOGI("Compat library path: %s", di.dli_fname);

    char php_path[1024];
    strncpy(php_path, di.dli_fname, sizeof(php_path) - 1);
    php_path[sizeof(php_path) - 1] = '\0';

    char *last_slash = strrchr(php_path, '/');
    if (!last_slash)
    {
        LOGE("Unexpected library path format (no '/'): %s", di.dli_fname);
        return JNI_VERSION_1_6;
    }

    size_t dir_len = (size_t)(last_slash - php_path);
    if (dir_len + sizeof("/libphp.so") >= sizeof(php_path))
    {
        LOGE("Path too long to append /libphp.so");
        return JNI_VERSION_1_6;
    }
    snprintf(last_slash, sizeof(php_path) - dir_len, "/libphp.so");

    LOGI("Pre-loading libphp.so with RTLD_GLOBAL: %s", php_path);

    dlerror();
    void *handle = dlopen(php_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
    {
        const char *err = dlerror();
        LOGE("Failed to pre-load libphp.so with RTLD_GLOBAL: %s",
             err ? err : "unknown");
        return JNI_VERSION_1_6;
    }

    void *exec_sym = dlsym(RTLD_DEFAULT, "execute_ex");
    void *zexec_sym = dlsym(RTLD_DEFAULT, "zend_execute_ex");
    LOGI("libphp.so loaded with RTLD_GLOBAL — execute_ex=%p, zend_execute_ex=%p",
         exec_sym, zexec_sym);

    if (!exec_sym)
    {
        LOGW("execute_ex not found via dlsym — libphp.so may be missing expected symbols");
    }

    /* Do NOT dlclose — keep RTLD_GLOBAL flag alive */
    return JNI_VERSION_1_6;
}