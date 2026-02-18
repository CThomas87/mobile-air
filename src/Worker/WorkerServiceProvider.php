<?php

namespace Native\Mobile\Worker;

use Illuminate\Support\ServiceProvider;

/**
 * WorkerServiceProvider — Drop-in Laravel integration for native PHP workers.
 *
 * This service provider is auto-discovered by Laravel and requires ZERO
 * configuration from the app developer. It:
 *
 * 1. Merges default worker configuration
 * 2. Configures the database queue driver for SQLite WAL mode
 * 3. Auto-creates queue tables if missing
 * 4. Provides optional config publishing for customization
 * 5. Caches config at build time for faster worker bootstrap
 *
 * Works automatically with vanilla Laravel queues and scheduler.
 */
class WorkerServiceProvider extends ServiceProvider
{
    /**
     * Register bindings.
     */
    public function register(): void
    {
        $this->mergeConfigFrom(
            __DIR__ . '/../../config/nativephp-worker.php',
            'nativephp-worker'
        );

        // Register WorkerConfig as a singleton for easy access
        $this->app->singleton(WorkerConfig::class, function () {
            return new WorkerConfig();
        });
    }

    /**
     * Bootstrap the worker integration.
     */
    public function boot(): void
    {
        // Only apply when running natively
        if (! WorkerConfig::isNative()) {
            return;
        }

        // Publish config (optional — not required for operation)
        $this->publishes([
            __DIR__ . '/../../config/nativephp-worker.php' => config_path('nativephp-worker.php'),
        ], 'nativephp-worker-config');

        // Configure SQLite WAL mode for database queue driver (offline-first).
        // This is the SINGLE authority for SQLite PRAGMAs — the bootstrap
        // entrypoints (queue_worker.php, scheduler_tick.php) rely on this.
        $this->configureSqliteWal();

        // Set sensible defaults for queue configuration if not explicitly set
        $this->configureQueueDefaults();

        // Configure OPcache shared memory if enabled
        $this->configureOpcache();

        // Keep HTTP page-serving lane lightweight. Heavy worker bootstrap
        // tasks should run only for headless worker/console execution.
        if (! $this->isHeadlessWorkerLane()) {
            return;
        }

        // Auto-create queue tables if they don't exist (zero-edit requirement).
        // IMPORTANT: Only do this for actual worker/scheduler lanes, NOT
        // for artisan commands like 'migrate' — otherwise we'd create the
        // jobs/failed_jobs/job_batches tables before the migration that
        // creates them runs, causing a "table already exists" error.
        //
        // Serialized with flock() because multiple worker threads boot
        // simultaneously and would otherwise race on SQLite writes and
        // config cache file writes.
        if ($this->isWorkerOrSchedulerLane()) {
            $this->serializedFirstBootTasks();
        }
    }

    /**
     * Determine if current execution is a headless worker/console lane.
     */
    protected function isHeadlessWorkerLane(): bool
    {
        $isConsoleEnv = strtolower((string) env('APP_RUNNING_IN_CONSOLE', 'false'));
        if (in_array($isConsoleEnv, ['1', 'true', 'yes', 'on'], true)) {
            return true;
        }

        $jobType = strtolower((string) env('NATIVEPHP_JOB_TYPE', ''));

        return in_array($jobType, ['queue', 'scheduler'], true);
    }

    /**
     * Determine if current execution is specifically a worker or scheduler
     * (not a general artisan command). Used to gate operations that would
     * conflict with migration commands.
     */
    protected function isWorkerOrSchedulerLane(): bool
    {
        $jobType = strtolower((string) env('NATIVEPHP_JOB_TYPE', ''));

        return in_array($jobType, ['queue', 'scheduler'], true);
    }

    /**
     * Enable WAL journal mode on SQLite connections.
     * This is critical for concurrent queue access from multiple worker threads.
     */
    protected function configureSqliteWal(): void
    {
        $connection = config('database.default', 'sqlite');
        $driver = config("database.connections.{$connection}.driver");

        if ($driver !== 'sqlite') {
            return;
        }

        // Set WAL mode, busy timeout, and synchronous mode via database connection event.
        // This handles ALL connections (HTTP, worker, scheduler) in one place.
        $this->app['events']->listen('Illuminate\Database\Events\ConnectionEstablished', function ($event) use ($connection) {
            if ($event->connectionName === $connection) {
                try {
                    // Set busy_timeout FIRST so the WAL upgrade can retry
                    // instead of returning SQLITE_BUSY immediately.
                    $event->connection->statement('PRAGMA busy_timeout=5000');
                    $event->connection->statement('PRAGMA journal_mode=WAL');
                    $event->connection->statement('PRAGMA synchronous=NORMAL');
                    $event->connection->statement('PRAGMA wal_autocheckpoint=100');
                    $event->connection->statement('PRAGMA cache_size=-8000');
                } catch (\Throwable $e) {
                    // Non-fatal: WAL may already be configured
                }
            }
        });
    }

    /**
     * Set queue configuration defaults for native mobile operation.
     */
    protected function configureQueueDefaults(): void
    {
        // If Redis queue backend is explicitly enabled, prefer it over database
        if (WorkerConfig::redisEnabled()) {
            $this->configureRedisQueue();
            return;
        }

        // Prefer database driver over sync for background processing.
        // Gated by override_sync_driver config escape hatch.
        if (config('queue.default') === 'sync'
            && WorkerConfig::autoStart()
            && WorkerConfig::overrideSyncDriver()) {
            config(['queue.default' => 'database']);
        }

        // Set retry_after to a reasonable value for mobile
        $connection = WorkerConfig::connection();
        $retryAfter = config("queue.connections.{$connection}.retry_after");
        if (! $retryAfter) {
            config(["queue.connections.{$connection}.retry_after" => 90]);
        }
    }

    /**
     * Configure Redis as queue backend when explicitly enabled.
     */
    protected function configureRedisQueue(): void
    {
        $redisConn = WorkerConfig::redisConnection();

        // Ensure a Redis queue connection exists
        if (! config("queue.connections.redis-native")) {
            config(["queue.connections.redis-native" => [
                'driver' => 'redis',
                'connection' => $redisConn,
                'queue' => WorkerConfig::queues(),
                'retry_after' => 90,
                'block_for' => null,
                'after_commit' => false,
            ]]);
        }

        config(['queue.default' => 'redis-native']);
    }

    /**
     * Serialize first-boot tasks across worker threads using flock().
     *
     * In ZTS mode, PHP static variables are per-thread, so we cannot use
     * static flags to prevent cross-thread races.  A filesystem lock
     * ensures only ONE thread performs table creation and config caching
     * at a time.
     */
    protected function serializedFirstBootTasks(): void
    {
        $lockPath = $this->app->storagePath() . '/framework/.worker_init.lock';

        // Ensure directory exists
        $dir = dirname($lockPath);
        if (! is_dir($dir)) {
            @mkdir($dir, 0775, true);
        }

        $fp = @fopen($lockPath, 'c+');
        if (! $fp) {
            // Can't acquire lock file — fall through without serialization
            $this->ensureQueueTablesExist();
            return;
        }

        try {
            if (flock($fp, LOCK_EX)) {
                try {
                    $this->ensureQueueTablesExist();
                    WorkerErrorReporter::ensureTableExists();
                    $this->ensureConfigCached();
                    NativeDbPool::instance()->warmUp();
                    $this->warmHotPaths();
                } finally {
                    flock($fp, LOCK_UN);
                }
            } else {
                // Couldn't lock — still do table creation (may race but is caught)
                $this->ensureQueueTablesExist();
            }
        } finally {
            fclose($fp);
        }
    }

    /**
     * Auto-create the jobs and failed_jobs tables if they don't exist.
     * This fulfills the "zero-edit" requirement — developers don't need
     * to remember to run queue:table and queue:failed-table migrations.
     *
     * Uses IF NOT EXISTS at the SQLite level to avoid TOCTOU races
     * when multiple worker threads boot concurrently.
     */
    protected function ensureQueueTablesExist(): void
    {
        // Only for database queue driver
        if (! in_array(config('queue.default'), ['database', 'sync'])) {
            return;
        }

        try {
            $db = $this->app->make('db')->connection();

            $db->statement('
                CREATE TABLE IF NOT EXISTS jobs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    queue TEXT NOT NULL,
                    payload TEXT NOT NULL,
                    attempts INTEGER NOT NULL DEFAULT 0,
                    reserved_at INTEGER,
                    available_at INTEGER NOT NULL,
                    created_at INTEGER NOT NULL
                )
            ');
            $db->statement('CREATE INDEX IF NOT EXISTS jobs_queue_index ON jobs (queue)');
            $db->statement('CREATE INDEX IF NOT EXISTS jobs_queue_reserved_available_index ON jobs (queue, reserved_at, available_at)');

            $db->statement('
                CREATE TABLE IF NOT EXISTS failed_jobs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    uuid TEXT NOT NULL UNIQUE,
                    connection TEXT NOT NULL,
                    queue TEXT NOT NULL,
                    payload TEXT NOT NULL,
                    exception TEXT NOT NULL,
                    failed_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
                )
            ');

            $db->statement('
                CREATE TABLE IF NOT EXISTS job_batches (
                    id TEXT PRIMARY KEY NOT NULL,
                    name TEXT NOT NULL,
                    total_jobs INTEGER NOT NULL,
                    pending_jobs INTEGER NOT NULL,
                    failed_jobs INTEGER NOT NULL,
                    failed_job_ids TEXT NOT NULL,
                    options TEXT,
                    cancelled_at INTEGER,
                    created_at INTEGER NOT NULL,
                    finished_at INTEGER
                )
            ');
        } catch (\Throwable $e) {
            // Non-fatal during early bootstrap; tables may be created by migrations later
        }
    }

    /**
     * Configure OPcache shared memory for worker threads.
     *
     * When multiple worker threads share the same compiled bytecode via OPcache's
     * shared memory, memory usage per thread drops significantly (~2-4MB savings per
     * thread for a typical Laravel app).
     */
    protected function configureOpcache(): void
    {
        if (! WorkerConfig::opcacheEnabled()) {
            return;
        }

        if (! function_exists('opcache_get_status')) {
            return;
        }

        $memoryMb = WorkerConfig::opcacheMemoryMb();

        // Set OPcache INI values — these take effect for subsequent script compilations
        // in the same process. Worker threads inherit the shared memory segment.
        @ini_set('opcache.enable', '1');
        @ini_set('opcache.enable_cli', '1'); // Required for embed SAPI
        @ini_set('opcache.memory_consumption', (string) $memoryMb);
        @ini_set('opcache.interned_strings_buffer', '8');
        @ini_set('opcache.max_accelerated_files', '4000');
        @ini_set('opcache.validate_timestamps', '0'); // Immutable on device
        @ini_set('opcache.save_comments', '1'); // Required for annotations
        @ini_set('opcache.file_update_protection', '0'); // No filesystem races on mobile

        // OPcache file cache — persist compiled bytecode to disk for faster cold boots.
        // On first boot this populates the cache; on subsequent boots, OPcache loads
        // pre-compiled bytecode from disk instead of recompiling ~200+ framework files.
        if (WorkerConfig::opcacheFileCache()) {
            $fileCachePath = WorkerConfig::opcacheFileCachePath();

            // Ensure the cache directory exists
            if (! is_dir($fileCachePath)) {
                @mkdir($fileCachePath, 0775, true);
            }

            if (is_dir($fileCachePath) && is_writable($fileCachePath)) {
                @ini_set('opcache.file_cache', $fileCachePath);
                @ini_set('opcache.file_cache_only', '0'); // Use SHM + file cache (hybrid)
                @ini_set('opcache.file_cache_consistency_checks', '0'); // Trust the cache
            }
        }
    }

    /**
     * Ensure Laravel config is cached for faster worker bootstrap.
     *
     * Full Artisan::call('config:cache') is NOT safe at runtime because it
     * creates a fresh Application → re-triggers all service providers →
     * 6+ concurrent bootstraps in ZTS mode.
     *
     * Instead we use a lightweight approach:
     *  1. Check if bootstrap/cache/config.php already exists (build-time).
     *  2. If not, generate it ONCE under the flock() guard by serializing
     *     the already-loaded config repository to a PHP return array.
     *  3. A marker file prevents re-generation on subsequent worker boots.
     *
     * This avoids spawning a fresh Application while still giving all
     * threads the benefit of a cached config on subsequent boots.
     */
    protected function ensureConfigCached(): void
    {
        $cachePath = $this->app->bootstrapPath('cache/config.php');

        // Fast path: cache already exists (from build-time or previous first-boot)
        if (file_exists($cachePath)) {
            return;
        }

        // Marker prevents repeated attempts if generation fails
        $markerPath = $this->app->storagePath('framework/.config_cache_attempted');
        if (file_exists($markerPath)) {
            return;
        }

        try {
            @file_put_contents($markerPath, (string) time());

            // Serialize the already-resolved config (no fresh Application needed)
            $config = $this->app->make('config')->all();

            $content = '<?php return ' . var_export($config, true) . ';' . PHP_EOL;

            $dir = dirname($cachePath);
            if (! is_dir($dir)) {
                @mkdir($dir, 0775, true);
            }

            // Atomic write via temp file + rename
            $tmpPath = $cachePath . '.' . getmypid() . '.tmp';
            if (@file_put_contents($tmpPath, $content) !== false) {
                @rename($tmpPath, $cachePath);
            }
        } catch (\Throwable $e) {
            // Non-fatal — workers continue with file-based config loading
        }
    }

    /**
     * Prime OPcache for frequently touched worker/bootstrap files.
     */
    protected function warmHotPaths(): void
    {
        if (! function_exists('opcache_compile_file')) {
            return;
        }

        $hotFiles = [
            base_path('vendor/autoload.php'),
            base_path('bootstrap/app.php'),
            base_path('routes/console.php'),
            base_path('vendor/nativephp/mobile/bootstrap/worker/queue_worker.php'),
            base_path('vendor/nativephp/mobile/bootstrap/worker/scheduler_tick.php'),
        ];

        foreach ($hotFiles as $file) {
            if (is_file($file)) {
                @opcache_compile_file($file);
            }
        }
    }

}
