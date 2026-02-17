<?php

/**
 * ios_worker.php — iOS BGProcessingTask entrypoint.
 *
 * Called by the Swift bridge when a BGProcessingTask fires. Processes
 * as many queue jobs as possible within the iOS time budget (~25 seconds)
 * and optionally runs the scheduler.
 *
 * This entrypoint reuses the shared common.php bootstrap and the
 * IosWorkerScheduler class which handles the time-constrained execution
 * model specific to iOS background tasks.
 *
 * Environment variables (set by native layer):
 *   NATIVEPHP_JOB_ID       — Unique task ID from the Swift bridge
 *   NATIVEPHP_JOB_TYPE     — "ios_worker"
 *   NATIVEPHP_PLATFORM     — "ios"
 *
 * Output: JSON on stdout
 *   { "jobs_processed": int, "jobs_failed": int, "scheduler_ran": bool,
 *     "stopped_reason": string, "duration_ms": int }
 */

// Timing
$startTime = hrtime(true);

// Suppress accidental output
ob_start(static function (string $buffer): string {
    return '';
}, 16 * 1024);

$result = [
    'jobs_processed' => 0,
    'jobs_failed' => 0,
    'scheduler_ran' => false,
    'scheduler_error' => null,
    'stopped_reason' => 'error',
    'duration_ms' => 0,
];

try {
    // ─── Bootstrap Laravel (shared) ───
    require __DIR__ . '/../worker/common.php';
    // $app, $kernel, $config, $jobId are now available

    // ─── Run iOS worker scheduler ───
    $scheduler = new \Native\Mobile\Worker\IosWorkerScheduler();
    $result = $scheduler->execute();

} catch (\Throwable $e) {
    $result['stopped_reason'] = 'bootstrap_error';
    $result['error'] = $e->getMessage() . ' in ' . $e->getFile() . ':' . $e->getLine();
    error_log("[WORKER] ios_worker EXCEPTION: {$result['error']}");

    try {
        \Native\Mobile\Worker\WorkerErrorReporter::capture($e, ['job_type' => 'ios_worker']);
    } catch (\Throwable $_) {
        // Bootstrap incomplete
    }
}

// ─── Output Result ───
ob_end_clean();

$result['duration_ms'] = (int) ((hrtime(true) - $startTime) / 1_000_000);

echo json_encode($result, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
