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
$jobId = getenv('NATIVEPHP_JOB_ID') ?: 'unknown';
error_log("[WORKER-DIAG] scheduler_tick.php ENTRY jobId={$jobId} pid=" . getmypid());

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
    // ─── Bootstrap Laravel ───

    $autoloadPaths = [
        __DIR__ . '/../../../../autoload.php',
        __DIR__ . '/../../../autoload.php',
        __DIR__ . '/../../vendor/autoload.php',
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

    error_log("[WORKER-DIAG] sched {$jobId}: require autoloader (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');
    require $autoloader;
    error_log("[WORKER-DIAG] sched {$jobId}: autoloader loaded (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');

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

    error_log("[WORKER-DIAG] sched {$jobId}: require app.php (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');
    $app = require $appBootstrap;
    error_log("[WORKER-DIAG] sched {$jobId}: app created (" . round((hrtime(true) - $startTime) / 1e6) . 'ms) mem=' . round(memory_get_usage(true) / 1048576) . 'M');

    if (method_exists($app, 'useStoragePath')) {
        $app->useStoragePath($storagePath);
    }

    error_log("[WORKER-DIAG] sched {$jobId}: creating kernel (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');
    $kernel = $app->make(\Illuminate\Contracts\Console\Kernel::class);

    // ─── Enforce memory limit per worker request ───
    // The C layer (php_request_execute) already sets memory_limit from
    // the supervisor config.  Only override here if a per-thread
    // $_SERVER value was injected (thread-safe, no getenv).
    $memoryLimit = $_SERVER['NATIVEPHP_WORKER_MEMORY_LIMIT'] ?? $_ENV['NATIVEPHP_WORKER_MEMORY_LIMIT'] ?? null;
    if ($memoryLimit) {
        ini_set('memory_limit', $memoryLimit);
    }

    // ─── Configure SQLite WAL (handled by WorkerServiceProvider) ───

    error_log("[WORKER-DIAG] sched {$jobId}: kernel->bootstrap() start (" . round((hrtime(true) - $startTime) / 1e6) . 'ms) mem=' . round(memory_get_usage(true) / 1048576) . 'M');
    $kernel->bootstrap();
    error_log("[WORKER-DIAG] sched {$jobId}: kernel->bootstrap() done (" . round((hrtime(true) - $startTime) / 1e6) . 'ms) mem=' . round(memory_get_usage(true) / 1048576) . 'M');

    $config = $app->make('config');
    $config->set('view.compiled', $viewCompiledPath);

    // ─── Run schedule:run ───

    error_log("[WORKER-DIAG] sched {$jobId}: schedule:run start (" . round((hrtime(true) - $startTime) / 1e6) . 'ms) mem=' . round(memory_get_usage(true) / 1048576) . 'M');
    $output = new \Symfony\Component\Console\Output\NullOutput();
    $exitCode = $kernel->call('schedule:run', ['--no-ansi' => true, '--quiet' => true], $output);
    error_log("[WORKER-DIAG] sched {$jobId}: schedule:run done exitCode={$exitCode} (" . round((hrtime(true) - $startTime) / 1e6) . 'ms)');

    $result['ran'] = true;
    $result['output'] = '';
    $result['exit_code'] = $exitCode;

    $kernel->terminate(
        new \Symfony\Component\Console\Input\ArrayInput(['command' => 'schedule:run']),
        $exitCode
    );

} catch (\Throwable $e) {
    $result['error'] = $e->getMessage() . ' in ' . $e->getFile() . ':' . $e->getLine();
    error_log("[WORKER-DIAG] sched {$jobId}: EXCEPTION: {$result['error']}");
}

// ─── Output Result ───

ob_end_clean();

$result['duration_ms'] = (int) ((hrtime(true) - $startTime) / 1_000_000);

echo json_encode($result, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
