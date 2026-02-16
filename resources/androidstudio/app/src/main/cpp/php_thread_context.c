/**
 * php_thread_context.c — Per-thread TSRM context management
 *
 * Under ZTS, each thread that wants to run PHP must have its own
 * interpreter context registered with TSRM. This file manages
 * the creation and destruction of those contexts.
 */
#include "php_thread_context.h"
#include "php_engine.h"
#include "zts_guard.h"

/* PHP headers */
#include "php_embed.h"
#include "TSRM.h"
#include "zend.h"
#include <pthread.h>
#include <stdio.h>

#ifdef __ANDROID__
#include <android/log.h>
#define TC_TAG "PhpThreadCtx"
#define TC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, TC_TAG, __VA_ARGS__)
#define TC_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TC_TAG, __VA_ARGS__)
#else
#define TC_LOGI(...) do { fprintf(stdout, "[PhpThreadCtx] "); fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while(0)
#define TC_LOGE(...) do { fprintf(stderr, "[PhpThreadCtx] ERROR: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

/*
 * Define a local thread-local TSRM cache for this translation unit.
 * This avoids referencing the emulated-TLS descriptor (__emutls_v._tsrm_ls_cache)
 * from libphp.so, which may not be exported due to -fvisibility=hidden.
 * This is the standard pattern used by all PHP extensions under ZTS.
 */
#ifdef ZTS
TSRMLS_CACHE_DEFINE()
#endif

/* Thread-local flag to track attachment */
static __thread int tls_attached = 0;

int php_thread_attach(void)
{
    if (!php_engine_is_initialized()) {
        TC_LOGE("Cannot attach thread: engine not initialized");
        return -1;
    }

    if (tls_attached) {
        TC_LOGI("Thread already attached (tid=%lu)", (unsigned long)pthread_self());
        return 0;
    }

#ifdef ZTS
    TC_LOGI("Attaching thread (tid=%lu) to TSRM...", (unsigned long)pthread_self());

    /*
     * ts_resource(0) allocates (or retrieves) ZTS globals for the calling
     * thread and registers it with TSRM.  This is the modern PHP 8.x API
     * that replaced the removed tsrm_new/set_interpreter_context() calls.
     */
    void *new_ctx = ts_resource(0);
    if (!new_ctx) {
        TC_LOGE("ts_resource(0) failed");
        return -1;
    }
    /* Update the thread-local TSRM cache */
    TSRMLS_CACHE_UPDATE();

    tls_attached = 1;
    TC_LOGI("Thread attached successfully (tid=%lu)", (unsigned long)pthread_self());
    return 0;
#else
    TC_LOGE("FATAL: ZTS not enabled. Cannot attach thread.");
    return -1;
#endif
}

void php_thread_detach(void)
{
    if (!tls_attached) {
        TC_LOGI("Thread not attached, nothing to detach (tid=%lu)", (unsigned long)pthread_self());
        return;
    }

#ifdef ZTS
    TC_LOGI("Detaching thread (tid=%lu) from TSRM...", (unsigned long)pthread_self());

    /*
     * ts_free_thread() releases all ZTS globals allocated for the
     * calling thread and unregisters it from TSRM.
     */
    ts_free_thread();

    tls_attached = 0;
    TC_LOGI("Thread detached (tid=%lu)", (unsigned long)pthread_self());
#else
    TC_LOGE("FATAL: ZTS not enabled. Cannot detach thread.");
#endif
}

int php_thread_is_attached(void)
{
    return tls_attached;
}
