/**
 * worker_pool.h — Native thread pool for concurrent PHP job execution
 *
 * Manages N worker threads, each with its own TSRM context.
 * Jobs are submitted to a queue and executed one-per-request-cycle.
 */
#ifndef NATIVEPHP_WORKER_POOL_H
#define NATIVEPHP_WORKER_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "php_request_context.h"
#include <stdint.h>

/* Opaque pool handle */
typedef struct worker_pool worker_pool_t;

/**
 * Create a worker pool.
 *
 * @param num_workers  Number of worker threads (1-8)
 * @param max_queue    Maximum pending jobs in queue (0 = unlimited)
 * @return Pool handle, or NULL on failure
 */
worker_pool_t *worker_pool_create(int num_workers, int max_queue);

/**
 * Start the worker pool threads.
 * The engine must be initialized before calling this.
 *
 * @return 0 on success, -1 on failure
 */
int worker_pool_start(worker_pool_t *pool);

/**
 * Submit a job to the pool.
 * The pool takes ownership of the context.
 *
 * @param pool  The worker pool
 * @param ctx   The request context (ownership transferred)
 * @return 0 on success, -1 if queue is full or pool is stopped
 */
int worker_pool_submit(worker_pool_t *pool, php_request_context_t *ctx);

/**
 * Wait for a specific job to complete.
 *
 * @param pool       The worker pool
 * @param job_id     Job ID to wait for
 * @param timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return The result context (caller must destroy), or NULL on timeout/not-found
 */
php_request_context_t *worker_pool_await(worker_pool_t *pool,
                                          const char *job_id,
                                          uint32_t timeout_ms);

/**
 * Cancel a pending or running job.
 *
 * @param pool    The worker pool
 * @param job_id  Job ID to cancel
 * @return 0 on success (cancellation requested), -1 if not found
 */
int worker_pool_cancel(worker_pool_t *pool, const char *job_id);

/**
 * Stop the worker pool. Cancels pending jobs and waits for running jobs.
 * After this returns, no more jobs will be executed.
 */
void worker_pool_stop(worker_pool_t *pool);

/**
 * Destroy the pool and free all resources.
 * Must call worker_pool_stop first.
 */
void worker_pool_destroy(worker_pool_t *pool);

/**
 * Get the number of active (running) jobs.
 */
int worker_pool_active_count(const worker_pool_t *pool);

/**
 * Get the number of pending (queued) jobs.
 */
int worker_pool_pending_count(const worker_pool_t *pool);

/**
 * Wake all idle worker threads immediately.
 * Used for foreground immediate dispatch (<500ms latency).
 */
void worker_pool_wake(worker_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* NATIVEPHP_WORKER_POOL_H */
