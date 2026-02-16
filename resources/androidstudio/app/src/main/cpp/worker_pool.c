/**
 * worker_pool.c — Thread pool implementation for concurrent PHP jobs
 */
#include "worker_pool.h"
#include "php_thread_context.h"
#include "php_request_context.h"
#include "supervisor.h"
#include "zts_guard.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
#define WP_TAG "WorkerPool"
#define WP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, WP_TAG, __VA_ARGS__)
#define WP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, WP_TAG, __VA_ARGS__)
#else
#define WP_LOGI(...) do { fprintf(stdout, "[WorkerPool] "); fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while(0)
#define WP_LOGE(...) do { fprintf(stderr, "[WorkerPool] ERROR: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

/* Forward declarations for supervisor helpers */
extern int supervisor_get_circuit_breaker_max(void);
extern int supervisor_get_circuit_breaker_backoff(void);
extern void supervisor_notify_job_failed(const char *job_id, const char *error);
extern int supervisor_is_ui_request_active(void);

/* ─── Job queue node ─── */
typedef struct job_node {
    php_request_context_t *ctx;
    struct job_node       *next;
} job_node_t;

/* ─── Completed job entry (for await) ─── */
typedef struct completed_entry {
    php_request_context_t   *ctx;
    struct completed_entry  *next;
} completed_entry_t;

/* ─── Worker pool structure ─── */
struct worker_pool {
    /* Configuration */
    int num_workers;
    int max_queue;

    /* Worker threads */
    pthread_t *threads;

    /* Job queue (pending) */
    pthread_mutex_t queue_mutex;
    pthread_cond_t  queue_cond;
    job_node_t     *queue_head;
    job_node_t     *queue_tail;
    int             queue_count;

    /* Completed jobs (for await) */
    pthread_mutex_t  completed_mutex;
    pthread_cond_t   completed_cond;
    completed_entry_t *completed_head;

    /* Active jobs tracking */
    pthread_mutex_t active_mutex;
    php_request_context_t **active_jobs;  /* array of num_workers slots */
    atomic_int active_count;

    /* Pool state */
    atomic_int running;
    atomic_int stopped;
};

/* ─── Thread argument: passed via pthread_create to avoid ID race ─── */
typedef struct {
    worker_pool_t *pool;
    int             worker_id;
} worker_thread_arg_t;

/* ─── Worker thread function ─── */
static void *worker_thread_func(void *arg)
{
    worker_thread_arg_t *targ = (worker_thread_arg_t *)arg;
    worker_pool_t *pool = targ->pool;
    int worker_id = targ->worker_id;
    free(targ); /* No longer needed */

    WP_LOGI("Worker %d started (tid=%lu)", worker_id, (unsigned long)pthread_self());

    /* Circuit breaker: track consecutive crashes per thread */
    int consecutive_crashes = 0;

    /* Attach thread to TSRM */
    if (php_thread_attach() != 0) {
        WP_LOGE("Worker %d: Failed to attach to TSRM, exiting", worker_id);
        return NULL;
    }

    while (atomic_load(&pool->running)) {
        php_request_context_t *ctx = NULL;

        /* UI-lane priority: while foreground UI request is active,
         * workers yield briefly so UI request latency is not impacted. */
        if (supervisor_is_ui_request_active()) {
            pthread_mutex_lock(&pool->queue_mutex);
            struct timespec ui_ts;
            clock_gettime(CLOCK_REALTIME, &ui_ts);
            ui_ts.tv_nsec += 25 * 1000000; /* 25ms */
            if (ui_ts.tv_nsec >= 1000000000) {
                ui_ts.tv_sec += 1;
                ui_ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&pool->queue_cond, &pool->queue_mutex, &ui_ts);
            pthread_mutex_unlock(&pool->queue_mutex);
            continue;
        }

        /* Wait for a job */
        pthread_mutex_lock(&pool->queue_mutex);
        while (pool->queue_head == NULL && atomic_load(&pool->running)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1; /* Wake every 1s to check running flag */
            pthread_cond_timedwait(&pool->queue_cond, &pool->queue_mutex, &ts);
        }

        if (!atomic_load(&pool->running) && pool->queue_head == NULL) {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }

        /* Dequeue job */
        if (pool->queue_head) {
            job_node_t *node = pool->queue_head;
            pool->queue_head = node->next;
            if (pool->queue_head == NULL) {
                pool->queue_tail = NULL;
            }
            pool->queue_count--;
            ctx = node->ctx;
            free(node);
        }

        pthread_mutex_unlock(&pool->queue_mutex);

        if (!ctx) continue;

        /* Track as active */
        pthread_mutex_lock(&pool->active_mutex);
        if (worker_id >= 0 && worker_id < pool->num_workers) {
            pool->active_jobs[worker_id] = ctx;
        }
        atomic_fetch_add(&pool->active_count, 1);
        pthread_mutex_unlock(&pool->active_mutex);

        /* Execute the job */
        WP_LOGI("Worker %d: Executing job %s", worker_id, php_request_get_job_id(ctx));
        int exec_result = php_request_execute(ctx);
        job_status_t job_status = php_request_get_status(ctx);

        if (job_status == JOB_STATUS_FAILED || exec_result != 0) {
            consecutive_crashes++;
            supervisor_notify_job_failed(php_request_get_job_id(ctx),
                                         php_request_get_error(ctx));
            WP_LOGE("Worker %d: Job %s FAILED (crashes=%d)", worker_id,
                    php_request_get_job_id(ctx), consecutive_crashes);

            /* Circuit breaker: back off after consecutive crashes.
             * Uses condvar wait (not nanosleep) so we can be woken for shutdown. */
            int cb_max = supervisor_get_circuit_breaker_max();
            if (consecutive_crashes >= cb_max) {
                int cb_base = supervisor_get_circuit_breaker_backoff();
                int backoff_sec = cb_base * (1 << (consecutive_crashes - cb_max));
                if (backoff_sec > 300) backoff_sec = 300; /* Cap at 5 minutes */
                WP_LOGE("Worker %d: Circuit breaker tripped (%d consecutive crashes). "
                        "Backing off for %d seconds.", worker_id, consecutive_crashes, backoff_sec);

                /* Wait on queue_cond with timeout — allows immediate wakeup on shutdown */
                pthread_mutex_lock(&pool->queue_mutex);
                struct timespec cb_ts;
                clock_gettime(CLOCK_REALTIME, &cb_ts);
                cb_ts.tv_sec += backoff_sec;
                while (atomic_load(&pool->running) && pool->queue_head == NULL) {
                    int rc = pthread_cond_timedwait(&pool->queue_cond, &pool->queue_mutex, &cb_ts);
                    if (rc != 0) break; /* Timeout expired or error */
                }
                pthread_mutex_unlock(&pool->queue_mutex);

                if (!atomic_load(&pool->running)) break;
            }
        } else {
            consecutive_crashes = 0; /* Reset on success */
            supervisor_notify_job_completed(php_request_get_job_id(ctx));
        }

        WP_LOGI("Worker %d: Job %s completed (status=%d)", worker_id,
                php_request_get_job_id(ctx), job_status);

        /* Remove from active */
        pthread_mutex_lock(&pool->active_mutex);
        if (worker_id >= 0 && worker_id < pool->num_workers) {
            pool->active_jobs[worker_id] = NULL;
        }
        atomic_fetch_sub(&pool->active_count, 1);
        pthread_mutex_unlock(&pool->active_mutex);

        /* Move to completed list (for await) */
        completed_entry_t *entry = (completed_entry_t *)malloc(sizeof(completed_entry_t));
        if (entry) {
            entry->ctx = ctx;
            entry->next = NULL;

            pthread_mutex_lock(&pool->completed_mutex);
            entry->next = pool->completed_head;
            pool->completed_head = entry;
            pthread_cond_broadcast(&pool->completed_cond);
            pthread_mutex_unlock(&pool->completed_mutex);
        }
    }

    /* Detach from TSRM */
    php_thread_detach();

    WP_LOGI("Worker %d exiting", worker_id);
    return NULL;
}

/* ─── Public API ─── */

worker_pool_t *worker_pool_create(int num_workers, int max_queue)
{
    if (num_workers < 1) num_workers = 1;
    if (num_workers > 8) num_workers = 8;

    worker_pool_t *pool = (worker_pool_t *)calloc(1, sizeof(worker_pool_t));
    if (!pool) return NULL;

    pool->num_workers = num_workers;
    pool->max_queue = max_queue;

    pool->threads = (pthread_t *)calloc(num_workers, sizeof(pthread_t));
    pool->active_jobs = (php_request_context_t **)calloc(num_workers, sizeof(php_request_context_t *));

    if (!pool->threads || !pool->active_jobs) {
        free(pool->threads);
        free(pool->active_jobs);
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->queue_mutex, NULL);
    pthread_cond_init(&pool->queue_cond, NULL);
    pthread_mutex_init(&pool->completed_mutex, NULL);
    pthread_cond_init(&pool->completed_cond, NULL);
    pthread_mutex_init(&pool->active_mutex, NULL);

    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->queue_count = 0;
    pool->completed_head = NULL;
    atomic_init(&pool->active_count, 0);
    atomic_init(&pool->running, 0);
    atomic_init(&pool->stopped, 0);

    WP_LOGI("Pool created with %d workers, max_queue=%d", num_workers, max_queue);
    return pool;
}

int worker_pool_start(worker_pool_t *pool)
{
    if (!pool) return -1;
    if (atomic_load(&pool->running)) return 0; /* Already started */

    atomic_store(&pool->running, 1);
    atomic_store(&pool->stopped, 0);

    for (int i = 0; i < pool->num_workers; i++) {
        worker_thread_arg_t *targ = (worker_thread_arg_t *)malloc(sizeof(worker_thread_arg_t));
        if (!targ) {
            WP_LOGE("Failed to allocate thread arg for worker %d", i);
            atomic_store(&pool->running, 0);
            pthread_cond_broadcast(&pool->queue_cond);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            return -1;
        }
        targ->pool = pool;
        targ->worker_id = i;

        int rc = pthread_create(&pool->threads[i], NULL, worker_thread_func, targ);
        if (rc != 0) {
            WP_LOGE("Failed to create worker thread %d (rc=%d)", i, rc);
            free(targ);
            /* Stop already-created threads */
            atomic_store(&pool->running, 0);
            pthread_cond_broadcast(&pool->queue_cond);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            return -1;
        }
    }

    WP_LOGI("Pool started with %d workers", pool->num_workers);
    return 0;
}

int worker_pool_submit(worker_pool_t *pool, php_request_context_t *ctx)
{
    if (!pool || !ctx) return -1;
    if (!atomic_load(&pool->running)) return -1;

    job_node_t *node = (job_node_t *)malloc(sizeof(job_node_t));
    if (!node) return -1;

    node->ctx = ctx;
    node->next = NULL;

    pthread_mutex_lock(&pool->queue_mutex);

    if (pool->max_queue > 0 && pool->queue_count >= pool->max_queue) {
        pthread_mutex_unlock(&pool->queue_mutex);
        free(node);
        WP_LOGE("Queue full, rejecting job %s", php_request_get_job_id(ctx));
        return -1;
    }

    if (pool->queue_tail) {
        pool->queue_tail->next = node;
    } else {
        pool->queue_head = node;
    }
    pool->queue_tail = node;
    pool->queue_count++;

    pthread_cond_signal(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    WP_LOGI("Job %s queued (queue_size=%d)", php_request_get_job_id(ctx), pool->queue_count);
    return 0;
}

php_request_context_t *worker_pool_await(worker_pool_t *pool,
                                          const char *job_id,
                                          uint32_t timeout_ms)
{
    if (!pool || !job_id) return NULL;

    struct timespec deadline;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }

    pthread_mutex_lock(&pool->completed_mutex);

    while (1) {
        /* Search completed list */
        completed_entry_t **prev = &pool->completed_head;
        completed_entry_t *entry = pool->completed_head;

        while (entry) {
            if (strcmp(php_request_get_job_id(entry->ctx), job_id) == 0) {
                /* Found — remove from list */
                *prev = entry->next;
                php_request_context_t *ctx = entry->ctx;
                free(entry);
                pthread_mutex_unlock(&pool->completed_mutex);
                return ctx;
            }
            prev = &entry->next;
            entry = entry->next;
        }

        /* Not found yet — wait */
        int rc;
        if (timeout_ms > 0) {
            rc = pthread_cond_timedwait(&pool->completed_cond,
                                         &pool->completed_mutex, &deadline);
            if (rc != 0) {
                /* Timeout */
                pthread_mutex_unlock(&pool->completed_mutex);
                return NULL;
            }
        } else {
            rc = pthread_cond_wait(&pool->completed_cond, &pool->completed_mutex);
        }

        /* Check if pool is stopped */
        if (atomic_load(&pool->stopped)) {
            pthread_mutex_unlock(&pool->completed_mutex);
            return NULL;
        }
    }
}

int worker_pool_cancel(worker_pool_t *pool, const char *job_id)
{
    if (!pool || !job_id) return -1;

    /* Check pending queue */
    pthread_mutex_lock(&pool->queue_mutex);
    job_node_t **prev = &pool->queue_head;
    job_node_t *node = pool->queue_head;

    while (node) {
        if (strcmp(php_request_get_job_id(node->ctx), job_id) == 0) {
            /* Remove from queue */
            *prev = node->next;
            if (node == pool->queue_tail) {
                if (pool->queue_head == NULL) {
                    pool->queue_tail = NULL;
                } else {
                    job_node_t *tail = pool->queue_head;
                    while (tail->next) {
                        tail = tail->next;
                    }
                    pool->queue_tail = tail;
                }
            }
            pool->queue_count--;

            php_request_cancel(node->ctx);

            /* Move to completed */
            completed_entry_t *entry = (completed_entry_t *)malloc(sizeof(completed_entry_t));
            if (entry) {
                entry->ctx = node->ctx;
                entry->next = NULL;
                pthread_mutex_lock(&pool->completed_mutex);
                entry->next = pool->completed_head;
                pool->completed_head = entry;
                pthread_cond_broadcast(&pool->completed_cond);
                pthread_mutex_unlock(&pool->completed_mutex);
            }

            free(node);
            pthread_mutex_unlock(&pool->queue_mutex);
            return 0;
        }
        prev = &node->next;
        node = node->next;
    }
    pthread_mutex_unlock(&pool->queue_mutex);

    /* Check active jobs */
    pthread_mutex_lock(&pool->active_mutex);
    for (int i = 0; i < pool->num_workers; i++) {
        if (pool->active_jobs[i] &&
            strcmp(php_request_get_job_id(pool->active_jobs[i]), job_id) == 0) {
            php_request_cancel(pool->active_jobs[i]);
            pthread_mutex_unlock(&pool->active_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&pool->active_mutex);

    return -1; /* Not found */
}

void worker_pool_stop(worker_pool_t *pool)
{
    if (!pool) return;
    if (!atomic_load(&pool->running)) return;

    WP_LOGI("Stopping pool...");

    atomic_store(&pool->running, 0);
    atomic_store(&pool->stopped, 1);

    /* Cancel all pending jobs */
    pthread_mutex_lock(&pool->queue_mutex);
    job_node_t *node = pool->queue_head;
    while (node) {
        php_request_cancel(node->ctx);
        node = node->next;
    }
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);

    /* Cancel active jobs */
    pthread_mutex_lock(&pool->active_mutex);
    for (int i = 0; i < pool->num_workers; i++) {
        if (pool->active_jobs[i]) {
            php_request_cancel(pool->active_jobs[i]);
        }
    }
    pthread_mutex_unlock(&pool->active_mutex);

    /* Join all threads */
    for (int i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    /* Wake any waiters */
    pthread_cond_broadcast(&pool->completed_cond);

    WP_LOGI("Pool stopped");
}

void worker_pool_destroy(worker_pool_t *pool)
{
    if (!pool) return;

    /* Ensure stopped */
    if (atomic_load(&pool->running)) {
        worker_pool_stop(pool);
    }

    /* Free pending jobs */
    job_node_t *node = pool->queue_head;
    while (node) {
        job_node_t *next = node->next;
        php_request_destroy(node->ctx);
        free(node);
        node = next;
    }

    /* Free completed jobs */
    completed_entry_t *entry = pool->completed_head;
    while (entry) {
        completed_entry_t *next = entry->next;
        php_request_destroy(entry->ctx);
        free(entry);
        entry = next;
    }

    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);
    pthread_mutex_destroy(&pool->completed_mutex);
    pthread_cond_destroy(&pool->completed_cond);
    pthread_mutex_destroy(&pool->active_mutex);

    free(pool->threads);
    free(pool->active_jobs);
    free(pool);

    WP_LOGI("Pool destroyed");
}

int worker_pool_active_count(const worker_pool_t *pool) {
    return pool ? atomic_load(&((worker_pool_t *)pool)->active_count) : 0;
}

int worker_pool_pending_count(const worker_pool_t *pool) {
    if (!pool) return 0;
    /* queue_count is only modified under queue_mutex but reading for stats is ok */
    return ((worker_pool_t *)pool)->queue_count;
}

void worker_pool_wake(worker_pool_t *pool) {
    if (!pool) return;
    /* Signal all waiting worker threads to check the queue immediately.
     * This is used for foreground immediate dispatch. */
    pthread_mutex_lock(&pool->queue_mutex);
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);
}
