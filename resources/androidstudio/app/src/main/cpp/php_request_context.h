/**
 * php_request_context.h — Per-job/per-request isolated PHP execution context
 *
 * Each queue job or scheduler tick gets its own PhpRequestContext with:
 * - Isolated stdout/stderr buffers (NOT global)
 * - Status tracking (pending, running, completed, failed, cancelled)
 * - Timing (startedAt, endedAt)
 * - Cancellation flag (atomic)
 * - Exit code
 *
 * The ub_write callback routes output to the correct per-thread buffer
 * using thread-local storage.
 */
#ifndef NATIVEPHP_PHP_REQUEST_CONTEXT_H
#define NATIVEPHP_PHP_REQUEST_CONTEXT_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include <stddef.h>

    /* Forward-declare Zend types used in function signatures below.
     * The full definition lives in Zend/zend_compile.h, which we don't
     * want to pull into every translation unit that includes this header. */
    struct _zend_execute_data;
    typedef struct _zend_execute_data zend_execute_data;

    /* Job status enum */
    typedef enum
    {
        JOB_STATUS_PENDING = 0,
        JOB_STATUS_RUNNING = 1,
        JOB_STATUS_COMPLETED = 2,
        JOB_STATUS_FAILED = 3,
        JOB_STATUS_CANCELLED = 4
    } job_status_t;

    /* Job type enum */
    typedef enum
    {
        JOB_TYPE_QUEUE = 0,
        JOB_TYPE_SCHEDULER = 1,
        JOB_TYPE_HTTP = 2
    } job_type_t;

    /* Opaque handle */
    typedef struct php_request_context php_request_context_t;

    /**
     * Create a new request context for a job.
     *
     * @param job_id     Unique job identifier string
     * @param job_type   JOB_TYPE_QUEUE or JOB_TYPE_SCHEDULER
     * @param script_path  Path to the PHP script to execute
     * @param payload_json JSON payload for the job (or NULL)
     * @return New context, or NULL on failure. Caller must free with php_request_destroy.
     */
    php_request_context_t *php_request_create(const char *job_id,
                                              job_type_t job_type,
                                              const char *script_path,
                                              const char *payload_json);

    /**
     * Destroy a request context and free all resources.
     */
    void php_request_destroy(php_request_context_t *ctx);

    /**
     * Execute the PHP script in this request context.
     * Must be called from a thread that has been attached via php_thread_attach().
     *
     * Performs: php_request_startup → set env → execute script → php_request_shutdown
     *
     * @param ctx The request context
     * @return 0 on success, -1 on failure
     */
    int php_request_execute(php_request_context_t *ctx);

    /**
     * Store per-request HTTP info on the context (thread-safe).
     * Must be called BEFORE php_request_execute() for HTTP jobs.
     *
     * Values are copied (strdup'd). Pass NULL for any field to use defaults.
     * headers_raw: newline-delimited "KEY\nVALUE\nKEY\nVALUE\n..." pairs
     *              injected into $_SERVER after request startup.
     */
    void php_request_set_http_info(php_request_context_t *ctx,
                                   const char *method,
                                   const char *uri,
                                   const char *query_string,
                                   const char *headers_raw);

    /**
     * Request cancellation of a running job.
     * Sets the cancellation flag; the job will check periodically.
     */
    void php_request_cancel(php_request_context_t *ctx);

    /**
     * Check if cancellation has been requested.
     */
    int php_request_is_cancelled(php_request_context_t *ctx);

    /* ─── Accessors ─── */

    const char *php_request_get_job_id(const php_request_context_t *ctx);
    job_status_t php_request_get_status(const php_request_context_t *ctx);
    int php_request_get_exit_code(const php_request_context_t *ctx);
    const char *php_request_get_stdout(const php_request_context_t *ctx);
    const char *php_request_get_stderr(const php_request_context_t *ctx);
    const char *php_request_get_error(const php_request_context_t *ctx);
    int64_t php_request_get_started_at(const php_request_context_t *ctx);
    int64_t php_request_get_ended_at(const php_request_context_t *ctx);
    job_type_t php_request_get_job_type(const php_request_context_t *ctx);

    /**
     * Build a JSON result string from the context.
     * Caller must free() the returned string.
     *
     * Format: { "ok": bool, "stdout": "...", "stderr": "...",
     *           "exitCode": int, "error": "...|null",
     *           "startedAt": epoch_ms, "endedAt": epoch_ms }
     */
    char *php_request_to_json(const php_request_context_t *ctx);

    /**
     * Set the current thread's active request context.
     * The ub_write callback uses this to route output to the right buffer.
     */
    void php_request_set_current(php_request_context_t *ctx);

    /**
     * Get the current thread's active request context.
     */
    php_request_context_t *php_request_get_current(void);

    /**
     * Custom ub_write that routes output to the current thread's request context.
     * Install this as `php_embed_module.ub_write` during engine init.
     */
    size_t php_request_ub_write(const char *str, size_t str_length);

    /**
     * Cooperative cancellation interrupt handler.
     * Installed as zend_interrupt_function during PHP request execution.
     * When the cancelled flag is set, triggers zend_bailout to cleanly exit.
     */
    void php_request_interrupt_handler(zend_execute_data *execute_data);

    /**
     * Request cancellation with VM interrupt.
     * Sets the cancelled flag AND triggers the VM interrupt so PHP checks
     * as soon as possible (at the next opcode boundary).
     */
    void php_request_set_cancelled_interrupt(php_request_context_t *ctx);

    /**
     * Circuit breaker: crash count tracking per request context.
     * The worker pool uses this to decide when to back off.
     */
    int php_request_get_crash_count(const php_request_context_t *ctx);
    void php_request_increment_crash_count(php_request_context_t *ctx);

    /**
     * Job priority (higher = more urgent).
     * Default: 0 (normal). Range: -10 (low) to +10 (high).
     * Used by the worker pool for priority queue insertion.
     */
    void php_request_set_priority(php_request_context_t *ctx, int priority);
    int php_request_get_priority(const php_request_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NATIVEPHP_PHP_REQUEST_CONTEXT_H */
