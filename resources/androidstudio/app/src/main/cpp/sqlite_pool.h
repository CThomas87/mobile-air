/**
 * sqlite_pool.h — Lightweight SQLite connection pool for worker threads
 *
 * Provides a fixed-size pool of pre-opened SQLite WAL connections that
 * worker threads borrow/return instead of each thread opening its own.
 * This reduces file handle overhead and ensures all connections share
 * the same WAL mode and PRAGMA configuration.
 *
 * Thread-safe: all operations are protected by a pthread mutex.
 *
 * Usage:
 *   sqlite_pool_t *pool = sqlite_pool_create("/data/.../database.sqlite", 4);
 *   sqlite3 *db = sqlite_pool_acquire(pool, 5000);
 *   // ... use db ...
 *   sqlite_pool_release(pool, db);
 *   sqlite_pool_destroy(pool);
 */
#ifndef NATIVEPHP_SQLITE_POOL_H
#define NATIVEPHP_SQLITE_POOL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

    /* Forward declaration — avoids requiring sqlite3.h in every consumer */
    typedef struct sqlite3 sqlite3;

    /* Opaque pool handle */
    typedef struct sqlite_pool sqlite_pool_t;

    /**
     * Create a connection pool.
     *
     * Opens `pool_size` SQLite connections to the given database path, each
     * configured with:
     *   PRAGMA journal_mode=WAL
     *   PRAGMA busy_timeout=5000
     *   PRAGMA synchronous=NORMAL
     *   PRAGMA cache_size=-4000  (4MB per connection)
     *
     * @param db_path     Absolute path to the SQLite database file
     * @param pool_size   Number of connections to pre-open (1-16)
     * @return Pool handle, or NULL on failure
     */
    sqlite_pool_t *sqlite_pool_create(const char *db_path, int pool_size);

    /**
     * Acquire a connection from the pool.
     * Blocks until a connection is available or timeout expires.
     *
     * @param pool        The connection pool
     * @param timeout_ms  Maximum wait time in milliseconds (0 = wait forever)
     * @return A sqlite3 handle, or NULL on timeout/error
     */
    sqlite3 *sqlite_pool_acquire(sqlite_pool_t *pool, uint32_t timeout_ms);

    /**
     * Release a connection back to the pool.
     *
     * @param pool  The connection pool
     * @param db    The connection handle (must have been acquired from this pool)
     */
    void sqlite_pool_release(sqlite_pool_t *pool, sqlite3 *db);

    /**
     * Destroy the pool and close all connections.
     * All connections must be released before calling this.
     */
    void sqlite_pool_destroy(sqlite_pool_t *pool);

    /**
     * Get the number of currently available (idle) connections.
     */
    int sqlite_pool_available_count(const sqlite_pool_t *pool);

    /**
     * Get the total pool size.
     */
    int sqlite_pool_total_count(const sqlite_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* NATIVEPHP_SQLITE_POOL_H */
