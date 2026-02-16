/**
 * supervisor.c — Top-level orchestrator implementation
 */
#include "supervisor.h"
#include "php_engine.h"
#include "php_thread_context.h"
#include "php_request_context.h"
#include "worker_pool.h"
#include "scheduler_gate.h"
#include "zts_guard.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
#define SV_TAG "Supervisor"
#define SV_LOGI(...) __android_log_print(ANDROID_LOG_INFO, SV_TAG, __VA_ARGS__)
#define SV_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SV_TAG, __VA_ARGS__)
#else
#define SV_LOGI(...)                      \
    do                                    \
    {                                     \
        fprintf(stdout, "[Supervisor] "); \
        fprintf(stdout, __VA_ARGS__);     \
        fprintf(stdout, "\n");            \
    } while (0)
#define SV_LOGE(...)                             \
    do                                           \
    {                                            \
        fprintf(stderr, "[Supervisor] ERROR: "); \
        fprintf(stderr, __VA_ARGS__);            \
        fprintf(stderr, "\n");                   \
    } while (0)
#endif

/* ─── Supervisor state (process singleton) ─── */
static pthread_mutex_t s_sv_mutex = PTHREAD_MUTEX_INITIALIZER;
static supervisor_status_t s_status = SUPERVISOR_STOPPED;
static supervisor_mode_t s_mode = SUPERVISOR_MODE_ALL;
static worker_pool_t *s_pool = NULL;
static scheduler_gate_t *s_sched_gate = NULL;
static atomic_uint s_job_counter = 0;
static int64_t s_started_at = 0;
static atomic_uint s_completed_jobs = 0;
static atomic_uint s_failed_jobs = 0;
static atomic_int s_ui_request_active = 0;

/* Configuration saved at start */
static char s_queues[512] = "default";
static char s_connection[128] = "database";
static char s_queue_script[2048] = {0};
static char s_sched_script[2048] = {0};

/* Circuit breaker configuration */
static int s_cb_max_crashes = 3;
static int s_cb_base_backoff_sec = 5;

/* Memory limit for worker PHP requests */
static char s_memory_limit[32] = "256M";

/* Worker log file */
static char s_log_path[2048] = {0};
static int s_log_max_size_kb = 1024;
static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ─── Structured worker log ─── */
static void sv_log_event(const char *event, const char *job_id, const char *detail)
{
    if (s_log_path[0] == '\0')
        return;

    pthread_mutex_lock(&s_log_mutex);

    FILE *f = fopen(s_log_path, "a");
    if (f)
    {
        /* Check size and truncate if over limit (ring buffer) */
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        if (size > s_log_max_size_kb * 1024L)
        {
            fclose(f);
            f = fopen(s_log_path, "w"); /* Truncate */
        }
        if (f)
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            fprintf(f, "{\"ts\":%lld,\"event\":\"%s\",\"job_id\":\"%s\",\"detail\":\"%s\"}\n",
                    (long long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000),
                    event ? event : "",
                    job_id ? job_id : "",
                    detail ? detail : "");
            fclose(f);
        }
    }

    pthread_mutex_unlock(&s_log_mutex);
}

/* ─── Helper: generate job ID ─── */
static char *generate_job_id(const char *prefix)
{
    unsigned int seq = atomic_fetch_add(&s_job_counter, 1);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s-%u-%ld", prefix, seq, (long)time(NULL));
    return strdup(buf);
}

/* ─── Public API ─── */

int supervisor_engine_init(const char *ini_path,
                           const char *ini_overrides,
                           const char *app_base_path)
{
    SV_LOGI("Initializing engine (app_path=%s)", app_base_path ? app_base_path : "(null)");
    int rc = php_engine_init(ini_path, ini_overrides, app_base_path);
    if (rc == 0)
    {
        /* Build script paths */
        const char *app = php_engine_get_app_path();
        snprintf(s_queue_script, sizeof(s_queue_script),
                 "%s/vendor/nativephp/mobile/bootstrap/worker/queue_worker.php", app);
        snprintf(s_sched_script, sizeof(s_sched_script),
                 "%s/vendor/nativephp/mobile/bootstrap/worker/scheduler_tick.php", app);
        SV_LOGI("Queue script: %s", s_queue_script);
        SV_LOGI("Scheduler script: %s", s_sched_script);
    }
    return rc;
}

int supervisor_start(supervisor_mode_t mode,
                     int worker_count,
                     const char *queues,
                     const char *connection)
{
    int result = -1;

    pthread_mutex_lock(&s_sv_mutex);

    if (s_status != SUPERVISOR_STOPPED)
    {
        SV_LOGI("Supervisor already running/starting, ignoring start");
        result = 0;
        goto done;
    }

    if (!php_engine_is_initialized())
    {
        SV_LOGE("Cannot start: PHP engine not initialized");
        goto done;
    }

    s_status = SUPERVISOR_STARTING;
    s_mode = mode;

    /* Save configuration */
    if (queues)
        strncpy(s_queues, queues, sizeof(s_queues) - 1);
    if (connection)
        strncpy(s_connection, connection, sizeof(s_connection) - 1);

    /* Set environment for workers */
    setenv("NATIVEPHP_QUEUE_CONNECTION", s_connection, 1);
    setenv("NATIVEPHP_QUEUE_NAMES", s_queues, 1);

    if (worker_count < 1)
        worker_count = 2;
    if (worker_count > 8)
        worker_count = 8;

    if (!php_engine_verify_zts() && worker_count > 1)
    {
        SV_LOGE("ZTS runtime verification failed; degrading to safe single-worker mode");
        worker_count = 1;
    }

    SV_LOGI("Starting supervisor: mode=%d workers=%d queues=%s connection=%s",
            mode, worker_count, s_queues, s_connection);

    /* Create scheduler gate */
    s_sched_gate = scheduler_gate_create();
    if (!s_sched_gate)
    {
        SV_LOGE("Failed to create scheduler gate");
        s_status = SUPERVISOR_STOPPED;
        goto done;
    }

    /* Create worker pool */
    int pool_size = worker_count;
    /* Add 1 extra thread for scheduler if in ALL mode */
    if (mode == SUPERVISOR_MODE_ALL || mode == SUPERVISOR_MODE_SCHEDULER)
    {
        pool_size += 1;
    }

    s_pool = worker_pool_create(pool_size, 256);
    if (!s_pool)
    {
        SV_LOGE("Failed to create worker pool");
        scheduler_gate_destroy(s_sched_gate);
        s_sched_gate = NULL;
        s_status = SUPERVISOR_STOPPED;
        goto done;
    }

    if (worker_pool_start(s_pool) != 0)
    {
        SV_LOGE("Failed to start worker pool");

        if (pool_size > 1)
        {
            SV_LOGI("Retrying supervisor startup in single-worker safe mode");
            worker_pool_destroy(s_pool);
            s_pool = worker_pool_create(1, 256);
            if (s_pool && worker_pool_start(s_pool) == 0)
            {
                s_status = SUPERVISOR_RUNNING;
                result = 0;
                SV_LOGI("Supervisor started in degraded single-worker mode");
                goto done;
            }
            if (s_pool)
            {
                worker_pool_destroy(s_pool);
                s_pool = NULL;
            }
        }

        worker_pool_destroy(s_pool);
        s_pool = NULL;
        scheduler_gate_destroy(s_sched_gate);
        s_sched_gate = NULL;
        s_status = SUPERVISOR_STOPPED;
        goto done;
    }

    s_status = SUPERVISOR_RUNNING;
    result = 0;

    struct timespec now_ts;
    clock_gettime(CLOCK_REALTIME, &now_ts);
    s_started_at = (int64_t)now_ts.tv_sec;
    atomic_store(&s_completed_jobs, 0);
    atomic_store(&s_failed_jobs, 0);

    SV_LOGI("Supervisor started successfully");
    sv_log_event("supervisor_started", "", "");

done:
    pthread_mutex_unlock(&s_sv_mutex);
    return result;
}

void supervisor_stop(void)
{
    pthread_mutex_lock(&s_sv_mutex);

    if (s_status != SUPERVISOR_RUNNING && s_status != SUPERVISOR_STARTING)
    {
        pthread_mutex_unlock(&s_sv_mutex);
        return;
    }

    SV_LOGI("Stopping supervisor...");
    s_status = SUPERVISOR_STOPPING;
    pthread_mutex_unlock(&s_sv_mutex);

    /* Stop pool (this cancels pending, waits for active) */
    if (s_pool)
    {
        worker_pool_stop(s_pool);
        worker_pool_destroy(s_pool);
        s_pool = NULL;
    }

    if (s_sched_gate)
    {
        scheduler_gate_destroy(s_sched_gate);
        s_sched_gate = NULL;
    }

    /* Clean up environment */
    unsetenv("NATIVEPHP_QUEUE_CONNECTION");
    unsetenv("NATIVEPHP_QUEUE_NAMES");

    pthread_mutex_lock(&s_sv_mutex);
    s_status = SUPERVISOR_STOPPED;
    pthread_mutex_unlock(&s_sv_mutex);

    SV_LOGI("Supervisor stopped");
    sv_log_event("supervisor_stopped", "", "");
    s_started_at = 0;
}

supervisor_status_t supervisor_get_status(void)
{
    return s_status;
}

char *supervisor_enqueue_queue_job(const char *payload_json)
{
    if (s_status != SUPERVISOR_RUNNING)
    {
        SV_LOGE("Cannot enqueue: supervisor not running");
        return NULL;
    }

    if (s_mode == SUPERVISOR_MODE_SCHEDULER)
    {
        SV_LOGE("Cannot enqueue queue job: supervisor in scheduler-only mode");
        return NULL;
    }

    char *job_id = generate_job_id("queue");
    if (!job_id)
        return NULL;

    php_request_context_t *ctx = php_request_create(
        job_id, JOB_TYPE_QUEUE, s_queue_script, payload_json);

    if (!ctx)
    {
        free(job_id);
        return NULL;
    }

    if (worker_pool_submit(s_pool, ctx) != 0)
    {
        php_request_destroy(ctx);
        free(job_id);
        return NULL;
    }

    SV_LOGI("Queue job enqueued: %s", job_id);
    sv_log_event("job_enqueued", job_id, "queue");
    return job_id;
}

char *supervisor_enqueue_scheduler_tick(const char *payload_json)
{
    if (s_status != SUPERVISOR_RUNNING)
    {
        SV_LOGE("Cannot enqueue: supervisor not running");
        return NULL;
    }

    if (s_mode == SUPERVISOR_MODE_QUEUE)
    {
        SV_LOGE("Cannot enqueue scheduler tick: supervisor in queue-only mode");
        return NULL;
    }

    /* Scheduler gate: only one tick at a time */
    if (!scheduler_gate_try_acquire(s_sched_gate))
    {
        SV_LOGI("Scheduler tick already running, skipping");
        return NULL;
    }

    char *job_id = generate_job_id("sched");
    if (!job_id)
    {
        scheduler_gate_release(s_sched_gate);
        return NULL;
    }

    php_request_context_t *ctx = php_request_create(
        job_id, JOB_TYPE_SCHEDULER, s_sched_script, payload_json);

    if (!ctx)
    {
        scheduler_gate_release(s_sched_gate);
        free(job_id);
        return NULL;
    }

    if (worker_pool_submit(s_pool, ctx) != 0)
    {
        php_request_destroy(ctx);
        scheduler_gate_release(s_sched_gate);
        free(job_id);
        return NULL;
    }

    /*
     * Note: The scheduler gate is released when the job completes.
     * The job's completion callback or the await caller is responsible
     * for calling scheduler_gate_release. We handle this in await.
     */

    SV_LOGI("Scheduler tick enqueued: %s", job_id);
    sv_log_event("tick_enqueued", job_id, "scheduler");
    return job_id;
}

char *supervisor_await_job(const char *job_id, uint32_t timeout_ms)
{
    if (!s_pool || !job_id)
        return NULL;

    php_request_context_t *ctx = worker_pool_await(s_pool, job_id, timeout_ms);
    if (!ctx)
    {
        return strdup("{\"ok\":false,\"error\":\"timeout or not found\",\"exitCode\":-1,"
                      "\"stdout\":\"\",\"stderr\":\"\",\"startedAt\":0,\"endedAt\":0}");
    }

    /*
     * NOTE: scheduler gate is released by supervisor_notify_job_completed()
     * which is called by the worker pool on job completion. No need to
     * release here — doing so would double-release the gate.
     */

    char *json = php_request_to_json(ctx);
    php_request_destroy(ctx);
    return json;
}

int supervisor_cancel_job(const char *job_id)
{
    if (!s_pool || !job_id)
        return -1;

    int rc = worker_pool_cancel(s_pool, job_id);

    /*
     * NOTE: for running scheduler jobs, the gate is released by
     * supervisor_notify_job_completed() on completion. For pending
     * scheduler jobs that are cancelled before execution, the gate
     * was acquired but will never trigger notify_job_completed,
     * so we must release it here.
     */
    if (rc == 0 && strncmp(job_id, "sched-", 6) == 0 && s_sched_gate)
    {
        /* Only release if the job was pending (not yet executed) */
        /* The cancel moves it to completed with CANCELLED status */
        scheduler_gate_release(s_sched_gate);
    }

    return rc;
}

void supervisor_notify_job_completed(const char *job_id)
{
    if (!job_id)
        return;

    atomic_fetch_add(&s_completed_jobs, 1);
    sv_log_event("job_completed", job_id, "");

    if (strncmp(job_id, "sched-", 6) == 0 && s_sched_gate)
    {
        scheduler_gate_release(s_sched_gate);
    }
}

void supervisor_notify_job_failed(const char *job_id, const char *error)
{
    if (!job_id)
        return;

    atomic_fetch_add(&s_failed_jobs, 1);
    sv_log_event("job_failed", job_id, error ? error : "unknown");

    if (strncmp(job_id, "sched-", 6) == 0 && s_sched_gate)
    {
        scheduler_gate_release(s_sched_gate);
    }
}

char *supervisor_status_json(void)
{
    const char *status_str;
    switch (s_status)
    {
    case SUPERVISOR_STOPPED:
        status_str = "stopped";
        break;
    case SUPERVISOR_STARTING:
        status_str = "starting";
        break;
    case SUPERVISOR_RUNNING:
        status_str = "running";
        break;
    case SUPERVISOR_STOPPING:
        status_str = "stopping";
        break;
    default:
        status_str = "unknown";
        break;
    }

    int active = s_pool ? worker_pool_active_count(s_pool) : 0;
    int pending = s_pool ? worker_pool_pending_count(s_pool) : 0;
    int sched_running = s_sched_gate ? scheduler_gate_is_running(s_sched_gate) : 0;

    char *json = (char *)malloc(512);
    if (!json)
        return strdup("{\"status\":\"error\"}");

    struct timespec now_ts;
    clock_gettime(CLOCK_REALTIME, &now_ts);
    int64_t uptime = (s_started_at > 0) ? ((int64_t)now_ts.tv_sec - s_started_at) : 0;
    unsigned int completed = atomic_load(&s_completed_jobs);
    unsigned int failed = atomic_load(&s_failed_jobs);

    snprintf(json, 512,
             "{\"status\":\"%s\",\"activeJobs\":%d,\"pendingJobs\":%d,"
             "\"completedJobs\":%u,\"failedJobs\":%u,"
             "\"schedulerRunning\":%s,\"uptimeSeconds\":%lld,\"mode\":%d}",
             status_str, active, pending, completed, failed,
             sched_running ? "true" : "false",
             (long long)uptime,
             (int)s_mode);

    return json;
}

/* ─── Circuit breaker configuration ─── */
void supervisor_configure_circuit_breaker(int max_consecutive_crashes,
                                          int base_backoff_seconds)
{
    s_cb_max_crashes = (max_consecutive_crashes > 0) ? max_consecutive_crashes : 3;
    s_cb_base_backoff_sec = (base_backoff_seconds > 0) ? base_backoff_seconds : 5;
    SV_LOGI("Circuit breaker configured: max_crashes=%d, base_backoff=%ds",
            s_cb_max_crashes, s_cb_base_backoff_sec);
}

void supervisor_set_memory_limit(const char *memory_limit)
{
    if (memory_limit)
    {
        pthread_mutex_lock(&s_sv_mutex);
        strncpy(s_memory_limit, memory_limit, sizeof(s_memory_limit) - 1);
        s_memory_limit[sizeof(s_memory_limit) - 1] = '\0';
        pthread_mutex_unlock(&s_sv_mutex);
        SV_LOGI("Worker memory limit: %s", s_memory_limit);
    }
}

void supervisor_set_log_file(const char *log_path, int max_size_kb)
{
    if (log_path)
    {
        strncpy(s_log_path, log_path, sizeof(s_log_path) - 1);
        s_log_path[sizeof(s_log_path) - 1] = '\0';
        s_log_max_size_kb = (max_size_kb > 0) ? max_size_kb : 1024;
        SV_LOGI("Worker log: %s (max %d KB)", s_log_path, s_log_max_size_kb);
    }
}

void supervisor_wake_workers(void)
{
    if (s_pool)
    {
        worker_pool_wake(s_pool);
    }
}

void supervisor_set_ui_request_active(int active)
{
    /* Multi-lane: track count of concurrent UI requests, not just a flag.
     * Workers yield whenever ANY UI request is in flight. */
    if (active)
        atomic_fetch_add(&s_ui_request_active, 1);
    else
        atomic_fetch_sub(&s_ui_request_active, 1);
}

int supervisor_is_ui_request_active(void)
{
    return atomic_load(&s_ui_request_active) > 0;
}

int supervisor_get_circuit_breaker_max(void) { return s_cb_max_crashes; }
int supervisor_get_circuit_breaker_backoff(void) { return s_cb_base_backoff_sec; }
const char *supervisor_get_memory_limit(void)
{
    /* s_memory_limit is written under s_sv_mutex by supervisor_set_memory_limit.
     * A full mutex lock here would be safest but the value changes rarely.
     * Since s_memory_limit is a fixed-size char[32], reads that happen
     * concurrently with a write could see a partial update.
     * We lock for correctness — this is not a hot path. */
    static __thread char tls_mem_limit[32];
    pthread_mutex_lock(&s_sv_mutex);
    memcpy(tls_mem_limit, s_memory_limit, sizeof(tls_mem_limit));
    pthread_mutex_unlock(&s_sv_mutex);
    return tls_mem_limit;
}

void supervisor_engine_shutdown(void)
{
    /* Ensure supervisor is stopped first */
    if (s_status != SUPERVISOR_STOPPED)
    {
        supervisor_stop();
    }

    php_engine_shutdown();
    SV_LOGI("Engine shutdown complete");
}
