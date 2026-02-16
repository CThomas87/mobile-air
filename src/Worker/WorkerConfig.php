<?php

namespace Native\Mobile\Worker;

/**
 * WorkerConfig — Configuration for the native PHP worker system.
 *
 * Reads from environment variables by default, requiring zero
 * configuration in a standard Laravel project. All values can
 * be overridden via the nativephp-worker.php config file.
 */
class WorkerConfig
{
    /** Hard cap on concurrent worker threads */
    public const MAX_WORKER_HARD_CAP = 4;

    /**
     * Get the queue connection to use.
     * Default: "database" (works offline with SQLite)
     */
    public static function connection(): string
    {
        return \config('nativephp-worker.connection')
            ?? \env('NATIVEPHP_QUEUE_CONNECTION')
            ?? \config('queue.default')
            ?? 'database';
    }

    /**
     * Get the comma-separated queue names to process.
     * Default: "default"
     */
    public static function queues(): string
    {
        return \config('nativephp-worker.queues')
            ?? \env('NATIVEPHP_QUEUE_NAMES')
            ?? 'default';
    }

    /**
     * Get the number of worker threads (enforces hard cap).
     * Default: 2 (Android), 1 (iOS background)
     */
    public static function workerCount(): int
    {
        $default = self::isIos() ? 1 : 2;

        $count = (int) \config('nativephp-worker.worker_count',
            \env('NATIVEPHP_WORKER_COUNT', $default));

        $cap = (int) \config('nativephp-worker.max_worker_count', self::MAX_WORKER_HARD_CAP);

        if ($count > $cap) {
            \Log::warning("[NativePHP Worker] worker_count={$count} exceeds hard cap of {$cap}; clamping to {$cap}");
            $count = $cap;
        }

        return max(1, $count);
    }

    /**
     * Get the supervisor mode.
     * 0 = all (queue + scheduler), 1 = queue only, 2 = scheduler only
     */
    public static function mode(): int
    {
        return (int) \config('nativephp-worker.mode',
            \env('NATIVEPHP_WORKER_MODE', 0));
    }

    /**
     * Get the scheduler interval in seconds.
     * Default: 60 (1 minute)
     */
    public static function schedulerIntervalSeconds(): int
    {
        return (int) \config('nativephp-worker.scheduler_interval',
            \env('NATIVEPHP_SCHEDULER_INTERVAL', 60));
    }

    /**
     * Get the queue poll interval in seconds.
     * Default: 5
     */
    public static function queuePollIntervalSeconds(): int
    {
        return (int) \config('nativephp-worker.queue_poll_interval',
            \env('NATIVEPHP_QUEUE_POLL_INTERVAL', 5));
    }

    /**
     * Get the maximum runtime per background window (seconds), per-platform.
     * Android: 6 hours (foreground service limit for dataSync in Android 14).
     * iOS: 25 seconds (BGProcessingTask with 5s safety margin).
     */
    public static function maxBackgroundRuntimeSeconds(): int
    {
        if (self::isIos()) {
            return (int) \config('nativephp-worker.max_background_runtime_ios',
                \env('NATIVEPHP_MAX_BG_RUNTIME_IOS', 25));
        }

        return (int) \config('nativephp-worker.max_background_runtime_android',
            \env('NATIVEPHP_MAX_BG_RUNTIME_ANDROID', 6 * 60 * 60));
    }

    /**
     * Whether workers should auto-start when the app starts.
     * Default: true if running natively
     */
    public static function autoStart(): bool
    {
        return (bool) \config('nativephp-worker.auto_start',
            \env('NATIVEPHP_WORKER_AUTO_START', self::isNative()));
    }

    /**
     * Whether to override the 'sync' queue driver with 'database'.
     */
    public static function overrideSyncDriver(): bool
    {
        return (bool) \config('nativephp-worker.override_sync_driver',
            \env('NATIVEPHP_OVERRIDE_SYNC_DRIVER', true));
    }

    /**
     * Circuit breaker: max consecutive crashes before thread backoff.
     */
    public static function circuitBreakerThreshold(): int
    {
        return (int) \config('nativephp-worker.circuit_breaker_threshold',
            \env('NATIVEPHP_CIRCUIT_BREAKER_THRESHOLD', 3));
    }

    /**
     * Circuit breaker: base backoff seconds (doubles per crash).
     */
    public static function circuitBreakerBackoff(): int
    {
        return (int) \config('nativephp-worker.circuit_breaker_backoff',
            \env('NATIVEPHP_CIRCUIT_BREAKER_BACKOFF', 5));
    }

    /**
     * Memory limit per worker request.
     */
    public static function memoryLimit(): string
    {
        return (string) \config('nativephp-worker.memory_limit',
            \env('NATIVEPHP_WORKER_MEMORY_LIMIT', '512M'));
    }

    /**
     * Whether structured worker logging is enabled.
     */
    public static function logEnabled(): bool
    {
        return (bool) \config('nativephp-worker.log_enabled',
            \env('NATIVEPHP_WORKER_LOG_ENABLED', true));
    }

    /**
     * Whether immediate dispatch wake is enabled in foreground.
     */
    public static function immediateDispatch(): bool
    {
        return (bool) \config('nativephp-worker.immediate_dispatch',
            \env('NATIVEPHP_WORKER_IMMEDIATE_DISPATCH', true));
    }

    /**
     * Whether OPcache shared memory is enabled for worker threads.
     */
    public static function opcacheEnabled(): bool
    {
        return (bool) \config('nativephp-worker.opcache_enabled',
            \env('NATIVEPHP_OPCACHE_ENABLED', true));
    }

    /**
     * OPcache shared memory size in MB.
     */
    public static function opcacheMemoryMb(): int
    {
        return (int) \config('nativephp-worker.opcache_memory_mb',
            \env('NATIVEPHP_OPCACHE_MEMORY_MB', 32));
    }

    /**
     * Whether Redis queue backend is enabled.
     */
    public static function redisEnabled(): bool
    {
        return (bool) \config('nativephp-worker.redis_enabled',
            \env('NATIVEPHP_REDIS_ENABLED', false));
    }

    /**
     * Redis connection name for queue backend.
     */
    public static function redisConnection(): string
    {
        return (string) \config('nativephp-worker.redis_connection',
            \env('NATIVEPHP_REDIS_CONNECTION', 'default'));
    }

    /**
     * Whether build-time config cache is enabled.
     */
    public static function configCacheEnabled(): bool
    {
        return (bool) \config('nativephp-worker.config_cache_enabled',
            \env('NATIVEPHP_CONFIG_CACHE_ENABLED', true));
    }

    /**
     * Check if we're running on a native platform.
     */
    public static function isNative(): bool
    {
        $value = \env('NATIVEPHP_RUNNING', false);

        if (is_bool($value)) {
            return $value;
        }

        return in_array(strtolower((string) $value), ['1', 'true', 'yes', 'on'], true);
    }

    /**
     * Check if running on iOS.
     */
    public static function isIos(): bool
    {
        return strtolower((string) \env('NATIVEPHP_PLATFORM', '')) === 'ios';
    }

    /**
     * Check if running on Android.
     */
    public static function isAndroid(): bool
    {
        return strtolower((string) \env('NATIVEPHP_PLATFORM', '')) === 'android';
    }
}
