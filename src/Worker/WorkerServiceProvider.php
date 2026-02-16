<?php

namespace Native\Mobile\Worker;

use Illuminate\Support\Facades\Artisan;
use Illuminate\Support\Facades\Schema;
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
        if ($this->isWorkerOrSchedulerLane()) {
            $this->ensureQueueTablesExist();

            // Pre-cache config for faster worker bootstrap
            $this->ensureConfigCached();
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
                    $event->connection->statement('PRAGMA journal_mode=WAL');
                    $event->connection->statement('PRAGMA busy_timeout=5000');
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
     * Auto-create the jobs and failed_jobs tables if they don't exist.
     * This fulfills the "zero-edit" requirement — developers don't need
     * to remember to run queue:table and queue:failed-table migrations.
     */
    protected function ensureQueueTablesExist(): void
    {
        // Only for database queue driver
        if (! in_array(config('queue.default'), ['database', 'sync'])) {
            return;
        }

        try {
            if (! Schema::hasTable('jobs')) {
                Schema::create('jobs', function ($table) {
                    $table->id();
                    $table->string('queue')->index();
                    $table->longText('payload');
                    $table->unsignedTinyInteger('attempts');
                    $table->unsignedInteger('reserved_at')->nullable();
                    $table->unsignedInteger('available_at');
                    $table->unsignedInteger('created_at');
                });
            }

            if (! Schema::hasTable('failed_jobs')) {
                Schema::create('failed_jobs', function ($table) {
                    $table->id();
                    $table->string('uuid')->unique();
                    $table->text('connection');
                    $table->text('queue');
                    $table->longText('payload');
                    $table->longText('exception');
                    $table->timestamp('failed_at')->useCurrent();
                });
            }

            if (! Schema::hasTable('job_batches')) {
                Schema::create('job_batches', function ($table) {
                    $table->string('id')->primary();
                    $table->string('name');
                    $table->integer('total_jobs');
                    $table->integer('pending_jobs');
                    $table->integer('failed_jobs');
                    $table->longText('failed_job_ids');
                    $table->mediumText('options')->nullable();
                    $table->integer('cancelled_at')->nullable();
                    $table->integer('created_at');
                    $table->integer('finished_at')->nullable();
                });
            }
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
    }

    /**
     * Ensure Laravel config is cached for faster worker bootstrap.
     *
     * Worker threads boot the Laravel application on every job. By pre-caching
     * the merged config, we avoid re-parsing ~30+ config files per boot cycle.
     * The cache is stored in bootstrap/cache/config.php (standard Laravel location).
     */
    protected function ensureConfigCached(): void
    {
        if (! WorkerConfig::configCacheEnabled()) {
            return;
        }

        $cachePath = $this->app->getCachedConfigPath();

        // Only cache if not already cached
        if (file_exists($cachePath)) {
            return;
        }

        try {
            Artisan::call('config:cache');
        } catch (\Throwable $e) {
            // Non-fatal: config caching is an optimization, not a requirement.
            // May fail during initial bootstrap before all providers are registered.
        }
    }

}
