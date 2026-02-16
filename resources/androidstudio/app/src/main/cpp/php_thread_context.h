/**
 * php_thread_context.h — Per-thread TSRM context management
 *
 * Each worker thread must register with TSRM to get its own
 * interpreter context. This manages that lifecycle.
 */
#ifndef NATIVEPHP_PHP_THREAD_CONTEXT_H
#define NATIVEPHP_PHP_THREAD_CONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Attach the current thread to TSRM.
 * Must be called once per thread before any PHP execution.
 * Creates a new interpreter context with its own globals.
 *
 * @return 0 on success, -1 on failure
 */
int php_thread_attach(void);

/**
 * Detach the current thread from TSRM.
 * Must be called when the thread is about to exit.
 * Destroys the thread's interpreter context.
 */
void php_thread_detach(void);

/**
 * Check if the current thread is attached to TSRM.
 * @return 1 if attached, 0 if not
 */
int php_thread_is_attached(void);

#ifdef __cplusplus
}
#endif

#endif /* NATIVEPHP_PHP_THREAD_CONTEXT_H */
