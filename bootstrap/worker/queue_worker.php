<?php

/**
 * queue_worker.php — Pop and execute ONE queue job, then exit.
 *
 * This is the entrypoint executed by the native worker pool for each
 * queue job run. It bootstraps Laravel, pops a single job from the
 * configured queue, executes it, and outputs a JSON result.
 *
 * Environment variables (set by native layer):
 *   NATIVEPHP_JOB_ID       — Unique job ID from the supervisor
 *   NATIVEPHP_JOB_TYPE     — "queue"
 *   NATIVEPHP_JOB_PAYLOAD  — Optional JSON payload
 *   NATIVEPHP_QUEUE_CONNECTION — Queue connection name (default: "database")
 *   NATIVEPHP_QUEUE_NAMES  — Comma-separated queue names (default: "default")
 *
 * Output: JSON on stdout
 *   { "processed": bool, "job_name": string|null, "error": string|null, "duration_ms": int }
 */

// Timing
$startTime = hrtime(true);

// Suppress accidental output without accumulating it in memory.
// Use chunked handling so long/noisy jobs cannot grow a giant output buffer.
ob_start(static function (string $buffer): string {
    return '';
}, 16 * 1024);

$result = [
    'processed' => false,
    'job_name' => null,
    'error' => null,
    'duration_ms' => 0,
];

try {
    // ─── Bootstrap Laravel (shared) ───
    require __DIR__ . '/common.php';
    // $app, $kernel, $config, $jobId are now available

    // ─── Read Configuration ───

    $connection = getenv('NATIVEPHP_QUEUE_CONNECTION') ?: config('queue.default', 'database');
    $queueNames = getenv('NATIVEPHP_QUEUE_NAMES') ?: 'default';
    $queues = explode(',', $queueNames);

    // ─── Pop and Execute ONE Job ───

    worker_diag("queue {$jobId}: popping job from queues={$queueNames} conn={$connection}", $startTime);
    $manager = $app->make('queue');

    $job = null;
    foreach ($queues as $queue) {
        $queue = trim($queue);
        $job = $manager->connection($connection)->pop($queue);
        if ($job) break;
    }

    if ($job) {
        $result['job_name'] = $job->resolveName();
        worker_diag("queue {$jobId}: firing job {$result['job_name']}", $startTime);

        try {
            $job->fire();
            worker_diag("queue {$jobId}: job fired OK", $startTime);

            if (!$job->isDeleted() && !$job->isReleased() && !$job->hasFailed()) {
                $job->delete();
            }

            $result['processed'] = true;
        } catch (\Throwable $e) {
            // Mark the job as failed
            if (method_exists($job, 'fail')) {
                $job->fail($e);
            } else {
                $job->delete();
            }

            // Report to unified error table
            \Native\Mobile\Worker\WorkerErrorReporter::capture($e, [
                'job_type' => 'queue',
                'queue' => $queue ?? null,
                'job_class' => $result['job_name'],
                'job_id' => $jobId ?? null,
            ]);

            $result['error'] = $e->getMessage();
            $result['processed'] = true; // We did process it, just with an error
        }

        $result['job_name'] = $job->resolveName();
    }

} catch (\Throwable $e) {
    $result['error'] = $e->getMessage() . ' in ' . $e->getFile() . ':' . $e->getLine();
    error_log("[WORKER] queue EXCEPTION: {$result['error']}");

    // Best-effort: report to unified error table (may fail if bootstrap didn't complete)
    try {
        \Native\Mobile\Worker\WorkerErrorReporter::capture($e, ['job_type' => 'queue']);
    } catch (\Throwable $_) {
        // Bootstrap incomplete — can't write to DB
    }
}

// ─── Output Result ───

// Clear any buffered output
ob_end_clean();

$result['duration_ms'] = (int) ((hrtime(true) - $startTime) / 1_000_000);

echo json_encode($result, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
