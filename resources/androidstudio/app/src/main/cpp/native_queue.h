/**
 * native_queue.h — Native SQLite queue popper
 *
 * Provides lightweight, zero-PHP-overhead queue introspection directly
 * from C.  Instead of bootstrapping a full Laravel stack (~8-10s) just
 * to discover the queue is empty, this module queries the `jobs` table
 * via SQLite C API and returns immediately.
 *
 * If jobs ARE available, the caller proceeds with the full PHP
 * queue_worker.php execution.  If empty, the caller can skip the
 * expensive bootstrap entirely.
 *
 * This reduces idle-poll CPU and memory usage from ~24MB/cycle to ~0.
 */
#ifndef NATIVEPHP_NATIVE_QUEUE_H
#define NATIVEPHP_NATIVE_QUEUE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

    /* Result of a native queue peek */
    typedef struct
    {
        int has_jobs;          /* 1 if jobs are available, 0 if empty */
        int job_count;         /* number of available jobs */
        int64_t oldest_job_at; /* available_at of the oldest ready job (epoch sec), or 0 */
        char error[256];       /* error message, empty if OK */
    } native_queue_peek_result_t;

    /**
     * Peek at the queue to check if any jobs are available.
     *
     * Opens a read-only SQLite connection (or uses the pool if provided),
     * queries `SELECT COUNT(*) FROM jobs WHERE available_at <= ?` and
     * returns immediately.
     *
     * This is a pure C operation — no PHP engine or TSRM needed.
     *
     * @param db_path    Absolute path to the SQLite database file
     * @param queue_name Queue name to check (e.g., "default"), or NULL for all
     * @return Result struct (stack-allocated by caller)
     */
    native_queue_peek_result_t native_queue_peek(const char *db_path,
                                                 const char *queue_name);

    /**
     * Peek and return the count of jobs available across multiple queues.
     *
     * @param db_path     Absolute path to the SQLite database file
     * @param queue_csv   Comma-separated queue names (e.g., "high,default,low")
     * @return Result struct
     */
    native_queue_peek_result_t native_queue_peek_multi(const char *db_path,
                                                       const char *queue_csv);

    /**
     * Get a summary of queue state as a JSON string.
     * Caller must free() the returned string.
     *
     * @param db_path  Absolute path to the SQLite database file
     * @return JSON: {"total":N,"queues":{"default":N,"high":N,...}} or NULL on error
     */
    char *native_queue_status_json(const char *db_path);

#ifdef __cplusplus
}
#endif

#endif /* NATIVEPHP_NATIVE_QUEUE_H */
