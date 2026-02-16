<?php

/**
 * NativePHP Worker Configuration
 *
 * These values control the behavior of the native PHP background worker
 * system. All values have sensible defaults that work out of the box
 * for a standard Laravel project — you only need to publish and edit
 * this file if you want to customize behavior.
 *
 * Environment variables take precedence over these defaults, so you
 * can also configure via .env without publishing this config.
 */
return [

    /*
    |--------------------------------------------------------------------------
    | Queue Connection
    |--------------------------------------------------------------------------
    |
    | The queue connection to use for background job processing.
    | Default: uses your app's default queue connection (usually "database").
    |
    | Env: NATIVEPHP_QUEUE_CONNECTION
    |
    */
    'connection' => env('NATIVEPHP_QUEUE_CONNECTION', null), // null = use app default

    /*
    |--------------------------------------------------------------------------
    | Queue Names
    |--------------------------------------------------------------------------
    |
    | Comma-separated list of queue names to process.
    |
    | Env: NATIVEPHP_QUEUE_NAMES
    |
    */
    'queues' => env('NATIVEPHP_QUEUE_NAMES', 'default'),

    /*
    |--------------------------------------------------------------------------
    | Worker Count
    |--------------------------------------------------------------------------
    |
    | Number of concurrent worker threads.
    | Android default: 2, iOS default: 1 (iOS background is more constrained).
    |
    | Env: NATIVEPHP_WORKER_COUNT
    |
    */
    'worker_count' => env('NATIVEPHP_WORKER_COUNT', null), // null = platform default

    /*
    |--------------------------------------------------------------------------
    | Supervisor Mode
    |--------------------------------------------------------------------------
    |
    | 0 = all (queue workers + scheduler)
    | 1 = queue workers only
    | 2 = scheduler only
    |
    | Env: NATIVEPHP_WORKER_MODE
    |
    */
    'mode' => env('NATIVEPHP_WORKER_MODE', 0),

    /*
    |--------------------------------------------------------------------------
    | Scheduler Interval (seconds)
    |--------------------------------------------------------------------------
    |
    | How often to run schedule:run. Default: 60 seconds.
    |
    | Env: NATIVEPHP_SCHEDULER_INTERVAL
    |
    */
    'scheduler_interval' => env('NATIVEPHP_SCHEDULER_INTERVAL', 60),

    /*
    |--------------------------------------------------------------------------
    | Queue Poll Interval (seconds)
    |--------------------------------------------------------------------------
    |
    | How often to check for new queue jobs. Default: 5 seconds.
    |
    | Env: NATIVEPHP_QUEUE_POLL_INTERVAL
    |
    */
    'queue_poll_interval' => env('NATIVEPHP_QUEUE_POLL_INTERVAL', 5),

    /*
    |--------------------------------------------------------------------------
    | Maximum Background Runtime — Android (seconds)
    |--------------------------------------------------------------------------
    |
    | Maximum time to run during a background execution window on Android.
    | Android foreground services can run for hours, but a safety ceiling
    | prevents runaway execution. Default: 6 hours (Android 14 dataSync limit).
    |
    | Env: NATIVEPHP_MAX_BG_RUNTIME_ANDROID
    |
    */
    'max_background_runtime_android' => env('NATIVEPHP_MAX_BG_RUNTIME_ANDROID', 6 * 60 * 60),

    /*
    |--------------------------------------------------------------------------
    | Maximum Background Runtime — iOS (seconds)
    |--------------------------------------------------------------------------
    |
    | Maximum time per BGProcessingTask invocation on iOS.
    | iOS grants ~30 seconds per task. We use 25s for a 5s safety margin.
    | Do NOT set this higher than 30 — iOS will kill the task.
    |
    | Env: NATIVEPHP_MAX_BG_RUNTIME_IOS
    |
    */
    'max_background_runtime_ios' => env('NATIVEPHP_MAX_BG_RUNTIME_IOS', 25),

    /*
    |--------------------------------------------------------------------------
    | Auto-Start Workers
    |--------------------------------------------------------------------------
    |
    | Whether to automatically start background workers when the app launches.
    | Default: true when running natively, false otherwise.
    |
    | Env: NATIVEPHP_WORKER_AUTO_START
    |
    */
    'auto_start' => env('NATIVEPHP_WORKER_AUTO_START', null), // null = auto-detect

    /*
    |--------------------------------------------------------------------------
    | Override Sync Driver
    |--------------------------------------------------------------------------
    |
    | When true (default), the worker system automatically promotes the
    | 'sync' queue driver to 'database' so that dispatched jobs are
    | persisted and processed by background workers.
    |
    | Set to false if your app intentionally uses 'sync' for some jobs
    | and you want to manage the queue driver yourself.
    |
    | Env: NATIVEPHP_OVERRIDE_SYNC_DRIVER
    |
    */
    'override_sync_driver' => env('NATIVEPHP_OVERRIDE_SYNC_DRIVER', true),

    /*
    |--------------------------------------------------------------------------
    | Maximum Worker Count (hard cap)
    |--------------------------------------------------------------------------
    |
    | Absolute upper limit on concurrent worker threads. Each thread holds
    | a full TSRM interpreter context (~2-8MB). On memory-constrained
    | mobile devices, more than 4 threads risks OOM kills.
    |
    */
    'max_worker_count' => 4,

    /*
    |--------------------------------------------------------------------------
    | Circuit Breaker — Max Consecutive Crashes
    |--------------------------------------------------------------------------
    |
    | If a worker thread crashes (segfault, fatal error) N times
    | consecutively, it pauses for an exponential backoff period.
    | This prevents infinite crash loops from a poison-pill job.
    |
    | Env: NATIVEPHP_CIRCUIT_BREAKER_THRESHOLD
    |
    */
    'circuit_breaker_threshold' => env('NATIVEPHP_CIRCUIT_BREAKER_THRESHOLD', 3),

    /*
    |--------------------------------------------------------------------------
    | Circuit Breaker — Base Backoff (seconds)
    |--------------------------------------------------------------------------
    |
    | Base delay before a crashed thread resumes. Doubles on each
    | consecutive crash: 5s → 10s → 20s → ...
    |
    | Env: NATIVEPHP_CIRCUIT_BREAKER_BACKOFF
    |
    */
    'circuit_breaker_backoff' => env('NATIVEPHP_CIRCUIT_BREAKER_BACKOFF', 5),

    /*
    |--------------------------------------------------------------------------
    | Memory Limit Per Worker (MB)
    |--------------------------------------------------------------------------
    |
    | PHP memory_limit applied to each worker request. Prevents a
    | runaway job from exhausting device memory and triggering OOM.
    |
    | Env: NATIVEPHP_WORKER_MEMORY_LIMIT
    |
    */
    'memory_limit' => env('NATIVEPHP_WORKER_MEMORY_LIMIT', '512M'),

    /*
    |--------------------------------------------------------------------------
    | Worker Log
    |--------------------------------------------------------------------------
    |
    | Enable structured worker log file. The native supervisor appends
    | JSON-line events to this file. Readable via `native:tail --type=worker`.
    |
    */
    'log_enabled' => env('NATIVEPHP_WORKER_LOG_ENABLED', true),
    'log_max_size_kb' => env('NATIVEPHP_WORKER_LOG_MAX_SIZE', 1024), // 1MB ring

    /*
    |--------------------------------------------------------------------------
    | Foreground Immediate Dispatch
    |--------------------------------------------------------------------------
    |
    | When the app is in the foreground, wake the worker pool immediately
    | on job dispatch instead of waiting for the next poll interval.
    | This reduces dispatch-to-execute latency to <500ms.
    |
    | Env: NATIVEPHP_WORKER_IMMEDIATE_DISPATCH
    |
    */
    'immediate_dispatch' => env('NATIVEPHP_WORKER_IMMEDIATE_DISPATCH', true),

    /*
    |--------------------------------------------------------------------------
    | OPcache Shared Memory
    |--------------------------------------------------------------------------
    |
    | When enabled, worker threads share compiled PHP bytecode via OPcache's
    | shared memory segment. This dramatically reduces memory usage when
    | running multiple worker threads (each thread reuses the same compiled
    | code instead of recompiling).
    |
    | The memory size (in MB) controls how much bytecode can be cached.
    | 32MB is sufficient for most Laravel apps; increase for large codebases.
    |
    */
    'opcache_enabled' => env('NATIVEPHP_OPCACHE_ENABLED', true),
    'opcache_memory_mb' => env('NATIVEPHP_OPCACHE_MEMORY_MB', 32),

    /*
    |--------------------------------------------------------------------------
    | Redis Queue Backend
    |--------------------------------------------------------------------------
    |
    | If your app bundles a Redis client (predis/predis or phpredis extension),
    | you can use Redis as the queue backend instead of SQLite.
    |
    | Set 'redis_enabled' to true and configure the connection details.
    | When enabled, the worker system will prefer Redis over database.
    |
    | NOTE: SQLite/database is the recommended default for offline-first
    | mobile apps. Redis requires network connectivity.
    |
    */
    'redis_enabled' => env('NATIVEPHP_REDIS_ENABLED', false),
    'redis_connection' => env('NATIVEPHP_REDIS_CONNECTION', 'default'),

    /*
    |--------------------------------------------------------------------------
    | Build-Time Config Cache
    |--------------------------------------------------------------------------
    |
    | When enabled, the worker bootstrap pre-caches Laravel's configuration
    | on first boot, reducing subsequent startup time by ~40%.
    |
    | The cache is invalidated automatically when config files change.
    | Disable this if you encounter stale config issues during development.
    |
    */
    'config_cache_enabled' => env('NATIVEPHP_CONFIG_CACHE_ENABLED', true),

];
