/**
 * sqlite_pool.c — Lightweight SQLite connection pool implementation
 *
 * Pre-opens N connections with WAL mode and busy_timeout, then hands
 * them out to worker threads on demand.  When all connections are in
 * use, callers block on a condvar until one is returned.
 */
#include "sqlite_pool.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* SQLite symbol declarations for Android system SQLite linkage. */
#include "sqlite_compat.h"

#ifdef __ANDROID__
#include <android/log.h>
#define SP_TAG "SQLitePool"
#define SP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, SP_TAG, __VA_ARGS__)
#define SP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SP_TAG, __VA_ARGS__)
#else
#define SP_LOGI(...)                      \
    do                                    \
    {                                     \
        fprintf(stdout, "[SQLitePool] "); \
        fprintf(stdout, __VA_ARGS__);     \
        fprintf(stdout, "\n");            \
    } while (0)
#define SP_LOGE(...)                             \
    do                                           \
    {                                            \
        fprintf(stderr, "[SQLitePool] ERROR: "); \
        fprintf(stderr, __VA_ARGS__);            \
        fprintf(stderr, "\n");                   \
    } while (0)
#endif

#define MAX_POOL_SIZE 16

/* ─── Connection slot ─── */
typedef struct
{
    sqlite3 *db;
    int in_use; /* 1 = checked out, 0 = available */
} pool_slot_t;

/* ─── Pool structure ─── */
struct sqlite_pool
{
    pool_slot_t slots[MAX_POOL_SIZE];
    int pool_size;
    pthread_mutex_t mutex;
    pthread_cond_t available_cond; /* signalled when a slot is released */
    char db_path[2048];
};

/* ─── Configure a freshly opened connection ─── */
static int configure_connection(sqlite3 *db)
{
    char *errmsg = NULL;
    int rc;

    rc = sqlite3_exec(db, "PRAGMA busy_timeout=5000", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK)
    {
        SP_LOGE("PRAGMA busy_timeout failed: %s", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return -1;
    }

    rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK)
    {
        SP_LOGE("PRAGMA journal_mode=WAL failed: %s", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        /* Non-fatal: WAL may already be set */
    }

    rc = sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK)
    {
        sqlite3_free(errmsg);
    }

    rc = sqlite3_exec(db, "PRAGMA cache_size=-4000", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK)
    {
        sqlite3_free(errmsg);
    }

    return 0;
}

/* ─── Public API ─── */

sqlite_pool_t *sqlite_pool_create(const char *db_path, int pool_size)
{
    if (!db_path || pool_size < 1)
        return NULL;
    if (pool_size > MAX_POOL_SIZE)
        pool_size = MAX_POOL_SIZE;

    sqlite_pool_t *pool = (sqlite_pool_t *)calloc(1, sizeof(sqlite_pool_t));
    if (!pool)
        return NULL;

    strncpy(pool->db_path, db_path, sizeof(pool->db_path) - 1);
    pool->pool_size = pool_size;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->available_cond, NULL);

    /* Pre-open all connections */
    for (int i = 0; i < pool_size; i++)
    {
        int rc = sqlite3_open_v2(
            db_path,
            &pool->slots[i].db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
            NULL);

        if (rc != SQLITE_OK)
        {
            SP_LOGE("Failed to open connection %d: %s",
                    i, pool->slots[i].db ? sqlite3_errmsg(pool->slots[i].db) : "unknown");
            /* Close any already-opened connections */
            for (int j = 0; j < i; j++)
            {
                sqlite3_close(pool->slots[j].db);
            }
            pthread_mutex_destroy(&pool->mutex);
            pthread_cond_destroy(&pool->available_cond);
            free(pool);
            return NULL;
        }

        if (configure_connection(pool->slots[i].db) != 0)
        {
            SP_LOGE("Failed to configure connection %d", i);
        }

        pool->slots[i].in_use = 0;
    }

    SP_LOGI("Pool created: %d connections to %s", pool_size, db_path);
    return pool;
}

sqlite3 *sqlite_pool_acquire(sqlite_pool_t *pool, uint32_t timeout_ms)
{
    if (!pool)
        return NULL;

    struct timespec deadline;
    if (timeout_ms > 0)
    {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000)
        {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }

    pthread_mutex_lock(&pool->mutex);

    while (1)
    {
        /* Search for an available slot */
        for (int i = 0; i < pool->pool_size; i++)
        {
            if (!pool->slots[i].in_use)
            {
                pool->slots[i].in_use = 1;
                sqlite3 *db = pool->slots[i].db;
                pthread_mutex_unlock(&pool->mutex);
                return db;
            }
        }

        /* All in use — wait */
        int rc;
        if (timeout_ms > 0)
        {
            rc = pthread_cond_timedwait(&pool->available_cond, &pool->mutex, &deadline);
            if (rc != 0)
            {
                /* Timeout */
                pthread_mutex_unlock(&pool->mutex);
                SP_LOGE("Acquire timed out after %u ms", timeout_ms);
                return NULL;
            }
        }
        else
        {
            pthread_cond_wait(&pool->available_cond, &pool->mutex);
        }
    }
}

void sqlite_pool_release(sqlite_pool_t *pool, sqlite3 *db)
{
    if (!pool || !db)
        return;

    pthread_mutex_lock(&pool->mutex);

    for (int i = 0; i < pool->pool_size; i++)
    {
        if (pool->slots[i].db == db)
        {
            pool->slots[i].in_use = 0;
            pthread_cond_signal(&pool->available_cond);
            pthread_mutex_unlock(&pool->mutex);
            return;
        }
    }

    pthread_mutex_unlock(&pool->mutex);
    SP_LOGE("Released connection not found in pool!");
}

void sqlite_pool_destroy(sqlite_pool_t *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->mutex);

    for (int i = 0; i < pool->pool_size; i++)
    {
        if (pool->slots[i].in_use)
        {
            SP_LOGE("Warning: closing connection %d that is still in use", i);
        }
        if (pool->slots[i].db)
        {
            sqlite3_close(pool->slots[i].db);
            pool->slots[i].db = NULL;
        }
    }

    pthread_mutex_unlock(&pool->mutex);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->available_cond);

    SP_LOGI("Pool destroyed");
    free(pool);
}

int sqlite_pool_available_count(const sqlite_pool_t *pool)
{
    if (!pool)
        return 0;

    int count = 0;
    pthread_mutex_lock(&((sqlite_pool_t *)pool)->mutex);
    for (int i = 0; i < pool->pool_size; i++)
    {
        if (!pool->slots[i].in_use)
            count++;
    }
    pthread_mutex_unlock(&((sqlite_pool_t *)pool)->mutex);
    return count;
}

int sqlite_pool_total_count(const sqlite_pool_t *pool)
{
    return pool ? pool->pool_size : 0;
}
