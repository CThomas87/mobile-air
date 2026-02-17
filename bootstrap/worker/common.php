<?php

/**
 * common.php — Shared bootstrap logic for worker entrypoints.
 *
 * Extracted from queue_worker.php and scheduler_tick.php to eliminate
 * ~60 lines of duplicated path detection, directory creation, and
 * superglobal setup.
 *
 * Usage:
 *   $startTime = hrtime(true);
 *   require __DIR__ . '/common.php';
 *   // $app, $kernel, $config are now available
 *
 * Expects $startTime to be set before inclusion.
 */

// ─── Debug logging helper ───
// Gate [WORKER-DIAG] lines behind NATIVEPHP_DEBUG env var so they're
// available during development but silent in production builds.
$__workerDebug = in_array(strtolower((string) getenv('NATIVEPHP_DEBUG')), ['1', 'true', 'yes', 'on'], true);

if (! function_exists('worker_diag')) {
    /**
     * Emit a diagnostic log line, gated by NATIVEPHP_DEBUG.
     */
    function worker_diag(string $message, float $startTime): void
    {
        global $__workerDebug;
        if ($__workerDebug) {
            $elapsed = round((hrtime(true) - $startTime) / 1e6);
            error_log("[WORKER-DIAG] {$message} ({$elapsed}ms)");
        }
    }
}

$jobId = getenv('NATIVEPHP_JOB_ID') ?: 'unknown';
$jobType = getenv('NATIVEPHP_JOB_TYPE') ?: 'unknown';

worker_diag("{$jobType} {$jobId}: bootstrap start pid=" . getmypid(), $startTime);

// ─── Locate Composer autoloader ───

$autoloadPaths = [
    __DIR__ . '/../../../../autoload.php',       // Standard Composer (vendor/nativephp/mobile/bootstrap/worker/)
    __DIR__ . '/../../../autoload.php',          // Alt layout
    __DIR__ . '/../../vendor/autoload.php',      // Direct repo
];

$autoloader = null;
foreach ($autoloadPaths as $path) {
    if (file_exists($path)) {
        $autoloader = $path;
        break;
    }
}

if (! $autoloader) {
    throw new RuntimeException('Could not find Composer autoload.php');
}

worker_diag("{$jobType} {$jobId}: require autoloader", $startTime);
require $autoloader;
worker_diag("{$jobType} {$jobId}: autoloader loaded", $startTime);

// ─── Prevent fork() deadlock in multithreaded process ───
// Symfony Console's Terminal::getWidth() calls initDimensions() which
// uses proc_open(['stty','-a']) to detect terminal size.  proc_open()
// calls fork() — UNSAFE in a multithreaded process: the forked child
// inherits locked mutexes from ~65 threads and can deadlock before
// exec(), causing the parent's error-pipe read() to block forever and
// permanently hang the worker thread.
//
// Pre-setting the private static $width/$height via reflection prevents
// initDimensions() from being called, avoiding fork() entirely.
// This also protects any code path that uses Artisan commands, Console
// output components, or Termwind rendering in worker context.
if (class_exists(\Symfony\Component\Console\Terminal::class, false)
    || class_exists(\Symfony\Component\Console\Terminal::class)) {
    $__termRef = new \ReflectionClass(\Symfony\Component\Console\Terminal::class);
    foreach (['width' => 80, 'height' => 50] as $__prop => $__val) {
        $__rp = $__termRef->getProperty($__prop);
        $__rp->setValue(null, $__val);
    }
    unset($__termRef, $__rp, $__prop, $__val);
}

// ─── Locate bootstrap/app.php ───

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

if (! $appBootstrap) {
    throw new RuntimeException('Could not find bootstrap/app.php');
}

// ─── Resolve storage & cache paths ───

$basePath = dirname($appBootstrap);
$legacyStoragePath = $basePath . '/storage';
$persistedStoragePath = dirname($basePath) . '/persisted_data/storage';
$storagePath = getenv('LARAVEL_STORAGE_PATH') ?: (is_dir($persistedStoragePath) ? $persistedStoragePath : $legacyStoragePath);

$bootstrapBase = getenv('LARAVEL_BOOTSTRAP_PATH') ?: ($basePath . '/bootstrap');
$bootstrapCachePath = rtrim($bootstrapBase, '/\\') . '/cache';
$viewCompiledPath = getenv('VIEW_COMPILED_PATH') ?: ($storagePath . '/framework/views');
$cachePath = getenv('CACHE_PATH') ?: ($storagePath . '/framework/cache');

// ─── Ensure writable directories exist ───

$requiredDirs = [
    $storagePath . '/framework',
    $storagePath . '/framework/views',
    $storagePath . '/framework/cache',
    $storagePath . '/framework/sessions',
    $storagePath . '/logs',
    $bootstrapCachePath,
    $viewCompiledPath,
    $cachePath,
];

foreach ($requiredDirs as $dir) {
    if (! is_dir($dir)) {
        @mkdir($dir, 0775, true);
    }
}

// ─── Thread-safe superglobal assignment ───
// Do NOT call putenv() — it mutates the process-global environment
// table, which races with other worker/UI threads in ZTS mode.

$_ENV['LARAVEL_STORAGE_PATH'] = $storagePath;
$_ENV['VIEW_COMPILED_PATH'] = $viewCompiledPath;
$_ENV['CACHE_PATH'] = $cachePath;
$_ENV['LARAVEL_BOOTSTRAP_PATH'] = $bootstrapBase;

$_SERVER['LARAVEL_STORAGE_PATH'] = $storagePath;
$_SERVER['VIEW_COMPILED_PATH'] = $viewCompiledPath;
$_SERVER['CACHE_PATH'] = $cachePath;
$_SERVER['LARAVEL_BOOTSTRAP_PATH'] = $bootstrapBase;

$artisanPath = $basePath . '/artisan';
$_SERVER['PHP_SELF'] = $_SERVER['PHP_SELF'] ?? 'artisan';
$_SERVER['SCRIPT_NAME'] = $_SERVER['SCRIPT_NAME'] ?? 'artisan';
$_SERVER['SCRIPT_FILENAME'] = $_SERVER['SCRIPT_FILENAME'] ?? $artisanPath;
$_SERVER['argv'] = $_SERVER['argv'] ?? ['artisan'];
$_SERVER['argc'] = $_SERVER['argc'] ?? count($_SERVER['argv']);

// ─── Create and bootstrap the Laravel application ───

worker_diag("{$jobType} {$jobId}: require app.php", $startTime);
$app = require $appBootstrap;
worker_diag("{$jobType} {$jobId}: app created mem=" . round(memory_get_usage(true) / 1048576) . 'M', $startTime);

if (method_exists($app, 'useStoragePath')) {
    $app->useStoragePath($storagePath);
}

worker_diag("{$jobType} {$jobId}: creating kernel", $startTime);
$kernel = $app->make(\Illuminate\Contracts\Console\Kernel::class);

// ─── Enforce memory limit per worker request ───
$memoryLimit = $_SERVER['NATIVEPHP_WORKER_MEMORY_LIMIT'] ?? $_ENV['NATIVEPHP_WORKER_MEMORY_LIMIT'] ?? null;
if ($memoryLimit) {
    ini_set('memory_limit', $memoryLimit);
}

worker_diag("{$jobType} {$jobId}: kernel->bootstrap() start mem=" . round(memory_get_usage(true) / 1048576) . 'M', $startTime);
$kernel->bootstrap();
worker_diag("{$jobType} {$jobId}: kernel->bootstrap() done mem=" . round(memory_get_usage(true) / 1048576) . 'M', $startTime);

$config = $app->make('config');
$config->set('view.compiled', $viewCompiledPath);
