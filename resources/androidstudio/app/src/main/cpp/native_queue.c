/**
 * native_queue.c — Native SQLite queue popper implementation
 *
 * Bypasses the full PHP bootstrap to check if queue jobs are available.
 * Uses SQLite C API directly for sub-millisecond queue introspection.
 */
#include "native_queue.h"

/*
 * SQLite3 header: see sqlite_pool.c for sourcing notes.
 * Guard: compiles as stubs if NATIVEPHP_HAS_SQLITE3 is not defined.
 */
#ifdef NATIVEPHP_HAS_SQLITE3
#include "sqlite_compat.h"
#else
/* Stub mode */
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
#define NQ_TAG "NativeQueue"
#define NQ_LOGI(...) __android_log_print(ANDROID_LOG_INFO, NQ_TAG, __VA_ARGS__)
#define NQ_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, NQ_TAG, __VA_ARGS__)
#else
#define NQ_LOGI(...)                       \
    do                                     \
    {                                      \
        fprintf(stdout, "[NativeQueue] "); \
        fprintf(stdout, __VA_ARGS__);      \
        fprintf(stdout, "\n");             \
    } while (0)
#define NQ_LOGE(...)                              \
    do                                            \
    {                                             \
        fprintf(stderr, "[NativeQueue] ERROR: "); \
        fprintf(stderr, __VA_ARGS__);             \
        fprintf(stderr, "\n");                    \
    } while (0)
#endif

/* ─── Helper: get current epoch seconds ─── */
static int64_t current_epoch(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

/* ─── Helper: open a read-only SQLite connection ─── */
static sqlite3 *open_readonly(const char *db_path, char *error_buf, size_t error_size)
{
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(
        db_path,
        &db,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
        NULL);

    if (rc != SQLITE_OK)
    {
        snprintf(error_buf, error_size, "sqlite3_open failed: %s",
                 db ? sqlite3_errmsg(db) : "unknown");
        if (db)
            sqlite3_close(db);
        return NULL;
    }

    /* Set a short busy timeout for reads */
    sqlite3_exec(db, "PRAGMA busy_timeout=1000", NULL, NULL, NULL);

    return db;
}

/* ─── Public API ─── */

native_queue_peek_result_t native_queue_peek(const char *db_path,
                                             const char *queue_name)
{
    native_queue_peek_result_t result;
    memset(&result, 0, sizeof(result));

    if (!db_path)
    {
        snprintf(result.error, sizeof(result.error), "db_path is NULL");
        return result;
    }

    sqlite3 *db = open_readonly(db_path, result.error, sizeof(result.error));
    if (!db)
        return result;

    int64_t now = current_epoch();
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (queue_name && queue_name[0] != '\0')
    {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT COUNT(*), MIN(available_at) FROM jobs "
            "WHERE reserved_at IS NULL AND available_at <= ? AND queue = ?",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, now);
            sqlite3_bind_text(stmt, 2, queue_name, -1, SQLITE_STATIC);
        }
    }
    else
    {
        rc = sqlite3_prepare_v2(
            db,
            "SELECT COUNT(*), MIN(available_at) FROM jobs "
            "WHERE reserved_at IS NULL AND available_at <= ?",
            -1, &stmt, NULL);
        if (rc == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, now);
        }
    }

    if (rc != SQLITE_OK)
    {
        snprintf(result.error, sizeof(result.error), "prepare failed: %s",
                 sqlite3_errmsg(db));
        sqlite3_close(db);
        return result;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        result.job_count = sqlite3_column_int(stmt, 0);
        result.oldest_job_at = sqlite3_column_int64(stmt, 1);
        result.has_jobs = (result.job_count > 0) ? 1 : 0;
    }
    else
    {
        snprintf(result.error, sizeof(result.error), "step failed: %s",
                 sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

native_queue_peek_result_t native_queue_peek_multi(const char *db_path,
                                                   const char *queue_csv)
{
    native_queue_peek_result_t result;
    memset(&result, 0, sizeof(result));

    if (!db_path)
    {
        snprintf(result.error, sizeof(result.error), "db_path is NULL");
        return result;
    }

    if (!queue_csv || queue_csv[0] == '\0')
    {
        return native_queue_peek(db_path, NULL);
    }

    sqlite3 *db = open_readonly(db_path, result.error, sizeof(result.error));
    if (!db)
        return result;

    int64_t now = current_epoch();

    /* Build query with IN clause using parameter binding.
     * Parse the CSV and bind each queue name individually. */
    char csv_copy[512];
    strncpy(csv_copy, queue_csv, sizeof(csv_copy) - 1);
    csv_copy[sizeof(csv_copy) - 1] = '\0';

    /* Count queues */
    int queue_count = 1;
    for (const char *p = csv_copy; *p; p++)
    {
        if (*p == ',')
            queue_count++;
    }
    if (queue_count > 32)
        queue_count = 32; /* sanity cap */

    /* Build "?,?,?" placeholder string */
    char placeholders[256] = {0};
    char *pp = placeholders;
    for (int i = 0; i < queue_count; i++)
    {
        if (i > 0)
            *pp++ = ',';
        *pp++ = '?';
    }
    *pp = '\0';

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*), MIN(available_at) FROM jobs "
             "WHERE reserved_at IS NULL AND available_at <= ? AND queue IN (%s)",
             placeholders);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        snprintf(result.error, sizeof(result.error), "prepare failed: %s",
                 sqlite3_errmsg(db));
        sqlite3_close(db);
        return result;
    }

    sqlite3_bind_int64(stmt, 1, now);

    /* Bind queue names from CSV */
    char *token = strtok(csv_copy, ",");
    int bind_idx = 2;
    while (token && bind_idx <= queue_count + 1)
    {
        /* Trim whitespace */
        while (*token == ' ')
            token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ')
            *end-- = '\0';

        sqlite3_bind_text(stmt, bind_idx, token, -1, SQLITE_TRANSIENT);
        bind_idx++;
        token = strtok(NULL, ",");
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        result.job_count = sqlite3_column_int(stmt, 0);
        result.oldest_job_at = sqlite3_column_int64(stmt, 1);
        result.has_jobs = (result.job_count > 0) ? 1 : 0;
    }
    else
    {
        snprintf(result.error, sizeof(result.error), "step failed: %s",
                 sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}

char *native_queue_status_json(const char *db_path)
{
    if (!db_path)
        return strdup("{\"error\":\"db_path is NULL\"}");

    char error_buf[256] = {0};
    sqlite3 *db = open_readonly(db_path, error_buf, sizeof(error_buf));
    if (!db)
    {
        char *json = (char *)malloc(512);
        if (json)
            snprintf(json, 512, "{\"error\":\"%s\"}", error_buf);
        return json;
    }

    /* Get per-queue counts */
    sqlite3_stmt *stmt = NULL;
    int64_t now = current_epoch();

    int rc = sqlite3_prepare_v2(
        db,
        "SELECT queue, COUNT(*) FROM jobs "
        "WHERE reserved_at IS NULL AND available_at <= ? "
        "GROUP BY queue ORDER BY queue",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK)
    {
        sqlite3_close(db);
        return strdup("{\"error\":\"prepare failed\"}");
    }

    sqlite3_bind_int64(stmt, 1, now);

    /* Build JSON response */
    char *json = (char *)malloc(2048);
    if (!json)
    {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return NULL;
    }

    char *p = json;
    int remaining = 2048;
    int total = 0;

    p += snprintf(p, remaining, "{\"queues\":{");
    remaining = 2048 - (int)(p - json);

    int first = 1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const char *queue = (const char *)sqlite3_column_text(stmt, 0);
        int count = sqlite3_column_int(stmt, 1);
        total += count;

        if (!first)
        {
            p += snprintf(p, remaining, ",");
            remaining = 2048 - (int)(p - json);
        }
        p += snprintf(p, remaining, "\"%s\":%d", queue ? queue : "unknown", count);
        remaining = 2048 - (int)(p - json);
        first = 0;
    }

    snprintf(p, remaining, "},\"total\":%d}", total);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return json;
}
