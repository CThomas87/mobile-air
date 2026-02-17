# Laravel Integration — Background Workers

This document explains how the NativePHP mobile concurrent runtime integrates with
Laravel, and what (if any) changes are needed in your Laravel application.

---

## Zero-Code Integration

The worker system requires **zero changes** to your Laravel app code.
Everything is automatically configured via `WorkerServiceProvider`, which is
auto-discovered through Composer.

### What Happens Automatically

1. **Queue driver promoted to `database`**
   If your app uses the `sync` driver and `override_sync_driver` is enabled
   (default: true), the provider switches it to `database` so jobs are persisted
   and picked up by background worker threads. Escape hatch:
   `NATIVEPHP_OVERRIDE_SYNC_DRIVER=false`.

2. **Queue tables auto-created**
   The provider auto-creates `jobs`, `failed_jobs`, and `job_batches` tables
   using `CREATE TABLE IF NOT EXISTS`. No migrations needed.

3. **Worker error table auto-created**
   A `worker_errors` table is auto-created for the `WorkerErrorReporter` to
   store structured failure data (exception class, message, trace, thread ID,
   memory usage) for UI-side diagnostics.

4. **SQLite WAL mode enabled**
   On every SQLite connection establishment, the provider configures:
   `busy_timeout=5000`, `journal_mode=WAL`, `synchronous=NORMAL`,
   `wal_autocheckpoint=100`, `cache_size=-8000`. This enables concurrent
   read/write from multiple worker threads.

5. **OPcache configured**
   When OPcache is available and enabled (default), the provider sets INI values
   for shared memory size, interned strings buffer, file cache path,
   and disables timestamp validation (immutable on device).

6. **Config cached on first boot**
   A lightweight config cache is generated under `flock()` serialization on the
   first worker boot — no fresh Application instantiation needed.

7. **Worker configuration loaded from environment**
   All settings are read from `config/nativephp-worker.php` with sensible
   defaults and environment variable overrides.

---

## How Jobs Are Processed

```
Your Laravel Code                   NativePHP Runtime
─────────────────                   ──────────────────
dispatch(new MyJob)
    │
    ▼
jobs table (SQLite WAL)  ◄──────── queue_worker.php reads from here
                                        │
                                        ▼
                                   Pop ONE job
                                   Execute job->handle()
                                   Delete from table on success
                                   WorkerErrorReporter::capture() on failure
                                   Output JSON result
                                        │
                                        ▼
                                   Supervisor collects result
                                   Enqueues next poll
```

Each `queue_worker.php` invocation:

1. Bootstraps Laravel via `common.php` (cached config, no HTTP overhead)
2. Connects to the configured queue on the configured connection
3. Pops exactly ONE job
4. Executes it inside a full PHP request lifecycle (`php_request_startup`/`shutdown`)
5. Outputs a JSON result: `{processed: bool, job_name: string, error: string|null, duration_ms: int}`
6. The thread is returned to the pool — the next poll starts a fresh invocation

### Why One Job Per Invocation?

Mobile PHP runs inside the Embed SAPI with process-scoped resources. Executing
one job per request cycle provides:

- **Clean state**: No memory leaks accumulating across jobs
- **Isolation**: A fatal error in one job doesn't kill the worker thread
- **Predictable memory**: Each job starts fresh with a 512M limit (configurable)
- **Cancellation**: The cooperative cancellation flag can be checked between jobs

---

## Scheduler Integration

```
                                   NativePHP Runtime
                                   ──────────────────
                                   scheduler_tick.php (every 60s)
                                        │
                                        ▼
                                   Bootstrap Laravel via common.php
                                   Kernel::call('schedule:run',
                                       ['--no-ansi', '--quiet'],
                                       NullOutput)
                                   Kernel::terminate()
                                        │
                                        ▼
                                   JSON: {ran, output, error,
                                          duration_ms, exit_code}
```

- The scheduler tick runs `schedule:run` exactly as `php artisan schedule:run`
- It executes inside a `SchedulerGate` — only ONE tick at a time, never concurrent
- The bootstrap pre-sets `Terminal::$width`/`$height` via Reflection and uses
  `NullOutput` to prevent `fork()` deadlock from `stty` detection
- Your existing `routes/console.php` or `app/Console/Kernel.php` works unchanged

---

## Queue Driver Configuration

### Automatic (recommended)

The `WorkerServiceProvider` automatically:

- Sets `QUEUE_CONNECTION=database` if it's currently `sync`
- Ensures `retry_after=90` on the configured connection
- Auto-creates queue tables via SQLite `CREATE TABLE IF NOT EXISTS`

This works out of the box with Laravel's default `config/queue.php`.

### Redis (optional)

If your app bundles a Redis client, enable Redis queue backend:

```env
NATIVEPHP_REDIS_ENABLED=true
NATIVEPHP_REDIS_CONNECTION=default
```

The provider creates a `redis-native` queue connection and sets it as default.
**Note:** Redis requires network connectivity — SQLite/database is recommended
for offline-first mobile apps.

### Manual (optional)

If you have a custom queue configuration, ensure your `config/queue.php` has:

```php
'connections' => [
    'database' => [
        'driver' => 'database',
        'connection' => env('DB_CONNECTION', 'sqlite'),
        'table' => env('QUEUE_TABLE', 'jobs'),
        'queue' => env('QUEUE_QUEUE', 'default'),
        'retry_after' => env('QUEUE_RETRY_AFTER', 90),
        'after_commit' => false,
    ],
],
```

---

## SQLite WAL Mode

SQLite's default journal mode (`DELETE`) uses a file-level lock for writes, which
means only one connection can write at a time. With concurrent worker threads, this
causes `SQLITE_BUSY` errors.

**WAL (Write-Ahead Logging)** allows one writer and multiple readers simultaneously:

- Writer appends to a WAL file instead of modifying the database directly
- Readers see the last committed state, even while a write is in progress
- Dramatically reduces lock contention for our use case

The `WorkerServiceProvider` enables WAL automatically on every connection
establishment event, not just once at boot. The PRAGMA order is:

```php
PRAGMA busy_timeout=5000;    // Set FIRST so WAL upgrade can retry
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA wal_autocheckpoint=100;
PRAGMA cache_size=-8000;     // 8MB page cache
```

No action needed from you. To verify:

```php
$mode = DB::select('PRAGMA journal_mode')[0]->journal_mode;
// Should return "wal"
```

---

## Environment Variables

All worker settings are configurable via environment variables in your `.env` file.
Values are read by `WorkerConfig` from `config/nativephp-worker.php`:

| Variable                                 | Default                  | Description                                         |
| ---------------------------------------- | ------------------------ | --------------------------------------------------- |
| `NATIVEPHP_QUEUE_CONNECTION`             | app default              | Queue connection name                               |
| `NATIVEPHP_QUEUE_NAMES`                  | `default`                | Comma-separated queue names                         |
| `NATIVEPHP_WORKER_COUNT`                 | `2` (Android), `1` (iOS) | Concurrent worker threads (hard cap: 4)             |
| `NATIVEPHP_WORKER_MODE`                  | `0`                      | `0`=all, `1`=queue only, `2`=scheduler only         |
| `NATIVEPHP_SCHEDULER_INTERVAL`           | `60`                     | Seconds between scheduler ticks                     |
| `NATIVEPHP_QUEUE_POLL_INTERVAL`          | `5`                      | Seconds between queue polls                         |
| `NATIVEPHP_WORKER_MEMORY_LIMIT`          | `512M`                   | PHP memory_limit per worker request                 |
| `NATIVEPHP_CIRCUIT_BREAKER_THRESHOLD`    | `3`                      | Consecutive failures before backoff                 |
| `NATIVEPHP_CIRCUIT_BREAKER_BACKOFF`      | `5`                      | Base backoff seconds (doubles each crash)           |
| `NATIVEPHP_OVERRIDE_SYNC_DRIVER`         | `true`                   | Auto-switch sync to database driver                 |
| `NATIVEPHP_WORKER_IMMEDIATE_DISPATCH`    | `true`                   | Wake pool on foreground dispatch (<500ms latency)   |
| `NATIVEPHP_OPCACHE_ENABLED`              | `true`                   | Enable OPcache shared memory                        |
| `NATIVEPHP_OPCACHE_MEMORY_MB`            | `32`                     | OPcache SHM size in MB                              |
| `NATIVEPHP_OPCACHE_FILE_CACHE`           | `true`                   | Persist bytecode to disk (reduces cold boot 30-50%) |
| `NATIVEPHP_WORKER_LOG_ENABLED`           | `true`                   | Enable structured worker.jsonl log                  |
| `NATIVEPHP_WORKER_LOG_MAX_SIZE`          | `1024`                   | Max log size in KB before rotation                  |
| `NATIVEPHP_WORKER_AUTO_START`            | auto-detect              | Auto-start workers on app launch                    |
| `NATIVEPHP_ANDROID_EXECUTION_STRATEGY`   | `auto`                   | `auto`/`foreground`/`workmanager`                   |
| `NATIVEPHP_WORKMANAGER_INTERVAL_MINUTES` | `15`                     | WorkManager repeat interval (min 15)                |
| `NATIVEPHP_REDIS_ENABLED`                | `false`                  | Use Redis instead of SQLite for queue               |
| `NATIVEPHP_REDIS_CONNECTION`             | `default`                | Redis connection name                               |
| `NATIVEPHP_CONFIG_CACHE_ENABLED`         | `true`                   | Lightweight config cache on first boot              |
| `NATIVEPHP_DB_POOL_SIZE`                 | `4`                      | Pre-opened SQLite connections                       |
| `NATIVEPHP_MAX_BG_RUNTIME_ANDROID`       | `21600` (6h)             | Android foreground service time limit               |
| `NATIVEPHP_MAX_BG_RUNTIME_IOS`           | `25`                     | iOS BGProcessingTask time limit                     |

### Example .env

```env
# Process multiple queues with priority
NATIVEPHP_QUEUE_NAMES=high,default,low

# Use 1 worker on resource-constrained device
NATIVEPHP_WORKER_COUNT=1

# Disable scheduler if you don't use scheduled commands
NATIVEPHP_WORKER_MODE=1

# Increase circuit breaker tolerance for flaky networks
NATIVEPHP_CIRCUIT_BREAKER_THRESHOLD=5
NATIVEPHP_CIRCUIT_BREAKER_BACKOFF=10

# Use WorkManager for battery-friendly periodic processing
NATIVEPHP_ANDROID_EXECUTION_STRATEGY=workmanager
```

---

## Dispatching Jobs

Use standard Laravel job dispatching. No changes needed:

```php
use App\Jobs\ProcessPhoto;

// Dispatch to default queue
ProcessPhoto::dispatch($photo);

// Dispatch to a specific queue
ProcessPhoto::dispatch($photo)->onQueue('high');

// Dispatch with delay
ProcessPhoto::dispatch($photo)->delay(now()->addMinutes(5));

// Dispatch chain
Bus::chain([
    new OptimizePhoto($photo),
    new GenerateThumbnail($photo),
    new NotifyUser($user),
])->dispatch();
```

### Job Best Practices for Mobile

1. **Keep jobs short**: Aim for < 10 seconds per job. On iOS, BGProcessingTask
   windows are ~25 seconds — long jobs may be interrupted.

2. **Make jobs idempotent**: Jobs may be retried if the app is killed mid-execution.
   The `database` driver auto-retries jobs whose `reserved_at` exceeds `retry_after`
   (default 90 seconds).

3. **Use small payloads**: Jobs are stored in SQLite. Avoid serializing large objects.

4. **Set reasonable retry limits**:

   ```php
   public $tries = 3;
   public $maxExceptions = 2;
   ```

5. **Handle failures gracefully**:

   ```php
   public function failed(\Throwable $exception): void
   {
       // Log, notify, clean up — errors also captured in
       // worker_errors table by WorkerErrorReporter
   }
   ```

6. **Never call `fork()`/`proc_open()`/`exec()`/`shell_exec()`** — these
   deadlock in a multithreaded process. Use Laravel's HTTP client or
   native bridges instead of shell commands.

---

## Scheduling Commands

Use the standard Laravel scheduler. Your `routes/console.php` or
`app/Console/Kernel.php` works unchanged:

```php
// routes/console.php (Laravel 11+)
use Illuminate\Support\Facades\Schedule;

Schedule::command('sync:data')->hourly();
Schedule::job(new CleanupOldRecords)->daily();
Schedule::call(fn () => cache()->flush())->weekly();
```

### Scheduler Considerations for Mobile

1. **Minimum resolution is 60 seconds** — the scheduler tick runs once per minute,
   matching Laravel's native `schedule:run` behavior. Configurable via
   `NATIVEPHP_SCHEDULER_INTERVAL`.

2. **On iOS**, the scheduler only runs during BGProcessingTask windows. Commands
   scheduled `everyMinute()` will NOT actually run every minute — they'll run
   once per BGProcessingTask window (every 15-60+ minutes).

3. **Use `withoutOverlapping()`** for long-running commands:

   ```php
   Schedule::command('reports:generate')
       ->hourly()
       ->withoutOverlapping();
   ```

4. **Avoid `runInBackground()`** — mobile has no shell process management.
   This flag causes `proc_open()` which deadlocks in a multithreaded process.

5. **Avoid commands that spawn subprocesses** — any `exec()`, `proc_open()`,
   `shell_exec()` in scheduled commands will deadlock because `fork()` is
   unsafe in a multithreaded process.

---

## Failed Jobs

Failed jobs are handled automatically:

- Stored in the `failed_jobs` table (auto-created, no manual migration needed)
- Captured by `WorkerErrorReporter` with full context (exception class, message,
  file, line, trace, thread ID, memory usage)
- Visible via `WorkerMetrics::dashboard()` and the worker dashboard component

You can retry failed jobs programmatically:

```php
use Illuminate\Support\Facades\Artisan;

// Retry all failed jobs
Artisan::call('queue:retry', ['id' => 'all']);

// Retry a specific job
Artisan::call('queue:retry', ['id' => $failedJobId]);
```

---

## Error Reporting

The `WorkerErrorReporter` captures all worker failures into a dedicated
`worker_errors` SQLite table. This runs automatically — both `queue_worker.php`
and `scheduler_tick.php` call `WorkerErrorReporter::capture()` on exceptions.

### Error Table Schema

| Column         | Type     | Description                                |
| -------------- | -------- | ------------------------------------------ |
| `worker_id`    | int      | Native worker thread ID                    |
| `job_type`     | text     | `queue` or `scheduler`                     |
| `queue`        | text     | Queue name (null for scheduler)            |
| `job_class`    | text     | Fully qualified job class name             |
| `job_id`       | text     | Job UUID                                   |
| `error_class`  | text     | Exception class name                       |
| `message`      | text     | Error message (max 1024 chars)             |
| `file`         | text     | Source file where error occurred           |
| `line`         | int      | Line number                                |
| `trace`        | text     | Stack trace (max 4096 chars)               |
| `php_thread`   | int      | ZTS thread ID (`zend_thread_id()`)         |
| `memory_usage` | int      | `memory_get_peak_usage()` at time of error |
| `created_at`   | datetime | When the error was captured                |

### Querying Errors

```php
use Native\Mobile\Worker\WorkerErrorReporter;

// Get recent errors
$errors = WorkerErrorReporter::recent(20);

// Get recent errors filtered by queue
$errors = WorkerErrorReporter::recent(10, 'high');

// Get error summary (counts by error class)
$summary = WorkerErrorReporter::summary();

// Count errors in the last N minutes
$count = WorkerErrorReporter::count(withinMinutes: 30);

// Prune old errors
WorkerErrorReporter::pruneOlderThan(days: 7);

// Clear all errors
WorkerErrorReporter::flush();
```

---

## Monitoring

### WorkerMetrics

`WorkerMetrics` provides dashboard data from two sources:

1. **Native bridge** (`nativephp_supervisor_status()`) — Live stats when running inside the native wrapper
2. **PHP-side fallback** — Queries DB and config when bridge unavailable

```php
use Native\Mobile\Worker\WorkerMetrics;

// Live supervisor snapshot
$snapshot = WorkerMetrics::snapshot();
// → [status, activeJobs, pendingJobs, completedJobs, failedJobs,
//    failureRatePercent, schedulerRunning, uptimeSeconds, workerCount,
//    circuitBreakerMax, sqlitePool, memoryLimit]

// Per-queue depths
$queues = WorkerMetrics::queueStatus();
// → [default: [pending: 5, reserved: 1], high: [pending: 0, reserved: 0]]

// Recent errors from worker.jsonl log
$errors = WorkerMetrics::recentErrors(10);

// Composite dashboard data
$dashboard = WorkerMetrics::dashboard();
// → [supervisor: {...}, queues: {...}, recentErrors: [...], dbPool: {...}]

// Throughput over sliding window
$jpm = WorkerMetrics::throughput(5);
// → 12.4 (jobs per minute over last 5 minutes)
```

### System Facade

The `System` facade provides the PHP API for worker management:

```php
use Native\Mobile\Facades\System;

// Start/stop background workers
System::startBackgroundWorker();
System::stopBackgroundWorker();

// Get worker status
$status = System::workerStatus();
// → ['status' => 'running', 'activeJobs' => 1, ...]

// Cancel a specific job
System::cancelJob($jobId);

// Request battery exemption (Android — shows system dialog)
System::requestBatteryExemption();

// Push data from worker to WebView
System::pushToWebView('job.completed', ['id' => $jobId]);

// Check if currently in worker context
if (System::isWorkerContext()) {
    // Running inside a queue worker or scheduler thread
}
```

### Worker Status API

A built-in API endpoint is available at `/_native/api/worker/status`:

```json
GET /_native/api/worker/status

{
  "native": { "status": "running", "activeJobs": 1, "completedJobs": 47, ... },
  "config": {
    "worker_count": 2,
    "circuit_breaker_threshold": 3,
    "memory_limit": "512M",
    "mode": 0,
    "queues": "default"
  },
  "queueStats": {
    "pending_jobs": 5,
    "failed_jobs": 0
  }
}
```

### Dashboard Blade Component

A pre-built Alpine.js dashboard component is available:

```blade
<x-nativephp-worker-dashboard />
```

Features:

- Auto-polls status every 5 seconds
- Status badge (running/stopped/error)
- Stats grid: active, pending, completed, failed jobs
- Uptime display
- Scheduler state indicator
- Database queue stats (pending/failed counts)
- Configuration summary

---

## Worker Events

The runtime fires Laravel events that you can listen to:

```php
use Native\Mobile\Events\Worker\JobCompleted;
use Native\Mobile\Events\Worker\JobFailed;
use Native\Mobile\Events\Worker\WorkerStarted;
use Native\Mobile\Events\Worker\WorkerStopped;
use Native\Mobile\Events\Worker\CircuitBreakerTripped;
use Native\Mobile\Events\Worker\SchedulerTickCompleted;
```

### Event Payloads

| Event                    | Properties                                                  |
| ------------------------ | ----------------------------------------------------------- |
| `WorkerStarted`          | `workerCount`, `queues`, `mode`, `platform`, `isForeground` |
| `WorkerStopped`          | `completedJobs`, `failedJobs`, `uptimeSeconds`, `reason`    |
| `JobCompleted`           | `jobId`, `jobName`, `durationMs`, `stdout`                  |
| `JobFailed`              | `jobId`, `jobName`, `durationMs`, `error`, `stderr`         |
| `CircuitBreakerTripped`  | `consecutiveCrashes`, `backoffSeconds`, `lastError`         |
| `SchedulerTickCompleted` | `durationMs`, `exitCode`                                    |

### Example Listener

```php
use Native\Mobile\Events\Worker\CircuitBreakerTripped;

class HandleCircuitBreaker
{
    public function handle(CircuitBreakerTripped $event): void
    {
        Log::warning("Circuit breaker tripped after {$event->consecutiveCrashes} failures", [
            'backoff' => $event->backoffSeconds,
            'last_error' => $event->lastError,
        ]);

        // Optionally notify the user via the WebView
        System::pushToWebView('worker.circuit_breaker', [
            'crashes' => $event->consecutiveCrashes,
        ]);
    }
}
```

---

## Development Tools

### native:worker Command

A local development simulator for the worker system:

```bash
# Simulate full worker (queue + scheduler)
php artisan native:worker

# Queue processing only
php artisan native:worker --queue-only

# Scheduler only
php artisan native:worker --scheduler-only

# Check worker status
php artisan native:worker --status

# Set worker count
php artisan native:worker --workers=2

# Process specific queues
php artisan native:worker --queues=high,default
```

This runs locally on your development machine, simulating the native
supervisor. Use it to test queue jobs and scheduled commands before deploying.

### native:tail Command

Tail the structured worker log in development:

```bash
php artisan native:tail --type=worker
```

Streams JSON-line events from the device via ADB. Output is color-coded:

- **Green (info)**: Successful job completions, scheduler ticks
- **Red (error)**: Job failures, circuit breaker trips
- **Yellow (comment)**: Warnings, timeouts

---

## Android Execution Strategies

The `NATIVEPHP_ANDROID_EXECUTION_STRATEGY` setting controls how background work runs:

| Strategy         | Implementation                  | Behavior                                                                  |
| ---------------- | ------------------------------- | ------------------------------------------------------------------------- |
| `auto` (default) | Auto-detect per API level       | WorkManager on Android 14+ (API 34+), FGS on older                        |
| `foreground`     | PhpWorkerService (FGS)          | Persistent notification, 6h limit on Android 14+, <500ms dispatch latency |
| `workmanager`    | PhpPeriodicWorker (WorkManager) | Battery-friendly, 15min minimum interval, system-managed scheduling       |

**Foreground Service** is better for real-time queue processing with low latency.
**WorkManager** is better for periodic/batch work and battery optimization.

---

## Troubleshooting

### Jobs aren't processing

1. Check that `QUEUE_CONNECTION` is `database` (not `sync`) — the provider
   auto-promotes this unless `NATIVEPHP_OVERRIDE_SYNC_DRIVER=false`
2. Verify the `jobs` table exists (auto-created, but check with `sqlite3`)
3. Check the worker service is running (Android: persistent notification visible)
4. Check for errors: `php artisan native:tail --type=worker` or `adb logcat -s Supervisor:*`

### SQLITE_BUSY errors

1. Verify WAL mode: `PRAGMA journal_mode` should return `wal`
2. `busy_timeout` is set to 5000ms by default; increase via SQLite connection config if needed
3. Reduce `NATIVEPHP_WORKER_COUNT` to 1 for diagnosis
4. Ensure jobs don't hold long database transactions

### Jobs re-executing after app kill

1. This is expected — the `database` driver auto-retries jobs whose `reserved_at`
   exceeds `retry_after` (default 90 seconds)
2. Make jobs idempotent to handle this gracefully
3. Adjust `retry_after` in `config/queue.php` if needed

### Scheduler running commands multiple times

1. The `SchedulerGate` prevents concurrent ticks, but if the app restarts
   mid-tick, the next tick may re-run due commands
2. Use `withoutOverlapping()` on commands that shouldn't run concurrently

### Worker thread hangs permanently

1. Likely `fork()` deadlock — check for `proc_open()`/`exec()`/`shell_exec()`
   calls reachable from worker context
2. Verify `common.php` has the `Terminal::$width`/`$height` Reflection pre-set
3. Check `adb logcat -s PhpEngine:*` for hung-thread diagnostics

### OPcache not loading

1. Check logcat: `adb logcat -s PhpEngine:*` — look for `execute_ex visible at`
2. If you see `cannot locate symbol "execute_ex"`: the RTLD_GLOBAL promotion failed
3. Ensure `php_engine.c` uses `RTLD_NOW | RTLD_GLOBAL` without `RTLD_NOLOAD`
4. Verify libphp.so exports the symbol: `nm -D libphp.so | grep execute_ex`

### Worker errors not appearing in dashboard

1. Check `worker_errors` table exists (created by `WorkerErrorReporter::ensureTableExists()` during first boot)
2. Query directly: `WorkerErrorReporter::recent(20)`
3. Check the worker.jsonl log: `php artisan native:tail --type=worker`
