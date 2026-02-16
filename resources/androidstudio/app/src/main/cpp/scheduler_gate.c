/**
 * scheduler_gate.c — Scheduler tick mutual exclusion
 */
#include "scheduler_gate.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>

#ifdef __ANDROID__
#include <android/log.h>
#define SG_TAG "SchedGate"
#define SG_LOGI(...) __android_log_print(ANDROID_LOG_INFO, SG_TAG, __VA_ARGS__)
#else
#define SG_LOGI(...) do { fprintf(stdout, "[SchedGate] "); fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while(0)
#endif

struct scheduler_gate {
    pthread_mutex_t mutex;
    atomic_int      running;
};

scheduler_gate_t *scheduler_gate_create(void)
{
    scheduler_gate_t *gate = (scheduler_gate_t *)calloc(1, sizeof(scheduler_gate_t));
    if (!gate) return NULL;

    pthread_mutex_init(&gate->mutex, NULL);
    atomic_init(&gate->running, 0);

    return gate;
}

void scheduler_gate_destroy(scheduler_gate_t *gate)
{
    if (!gate) return;
    pthread_mutex_destroy(&gate->mutex);
    free(gate);
}

int scheduler_gate_try_acquire(scheduler_gate_t *gate)
{
    if (!gate) return 0;

    int expected = 0;
    if (atomic_compare_exchange_strong(&gate->running, &expected, 1)) {
        SG_LOGI("Scheduler gate acquired");
        return 1;
    }

    SG_LOGI("Scheduler gate busy — tick already running");
    return 0;
}

void scheduler_gate_release(scheduler_gate_t *gate)
{
    if (!gate) return;
    atomic_store(&gate->running, 0);
    SG_LOGI("Scheduler gate released");
}

int scheduler_gate_is_running(const scheduler_gate_t *gate)
{
    return gate ? atomic_load(&((scheduler_gate_t *)gate)->running) : 0;
}
