<?php

/**
 * scheduler_tick.php — Run `schedule:run` once, then exit.
 *
 * This is the entrypoint executed by the native supervisor for each
 * scheduler tick. It bootstraps Laravel and runs the schedule:run
 * artisan command, capturing its output as JSON.
 *
 * Environment variables (set by native layer):
 *   NATIVEPHP_JOB_ID       — Unique job ID from the supervisor
 *   NATIVEPHP_JOB_TYPE     — "scheduler"
 *   NATIVEPHP_JOB_PAYLOAD  — Optional JSON payload (unused)
 *
 * Output: JSON on stdout
 *   { "ran": bool, "output": string, "error": string|null, "duration_ms": int }
 */

// Timing
$startTime = hrtime(true);

// Suppress accidental output without accumulating it in memory.
// Use chunked handling so long/noisy commands cannot grow a giant output buffer.
ob_start(static function (string $buffer): string {
    return '';
}, 16 * 1024);

$result = [
    'ran' => false,
    'output' => '',
    'error' => null,
    'duration_ms' => 0,
];

try {
    // ─── Bootstrap Laravel (shared) ───
    require __DIR__ . '/common.php';
    // $app, $kernel, $config, $jobId are now available

    // ─── Run scheduled events (fork-safe) ───
    //
    // IMPORTANT: We do NOT use `$kernel->call('schedule:run')` here.
    //
    // Laravel's schedule:run command internally uses
    // Symfony\Component\Process\Process::fromShellCommandline() to execute
    // each scheduled command event. This calls proc_open() → fork().
    //
    // In the Android ZTS worker pool (~65 threads), fork() deadlocks: the
    // forked child inherits locked mutexes from all other threads and hangs
    // before reaching exec(), causing the parent's read() on the error pipe
    // to block forever — permanently killing the worker thread.
    //
    // SafeScheduleRunner replaces this with Artisan::call(), which runs
    // each command in the current thread without forking.

    worker_diag("sched {$jobId}: SafeScheduleRunner::run start mem=" . round(memory_get_usage(true) / 1048576) . 'M', $startTime);
    $scheduleResults = \Native\Mobile\Worker\SafeScheduleRunner::run($app);
    worker_diag("sched {$jobId}: SafeScheduleRunner::run done ran={$scheduleResults['ran']} skipped={$scheduleResults['skipped']} failed={$scheduleResults['failed']}", $startTime);

    $result['ran'] = true;
    $result['output'] = json_encode($scheduleResults['events'], JSON_UNESCAPED_SLASHES);
    $result['exit_code'] = $scheduleResults['failed'] > 0 ? 1 : 0;
    $result['schedule_ran'] = $scheduleResults['ran'];
    $result['schedule_skipped'] = $scheduleResults['skipped'];
    $result['schedule_failed'] = $scheduleResults['failed'];

    $kernel->terminate(
        new \Symfony\Component\Console\Input\ArrayInput(['command' => 'schedule:run']),
        $result['exit_code']
    );

} catch (\Throwable $e) {
    $result['error'] = $e->getMessage() . ' in ' . $e->getFile() . ':' . $e->getLine();
    error_log("[WORKER] sched EXCEPTION: {$result['error']}");

    // Best-effort: report to unified error table
    try {
        \Native\Mobile\Worker\WorkerErrorReporter::capture($e, ['job_type' => 'scheduler']);
    } catch (\Throwable $_) {
        // Bootstrap incomplete — can't write to DB
    }
}

// ─── Output Result ───

ob_end_clean();

$result['duration_ms'] = (int) ((hrtime(true) - $startTime) / 1_000_000);

echo json_encode($result, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
