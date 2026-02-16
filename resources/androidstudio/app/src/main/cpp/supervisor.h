/**
 * supervisor.h — Top-level orchestrator for PHP background work
 *
 * Owns:
 * - PhpEngine lifecycle
 * - WorkerPool for queue jobs
 * - SchedulerGate for exclusive scheduler ticks
 * - Job ID generation
 *
 * Exposes the unified API that JNI/Swift bridges call.
 */
#ifndef NATIVEPHP_SUPERVISOR_H
#define NATIVEPHP_SUPERVISOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Supervisor mode */
typedef enum {
    SUPERVISOR_MODE_ALL       = 0,  /* queue workers + scheduler */
    SUPERVISOR_MODE_QUEUE     = 1,  /* queue workers only */
    SUPERVISOR_MODE_SCHEDULER = 2   /* scheduler only */
} supervisor_mode_t;

/* Supervisor status */
typedef enum {
    SUPERVISOR_STOPPED  = 0,
    SUPERVISOR_STARTING = 1,
    SUPERVISOR_RUNNING  = 2,
    SUPERVISOR_STOPPING = 3
} supervisor_status_t;

/**
 * Initialize the PHP engine.
 *
 * @param ini_path       Path to php.ini (or NULL)
 * @param ini_overrides  Additional INI entries (or NULL)
 * @param app_base_path  Laravel app base path
 * @return 0 on success, -1 on failure
 */
int supervisor_engine_init(const char *ini_path,
                           const char *ini_overrides,
                           const char *app_base_path);

/**
 * Start the supervisor.
 *
 * @param mode          SUPERVISOR_MODE_ALL / QUEUE / SCHEDULER
 * @param worker_count  Number of queue worker threads (default 2)
 * @param queues        Comma-separated queue names (or NULL for "default")
 * @param connection    Queue connection name (or NULL for "database")
 * @return 0 on success, -1 on failure
 */
int supervisor_start(supervisor_mode_t mode,
                     int worker_count,
                     const char *queues,
                     const char *connection);

/**
 * Stop the supervisor.
 * Cancels pending work, waits for active jobs, shuts down workers.
 */
void supervisor_stop(void);

/**
 * Get supervisor status.
 */
supervisor_status_t supervisor_get_status(void);

/**
 * Enqueue a queue job.
 *
 * @param payload_json  JSON string with job payload (or NULL for auto-pop)
 * @return Job ID string (caller must free), or NULL on failure
 */
char *supervisor_enqueue_queue_job(const char *payload_json);

/**
 * Enqueue a scheduler tick.
 *
 * @param payload_json  JSON string with tick payload (or NULL)
 * @return Job ID string (caller must free), or NULL on failure (e.g., already running)
 */
char *supervisor_enqueue_scheduler_tick(const char *payload_json);

/**
 * Await a job result.
 *
 * @param job_id      Job ID to wait for
 * @param timeout_ms  Timeout in milliseconds (0 = wait forever)
 * @return JSON result string (caller must free), or NULL on timeout
 */
char *supervisor_await_job(const char *job_id, uint32_t timeout_ms);

/**
 * Cancel a job.
 *
 * @param job_id  Job ID to cancel
 * @return 0 on success, -1 on failure
 */
int supervisor_cancel_job(const char *job_id);

/**
 * Notify supervisor that a job has completed execution.
 * Used by the worker pool to release scheduler gate even when no awaiter exists.
 */
void supervisor_notify_job_completed(const char *job_id);

/**
 * Notify supervisor that a job has failed.
 * Increments failure counter and releases scheduler gate if needed.
 */
void supervisor_notify_job_failed(const char *job_id, const char *error);

/**
 * Get supervisor status as JSON.
 * Format: { "status": "running", "activeJobs": N, "pendingJobs": N,
 *           "completedJobs": N, "failedJobs": N, "schedulerRunning": bool,
 *           "uptimeSeconds": N, "mode": N }
 *
 * @return JSON string (caller must free)
 */
char *supervisor_status_json(void);

/**
 * Configure the circuit breaker for crash recovery.
 *
 * @param max_consecutive_crashes  Pause thread after this many crashes (default 3)
 * @param base_backoff_seconds     Initial backoff (doubles per crash, default 5)
 */
void supervisor_configure_circuit_breaker(int max_consecutive_crashes,
                                          int base_backoff_seconds);

/**
 * Set the memory_limit INI override for worker PHP requests.
 *
 * @param memory_limit  PHP memory_limit string (e.g., "64M")
 */
void supervisor_set_memory_limit(const char *memory_limit);

/**
 * Enable/disable worker logging to a file.
 *
 * @param log_path     Path to the worker log file
 * @param max_size_kb  Maximum log file size in KB (ring buffer)
 */
void supervisor_set_log_file(const char *log_path, int max_size_kb);

/**
 * Notify supervisor that a job should be dispatched immediately.
 * Used for foreground immediate dispatch (<500ms latency).
 */
void supervisor_wake_workers(void);

/**
 * Mark whether a foreground UI request is currently active.
 * When active, worker threads should yield to avoid impacting first paint.
 */
void supervisor_set_ui_request_active(int active);

/**
 * Query whether a foreground UI request is active.
 */
int supervisor_is_ui_request_active(void);

/**
 * Shutdown the PHP engine.
 * Call after supervisor_stop and once all jobs are done.
 */
void supervisor_engine_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NATIVEPHP_SUPERVISOR_H */
