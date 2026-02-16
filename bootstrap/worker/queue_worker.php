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
$jobId = getenv('NATIVEPHP_JOB_ID') ?: 'unknown';
error_log("[WORKER-DIAG] queue_worker.php ENTRY jobId={$jobId} pid=" . getmypid());

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
    // ─── Bootstrap Laravel ───

    // Autodetect paths: this file lives in vendor/nativephp/mobile/bootstrap/worker/
    $autoloadPaths = [
        __DIR__ . '/../../../../autoload.php',           // Standard Composer
        __DIR__ . '/../../../autoload.php',              // Alt layout
        __DIR__ . '/../../vendor/autoload.php',          // Direct repo
    ];

    $autoloader = null;
    foreach ($autoloadPaths as $path) {
        if (file_exists($path)) {
            $autoloader = $path;
            break;
        }
    }

    if (!$autoloader) {
        throw new RuntimeException('Could not find Composer autoload.php');
    }

    error_log("[WORKER-DIAG] queue {$jobId}: require autoloader (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');
    require $autoloader;
    error_log("[WORKER-DIAG] queue {$jobId}: autoloader loaded (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');

    // Bootstrap the Laravel application
    $appBootstrapPaths = [
        dirname($autoloader) . '/../bootstrap/app.php',
        dirname($autoloader) . '/bootstrap/app.php',
    ];

    $appBootstrap = null;
    foreach ($appBootstrapPaths as $path) {
        if (file_exists($path)) {
            $appBootstrap = $path;
            break;
        }
    }

    if (!$appBootstrap) {
        throw new RuntimeException('Could not find bootstrap/app.php');
    }

    // Ensure Laravel has valid writable cache/view paths in worker context.
    $basePath = dirname($appBootstrap);
    $legacyStoragePath = $basePath.'/storage';
    $persistedStoragePath = dirname($basePath).'/persisted_data/storage';
    $storagePath = getenv('LARAVEL_STORAGE_PATH') ?: (is_dir($persistedStoragePath) ? $persistedStoragePath : $legacyStoragePath);

    $bootstrapBase = getenv('LARAVEL_BOOTSTRAP_PATH') ?: ($basePath.'/bootstrap');
    $bootstrapCachePath = rtrim($bootstrapBase, '/\\').'/cache';
    $viewCompiledPath = getenv('VIEW_COMPILED_PATH') ?: ($storagePath.'/framework/views');
    $cachePath = getenv('CACHE_PATH') ?: ($storagePath.'/framework/cache');

    foreach ([$storagePath.'/framework', $storagePath.'/framework/views', $storagePath.'/framework/cache', $storagePath.'/framework/sessions', $storagePath.'/logs', $bootstrapCachePath, $viewCompiledPath, $cachePath] as $dir) {
        if (!is_dir($dir)) {
            @mkdir($dir, 0775, true);
        }
    }

    /* Thread-safe: assign only to per-thread superglobal arrays.
     * Do NOT call putenv() — it mutates the process-global environment
     * table, which races with other worker/UI threads in ZTS mode. */
    $_ENV['LARAVEL_STORAGE_PATH'] = $storagePath;
    $_ENV['VIEW_COMPILED_PATH'] = $viewCompiledPath;
    $_ENV['CACHE_PATH'] = $cachePath;
    $_ENV['LARAVEL_BOOTSTRAP_PATH'] = $bootstrapBase;

    $_SERVER['LARAVEL_STORAGE_PATH'] = $storagePath;
    $_SERVER['VIEW_COMPILED_PATH'] = $viewCompiledPath;
    $_SERVER['CACHE_PATH'] = $cachePath;
    $_SERVER['LARAVEL_BOOTSTRAP_PATH'] = $bootstrapBase;

    $artisanPath = $basePath.'/artisan';
    $_SERVER['PHP_SELF'] = $_SERVER['PHP_SELF'] ?? 'artisan';
    $_SERVER['SCRIPT_NAME'] = $_SERVER['SCRIPT_NAME'] ?? 'artisan';
    $_SERVER['SCRIPT_FILENAME'] = $_SERVER['SCRIPT_FILENAME'] ?? $artisanPath;
    $_SERVER['argv'] = $_SERVER['argv'] ?? ['artisan'];
    $_SERVER['argc'] = $_SERVER['argc'] ?? count($_SERVER['argv']);

    error_log("[WORKER-DIAG] queue {$jobId}: require app.php (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');
    $app = require $appBootstrap;
    error_log("[WORKER-DIAG] queue {$jobId}: app created (" . round((hrtime(true) - $startTime) / 1e6) . 'ms) mem=' . round(memory_get_usage(true) / 1048576) . 'M');

    if (method_exists($app, 'useStoragePath')) {
        $app->useStoragePath($storagePath);
    }

    // Create the console kernel
    error_log("[WORKER-DIAG] queue {$jobId}: creating kernel (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');
    $kernel = $app->make(\Illuminate\Contracts\Console\Kernel::class);
    error_log("[WORKER-DIAG] queue {$jobId}: kernel->bootstrap() start (" . round((hrtime(true) - $startTime) / 1e6) . 'ms) mem=' . round(memory_get_usage(true) / 1048576) . 'M');
    $kernel->bootstrap();
    error_log("[WORKER-DIAG] queue {$jobId}: kernel->bootstrap() done (" . round((hrtime(true) - $startTime) / 1e6) . 'ms) mem=' . round(memory_get_usage(true) / 1048576) . 'M');

    $config = $app->make('config');
    $config->set('view.compiled', $viewCompiledPath);

    // ─── Enforce memory limit per worker request ───
    // The C layer (php_request_execute) already sets memory_limit from
    // the supervisor config.  Only override here if a per-thread
    // $_SERVER value was injected (thread-safe, no getenv).
    $memoryLimit = $_SERVER['NATIVEPHP_WORKER_MEMORY_LIMIT'] ?? $_ENV['NATIVEPHP_WORKER_MEMORY_LIMIT'] ?? null;
    if ($memoryLimit) {
        ini_set('memory_limit', $memoryLimit);
    }

    // ─── Read Configuration ───

    $connection = getenv('NATIVEPHP_QUEUE_CONNECTION') ?: config('queue.default', 'database');
    $queueNames = getenv('NATIVEPHP_QUEUE_NAMES') ?: 'default';
    $queues = explode(',', $queueNames);

    // ─── SQLite WAL is configured by WorkerServiceProvider ───
    // (No duplicate PRAGMA calls — the service provider handles it
    //  via ConnectionEstablished event listener, which fires on first
    //  DB::connection() call below.)

    // ─── Pop and Execute ONE Job ───

    error_log("[WORKER-DIAG] queue {$jobId}: popping job from queues={$queueNames} conn={$connection} (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');
    $manager = $app->make('queue');

    $job = null;
    foreach ($queues as $queue) {
        $queue = trim($queue);
        $job = $manager->connection($connection)->pop($queue);
        if ($job) break;
    }

    if ($job) {
        $result['job_name'] = $job->resolveName();
        error_log("[WORKER-DIAG] queue {$jobId}: firing job {$result['job_name']} (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');

        try {
            $job->fire();
            error_log("[WORKER-DIAG] queue {$jobId}: job fired OK (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');

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

            $result['error'] = $e->getMessage();
            $result['processed'] = true; // We did process it, just with an error
        }

        $result['job_name'] = $job->resolveName();
    }

} catch (\Throwable $e) {
    $result['error'] = $e->getMessage() . ' in ' . $e->getFile() . ':' . $e->getLine();
    error_log("[WORKER-DIAG] queue {$jobId}: EXCEPTION: {$result['error']}");
}

// ─── Output Result ───

// Clear any buffered output
ob_end_clean();

$result['duration_ms'] = (int) ((hrtime(true) - $startTime) / 1_000_000);

echo json_encode($result, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
