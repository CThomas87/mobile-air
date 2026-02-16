/**
 * scheduler_gate.h — Mutex gate ensuring schedule:run never overlaps
 *
 * Only one scheduler tick can run at a time. If a tick is already
 * running, new ticks are rejected (not queued).
 */
#ifndef NATIVEPHP_SCHEDULER_GATE_H
#define NATIVEPHP_SCHEDULER_GATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle */
typedef struct scheduler_gate scheduler_gate_t;

/**
 * Create a scheduler gate.
 * @return Gate handle, or NULL on failure
 */
scheduler_gate_t *scheduler_gate_create(void);

/**
 * Destroy the gate.
 */
void scheduler_gate_destroy(scheduler_gate_t *gate);

/**
 * Try to acquire the gate for a scheduler tick.
 * @return 1 if acquired (caller should run the tick), 0 if already running
 */
int scheduler_gate_try_acquire(scheduler_gate_t *gate);

/**
 * Release the gate after a scheduler tick completes.
 */
void scheduler_gate_release(scheduler_gate_t *gate);

/**
 * Check if a scheduler tick is currently running.
 */
int scheduler_gate_is_running(const scheduler_gate_t *gate);

#ifdef __cplusplus
}
#endif

#endif /* NATIVEPHP_SCHEDULER_GATE_H */
