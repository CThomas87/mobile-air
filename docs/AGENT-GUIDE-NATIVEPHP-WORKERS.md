# NativePHP Mobile Worker — AI Agent Runbook

## Purpose

This guide is for AI agents operating on the NativePHP mobile runtime.
It defines **source-of-truth files**, the **runtime architecture**, **startup
sequence**, **migration behavior**, **worker/scheduler internals**, **analytics**,
and a **safe troubleshooting workflow**.

---

## 1. Source of Truth Files

### Android Native Layer (C / Kotlin)

| File                                                                                          | Purpose                                                                                                                                                                                                                                                           |
| --------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `resources/androidstudio/app/src/main/cpp/compat/android_compat.cpp`                          | **Primary RTLD_GLOBAL preloader.** `JNI_OnLoad` loads libphp.so with `RTLD_GLOBAL` before `System.loadLibrary("php")`. Also provides `getdtablesize()` and `copy_file_range()` Bionic polyfills.                                                                  |
| `resources/androidstudio/app/src/main/cpp/php_engine.c` / `.h`                                | Process-wide PHP engine singleton (1188 lines). `php_engine_init()` with RTLD_GLOBAL probe, OPcache dlopen, `php_embed_init()`, OPcache warmup, TLS-routed output. Declares `fix_opcache_tls_cache()`, `fix_opcache_per_thread_state()`.                          |
| `resources/androidstudio/app/src/main/cpp/worker_pool.c`                                      | Native pthread pool (1–4 workers). Condvar-based job queue, circuit breaker, staggered starts, UI-lane priority yielding.                                                                                                                                         |
| `resources/androidstudio/app/src/main/cpp/supervisor.c`                                       | Top-level orchestrator. Owns engine + pool + gate lifecycle, structured JSON logging, `supervisor_status_json()`. Default memory limit: `256M`.                                                                                                                   |
| `resources/androidstudio/app/src/main/cpp/scheduler_gate.c`                                   | Mutex ensuring `schedule:run` never overlaps (atomic CAS, reject-not-queue).                                                                                                                                                                                      |
| `resources/androidstudio/app/src/main/cpp/php_request_context.c` / `.h`                       | Per-job isolated execution context (1038 lines). `php_request_create()`, `php_request_execute()`, `php_request_destroy()`. Per-thread output buffers, cooperative cancellation (Zend VM interrupt), HTTP `$_SERVER` injection, status/type enums, priority field. |
| `resources/androidstudio/app/src/main/cpp/php_thread_context.c`                               | Per-thread TSRM attach/detach (`ts_resource(0)` + `TSRMLS_CACHE_UPDATE()`). Defines `TSRMLS_CACHE_DEFINE()` to avoid emutls descriptor conflicts.                                                                                                                 |
| `resources/androidstudio/app/src/main/cpp/bridge_jni.cpp`                                     | JNI bridge for `BridgeRouterKt` can/call API. Caches `nativePHPCan`/`nativePHPCall` method IDs. Replaces the former `php_bridge.c`.                                                                                                                               |
| `resources/androidstudio/app/src/main/cpp/libphp_wrapper.cpp`                                 | libphp.so dlopen wrapper with RTLD_GLOBAL. Re-opens `libphp.so`, `libcompat.so`, and `libphp_wrapper.so` with `RTLD_GLOBAL` via C++ constructor attributes.                                                                                                       |
| `resources/androidstudio/app/src/main/java/com/nativephp/mobile/worker/PhpWorkerService.kt`   | Android Foreground Service (FGS type `dataSync`). WakeLock, polling loops, `supervisor_*` JNI calls. Default queues: `high,default,low`.                                                                                                                          |
| `resources/androidstudio/app/src/main/java/com/nativephp/mobile/bridge/LaravelEnvironment.kt` | App bootstrap: directory creation, OTA update, env vars, migrations, engine init.                                                                                                                                                                                 |

### iOS Native Layer

| File                                                           | Purpose                                                                                  |
| -------------------------------------------------------------- | ---------------------------------------------------------------------------------------- |
| `resources/xcode/NativePHP/NativePHPApp.swift`                 | App entry point, iOS lane bootstrap.                                                     |
| `resources/xcode/NativePHP/Worker/PhpSupervisor.swift`         | In-process supervisor for iOS.                                                           |
| `resources/xcode/NativePHP/Worker/BackgroundTaskManager.swift` | BGProcessingTask registration and scheduling (25s windows).                              |
| `src/Worker/IosWorkerScheduler.php`                            | iOS-specific work window: processes queue jobs within 25s deadline, runs scheduler tick. |

### PHP Framework Layer

| File                                   | Purpose                                                                                                                          |
| -------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `src/Worker/WorkerServiceProvider.php` | Auto-discovered by Laravel. Merges config, SQLite WAL, auto-creates queue/error tables, OPcache setup, lightweight config cache. |
| `src/Worker/WorkerConfig.php`          | Static config reader. All settings via env vars or `nativephp-worker.php`. Hard cap of 4 worker threads.                         |
| `src/Worker/WorkerMetrics.php`         | Dashboard data: live supervisor snapshot, queue status, recent errors, throughput calculation.                                   |
| `src/Worker/WorkerErrorReporter.php`   | Captures worker failures into `worker_errors` SQLite table for UI-side diagnosis.                                                |
| `src/Worker/NativeDbPool.php`          | PHP-side SQLite connection pool: `acquire()`/`release()`, `warmUp()`, `drain()`, `stats()`.                                      |
| `config/nativephp-worker.php`          | Published config file with all worker settings and documentation.                                                                |

### Bootstrap Entrypoints

| File                                  | Purpose                                                                                                                                                                     |
| ------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `bootstrap/worker/common.php`         | Shared bootstrap: autoloader, path detection, directory creation, fork() deadlock prevention (Terminal Reflection), thread-safe `$_SERVER`/`$_ENV` setup, kernel bootstrap. |
| `bootstrap/worker/queue_worker.php`   | Pop and execute ONE queue job. Outputs JSON result: `{processed, job_name, error, duration_ms}`.                                                                            |
| `bootstrap/worker/scheduler_tick.php` | Run `schedule:run` once with NullOutput. Outputs JSON: `{ran, output, error, duration_ms, exit_code}`.                                                                      |

### Events

| Event class                            | Fired when                                                                                       |
| -------------------------------------- | ------------------------------------------------------------------------------------------------ |
| `Events\Worker\WorkerStarted`          | Supervisor starts; properties: `workerCount`, `queues`, `mode`, `platform`, `isForeground`       |
| `Events\Worker\WorkerStopped`          | Supervisor stops; properties: `completedJobs`, `failedJobs`, `uptimeSeconds`, `reason`           |
| `Events\Worker\JobCompleted`           | Queue job succeeds; properties: `jobId`, `jobName`, `durationMs`, `stdout`                       |
| `Events\Worker\JobFailed`              | Queue job fails; properties: `jobId`, `jobName`, `durationMs`, `error`, `stderr`                 |
| `Events\Worker\CircuitBreakerTripped`  | Consecutive crash threshold hit; properties: `consecutiveCrashes`, `backoffSeconds`, `lastError` |
| `Events\Worker\SchedulerTickCompleted` | schedule:run finishes; properties: `durationMs`, `exitCode`                                      |

### API & UI

| File                                                    | Purpose                                                                                                                         |
| ------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `routes/api.php`                                        | Registers `GET /_native/api/worker/status` → `WorkerStatusController`                                                           |
| `src/Http/Controllers/WorkerStatusController.php`       | Returns supervisor status + config + DB queue stats as JSON                                                                     |
| `resources/views/components/worker-dashboard.blade.php` | Alpine.js dashboard: polls status every 5s, shows active/pending/completed/failed jobs, uptime, scheduler state, config summary |

### Commands

| Command                                 | Purpose                                                                                            |
| --------------------------------------- | -------------------------------------------------------------------------------------------------- |
| `php artisan native:worker`             | Local dev simulator. `--queue-only`, `--scheduler-only`, `--status`, `--workers=N`, `--queues=...` |
| `php artisan native:tail --type=worker` | Tail structured JSON-line worker log from device via ADB                                           |

---

## 2. Runtime Architecture

### Process Model

A single Android app process hosts both the UI (WebView HTTP) and the worker
service. PHP runs via the Embed SAPI through JNI — there is no CLI binary.

```
┌───────────────────────────────────────────────────────┐
│  Android App Process (single)                         │
│                                                       │
│  ┌─────────────────┐  ┌───────────────────────────┐   │
│  │ UI/HTTP Lane    │  │ Worker Service (FGS)      │   │
│  │  WebView ↔ PHP  │  │  Supervisor (C)           │   │
│  │  RTLD_GLOBAL    │  │   ├─ WorkerPool (pthreads)│   │
│  │  php_embed      │  │   │   ├─ Worker 0 (TSRM) │   │
│  └─────────────────┘  │   │   ├─ Worker 1 (TSRM) │   │
│                        │   │   └─ ...              │   │
│  Shared:               │   └─ SchedulerGate       │   │
│   • OPcache SHM       │                           │   │
│   • SQLite WAL DB     │  PhpWorkerService.kt      │   │
│   • libphp.so         │   WakeLock, Notification   │   │
│                        └───────────────────────────┘   │
└───────────────────────────────────────────────────────┘
```

### Lane Separation (MUST preserve)

| Lane             | Bootstrap                                                   | Environment                                                   |
| ---------------- | ----------------------------------------------------------- | ------------------------------------------------------------- |
| **HTTP/UI**      | `bootstrap/android/native.php` / `bootstrap/ios/native.php` | `APP_RUNNING_IN_CONSOLE=false`, `NATIVEPHP_JOB_TYPE=http`     |
| **Queue worker** | `bootstrap/worker/queue_worker.php` → `common.php`          | `APP_RUNNING_IN_CONSOLE=true`, `NATIVEPHP_JOB_TYPE=queue`     |
| **Scheduler**    | `bootstrap/worker/scheduler_tick.php` → `common.php`        | `APP_RUNNING_IN_CONSOLE=true`, `NATIVEPHP_JOB_TYPE=scheduler` |

**Never allow console/headless env flags to leak into HTTP requests.**

### Thread Safety Model

- **Process globals** (set once before threads start via `setenv()`):
  `NATIVEPHP_RUNNING`, `APP_URL`, `COLUMNS`, `LINES`
- **Per-thread** (TSRM-managed): `$_ENV`, `$_SERVER`, `$GLOBALS`, Zend compiler/executor
- **Shared read-mostly**: OPcache bytecode cache (internally locked SHM)
- **Cross-thread sync**: `flock(LOCK_EX)` on lockfile for table creation/config caching;
  `pthread_mutex`/`condvar` for native job queue and scheduler gate

### Key Design Invariants

1. **One job per invocation** — Each queue job does `request_startup → execute → request_shutdown`. Clean superglobals per job, no memory leak accumulation.
2. **No `fork()` in worker threads** — fork() in a multithreaded process deadlocks. The `common.php` bootstrap pre-sets `Terminal::$width`/`$height` via Reflection to prevent Symfony Console from calling `proc_open('stty')`.
3. **No `setenv()`/`putenv()` from worker threads** — `common.php` uses `$_ENV`/`$_SERVER` assignment only (thread-local in ZTS). The C bridge blocks `setenv()` after engine init.
4. **Static variables are per-thread in ZTS** — Guards like `static $done = false` do NOT prevent cross-thread races. Use `flock()` on lockfiles instead.
5. **`RTLD_GLOBAL` via compat preloader** — Android's `System.loadLibrary()` uses `RTLD_LOCAL`. On API 36+, re-opening with `RTLD_GLOBAL` does NOT promote. The `JNI_OnLoad` in `compat/android_compat.cpp` (compiled into `libcompat.so`) loads libphp.so with `RTLD_GLOBAL` _before_ `System.loadLibrary("php")` so opcache.so can resolve `execute_ex`. The build script also adds `DT_NEEDED: libphp.so` to opcache.so via patchelf as belt-and-suspenders. The former `php_preloader.c` approach was abandoned (file removed) because the separate library wasn't packaged into the APK.

---

## 3. Android Startup Sequence

`LaravelEnvironment.initialize()` runs in this order:

1. Create required directories (`app_storage/laravel/`, `persisted_data/`)
2. Apply OTA update or extract Laravel bundle ZIP
3. Set process-wide environment variables via `setenv()` (before engine init)
4. Run base startup commands (`runBaseArtisanCommands` — migrations, config)
5. Initialize persistent PHP engine (`php_engine_init()`)
6. Start worker service (`PhpWorkerService`)

**Do not move migrations after engine init** without redesigning startup.

### Threading Policy for Migrations

- Migrations run in the startup worker thread (not Android UI/main thread).
- Migrations execute BEFORE the first WebView request is allowed.
- Worker pool threads do NOT run migrations — startup migration is a single-shot bootstrap gate.

---

## 4. Migration Strategy

### Why Deterministic State Keys

A simple timestamp stamp can incorrectly skip migrations after schema drift,
producing `SQLSTATE[HY000]: no such table: ...` errors at runtime.

### Current Strategy

`runBaseArtisanCommands()` computes a **migration state key** from:

- Extracted app version from `.env`
- Fingerprint of `database/migrations/*.php` files (path + size + mtime)
- Bundled Laravel ZIP hash (`.bundle_hash`)

**Migrations run when any of these are true:**

- Bundle/update extraction just occurred
- State file missing (`persisted_data/.nativephp_migration_state`)
- Stored state differs from computed state
- `migrations` table absent/empty in SQLite
- `.bundle_hash` changed

**After successful migration:**

- Writes `persisted_data/.nativephp_migration_state` (deterministic key)
- Writes `persisted_data/.nativephp_migrations_bootstrapped` (legacy marker)
- State files only written after verifying DB migration history

### Database Handling

- SQLite DB lives at `persisted_data/database/database.sqlite`
- DB is **not** copied from the bundle ZIP — created fresh if missing
- Bundle extraction intentionally skips `storage/` and uses `persisted_data/` for durable data

### Environment Override

- `NATIVEPHP_RUN_MIGRATIONS_ON_BOOT=true` (default): run migrations
- `NATIVEPHP_RUN_MIGRATIONS_ON_BOOT=false`: skip (not recommended)

---

## 5. Worker System Internals

### WorkerServiceProvider Boot Sequence

The provider is auto-discovered and performs these steps:

1. **Register**: merges `config/nativephp-worker.php`, registers `WorkerConfig` singleton
2. **Boot** (only when `NATIVEPHP_RUNNING=true`):
   a. Configure SQLite WAL mode on connection establishment (`busy_timeout=5000`, `journal_mode=WAL`, `synchronous=NORMAL`, `wal_autocheckpoint=100`, `cache_size=-8000`)
   b. Configure queue defaults (promote `sync` → `database` if enabled, or configure Redis)
   c. Configure OPcache INI settings if enabled
   d. **If headless worker lane**: run serialized first-boot tasks under `flock()`:
   - Auto-create `jobs`, `failed_jobs`, `job_batches` tables (IF NOT EXISTS)
   - Auto-create `worker_errors` table for `WorkerErrorReporter`
   - Generate lightweight config cache (serializes loaded config repository, no fresh Application)

### Queue Processing Flow

```
dispatch(new MyJob)  →  jobs table (SQLite WAL)
                              │
Supervisor polls (5s)  ←──────┘
  supervisor_enqueue_queue_job()
  → php_request_context_t created
  → submitted to worker_pool
      │
Worker thread:
  php_request_startup()
  $_SERVER injection (thread-safe)
  zend_execute_scripts(queue_worker.php)
    → require common.php (bootstrap Laravel)
    → pop ONE job from queue
    → job->fire()
    → job->delete() on success
    → WorkerErrorReporter::capture() on failure
    → JSON result to stdout
  php_request_shutdown()
      │
  supervisor_await_job() → Kotlin reads result
```

### Scheduler Processing Flow

```
Supervisor tick (60s)
  supervisor_enqueue_scheduler_tick()
  → gated by scheduler_gate (mutex, reject-not-queue)
  → php_request_context_t submitted to pool
      │
Worker thread:
  php_request_startup()
  zend_execute_scripts(scheduler_tick.php)
    → require common.php
    → $kernel->call('schedule:run', ['--no-ansi', '--quiet'], NullOutput)
    → $kernel->terminate()
    → JSON result to stdout
  php_request_shutdown()
```

**Critical:** `scheduler_tick.php` uses `NullOutput` and `--quiet` flags. The
`common.php` bootstrap pre-sets `Terminal::$width`/`$height` to prevent
`fork()` deadlock from `stty` detection.

### Error Reporting

Worker failures are captured in two ways:

1. **`WorkerErrorReporter`** — Writes to `worker_errors` SQLite table with full context: worker_id, job_type, queue, job_class, error_class, message, file, line, trace, php_thread, memory_usage. Methods: `capture($e, $context)`, `recent($limit, $filter)`, `summary()`, `count($withinMinutes)`, `pruneOlderThan($days)`, `flush()`.
2. **Structured JSON log** — The C supervisor writes JSON-line events to `storage/logs/worker.jsonl` (readable via `native:tail --type=worker`)

### Circuit Breaker

Each worker thread tracks consecutive crashes. After N consecutive failures
(default 3), the thread backs off exponentially: `base * 2^(crashes - threshold)`
seconds, capped at 5 minutes. A single successful job resets the counter.

### OPcache Integration

OPcache symbol resolution uses a two-layer approach:

**Layer 1 — `compat/android_compat.cpp` JNI_OnLoad (primary, Kotlin load order)**:

1. `System.loadLibrary("compat")` — `JNI_OnLoad` runs and calls `dlopen(libphp.so, RTLD_NOW | RTLD_GLOBAL)` before anything else loads it
2. `System.loadLibrary("php")` — Bionic finds already-loaded libphp.so, preserves RTLD_GLOBAL flag
3. Later, `dlopen(opcache.so)` in `php_engine_init()` resolves all 426 undefined symbols from global scope

**Layer 2 — patchelf DT_NEEDED (belt-and-suspenders, build-time)**:

- The build script adds `DT_NEEDED: libphp.so` to opcache.so via patchelf
- Bionic resolves opcache.so's symbols via direct dependency linkage, regardless of RTLD_GLOBAL

**Layer 3 — `php_engine_init()` dlopen fallback (legacy, for older API levels)**:

- `dlopen(libphp.so, RTLD_NOW | RTLD_GLOBAL)` attempt in `php_engine_init()` — works on pre-API 36 Bionic
- `php_engine.c` is committed (1188 lines) and compiled into `libphp_wrapper.so`

**Post-load steps** (in `php_engine_init()`):

1. `dlopen(opcache.so, RTLD_NOW | RTLD_GLOBAL)` — loads OPcache before `php_embed_init()`
2. INI: `zend_extension=<path>/opcache.so`, `opcache.enable=1`, `opcache.enable_cli=1`
3. OPcache warmup: after engine init, requires autoloader + key framework classes to populate SHM
4. All worker threads share the OPcache SHM segment — no per-thread compilation

**Why `dlsym(RTLD_DEFAULT)` is unreliable**: Bionic's RTLD_DEFAULT searches ALL loaded objects including RTLD_LOCAL ones, so a non-NULL result does NOT prove RTLD_GLOBAL promotion. The definitive test is whether opcache.so's dlopen succeeds.

The `WorkerServiceProvider` also sets OPcache INI values from PHP: memory size,
interned strings buffer, file cache path (hybrid SHM + disk mode), timestamps
disabled (immutable on device).

### Analytics & Metrics

**`WorkerMetrics`** provides dashboard data via two sources:

- **Native bridge**: `nativephp_supervisor_status()` / `nativephp_queue_status()` for live stats
- **Fallback**: PHP-side queries when native bridge unavailable

Key methods:

- `WorkerMetrics::snapshot()` — Live supervisor status (activeJobs, pendingJobs, completedJobs, failedJobs, failureRatePercent, uptimeSeconds, workerCount, sqlitePool stats, memoryLimit)
- `WorkerMetrics::queueStatus()` — Per-queue depths and stats
- `WorkerMetrics::recentErrors($limit)` — Parsed from worker.jsonl log
- `WorkerMetrics::dashboard()` — Composite of snapshot + queueStatus + recentErrors + db pool
- `WorkerMetrics::throughput($windowMinutes)` — Jobs per minute over sliding window

**Status API**: `GET /_native/api/worker/status` returns JSON with native status, config, and DB queue stats.

**Dashboard Blade component**: `<x-nativephp-worker-dashboard />` — Alpine.js, auto-polls every 5s, shows status badge, jobs grid, uptime, scheduler state, DB stats, config summary.

---

## 6. Configuration Reference

All settings are in `config/nativephp-worker.php` with env var overrides:

| Setting                          | Env Var                                  | Default                       | Description                           |
| -------------------------------- | ---------------------------------------- | ----------------------------- | ------------------------------------- |
| `connection`                     | `NATIVEPHP_QUEUE_CONNECTION`             | app default                   | Queue connection name                 |
| `queues`                         | `NATIVEPHP_QUEUE_NAMES`                  | `default`                     | Comma-separated queue names           |
| `worker_count`                   | `NATIVEPHP_WORKER_COUNT`                 | 2 (Android), 1 (iOS)          | Concurrent worker threads (max 4)     |
| `mode`                           | `NATIVEPHP_WORKER_MODE`                  | `0` (all)                     | 0=all, 1=queue only, 2=scheduler only |
| `scheduler_interval`             | `NATIVEPHP_SCHEDULER_INTERVAL`           | `60`                          | Seconds between scheduler ticks       |
| `queue_poll_interval`            | `NATIVEPHP_QUEUE_POLL_INTERVAL`          | `5`                           | Seconds between queue polls           |
| `memory_limit`                   | `NATIVEPHP_WORKER_MEMORY_LIMIT`          | `512M`                        | Per-worker PHP memory limit           |
| `circuit_breaker_threshold`      | `NATIVEPHP_CIRCUIT_BREAKER_THRESHOLD`    | `3`                           | Consecutive failures before backoff   |
| `circuit_breaker_backoff`        | `NATIVEPHP_CIRCUIT_BREAKER_BACKOFF`      | `5`                           | Base backoff seconds (doubles)        |
| `override_sync_driver`           | `NATIVEPHP_OVERRIDE_SYNC_DRIVER`         | `true`                        | Auto-promote sync → database          |
| `immediate_dispatch`             | `NATIVEPHP_WORKER_IMMEDIATE_DISPATCH`    | `true`                        | Wake pool on foreground dispatch      |
| `opcache_enabled`                | `NATIVEPHP_OPCACHE_ENABLED`              | `true`                        | Enable OPcache SHM                    |
| `opcache_memory_mb`              | `NATIVEPHP_OPCACHE_MEMORY_MB`            | `32`                          | OPcache SHM size                      |
| `opcache_file_cache`             | `NATIVEPHP_OPCACHE_FILE_CACHE`           | `true`                        | Persist bytecode to disk              |
| `log_enabled`                    | `NATIVEPHP_WORKER_LOG_ENABLED`           | `true`                        | Structured worker.jsonl log           |
| `log_max_size_kb`                | `NATIVEPHP_WORKER_LOG_MAX_SIZE`          | `1024`                        | Max log size before rotation          |
| `auto_start`                     | `NATIVEPHP_WORKER_AUTO_START`            | auto-detect                   | Auto-start workers on app launch      |
| `android_execution_strategy`     | `NATIVEPHP_ANDROID_EXECUTION_STRATEGY`   | `auto`                        | `auto`/`foreground`/`workmanager`     |
| `workmanager_interval_minutes`   | `NATIVEPHP_WORKMANAGER_INTERVAL_MINUTES` | `15`                          | WorkManager repeat interval           |
| `redis_enabled`                  | `NATIVEPHP_REDIS_ENABLED`                | `false`                       | Use Redis instead of SQLite           |
| `redis_connection`               | `NATIVEPHP_REDIS_CONNECTION`             | `default`                     | Redis connection name                 |
| `config_cache_enabled`           | `NATIVEPHP_CONFIG_CACHE_ENABLED`         | `true`                        | Cache config on first boot            |
| `db_pool_size`                   | `NATIVEPHP_DB_POOL_SIZE`                 | `4`                           | Pre-opened SQLite connections         |
| `max_worker_count`               | —                                        | `4`                           | Hard cap on concurrent threads        |
| `max_background_runtime_android` | `NATIVEPHP_MAX_BG_RUNTIME_ANDROID`       | `21600` (6h)                  | Android FGS time limit                |
| `max_background_runtime_ios`     | `NATIVEPHP_MAX_BG_RUNTIME_IOS`           | `25`                          | iOS BGProcessingTask limit            |
| `priority_map`                   | —                                        | `{high:5, default:0, low:-5}` | Queue name → native priority          |

---

## 7. AI Troubleshooting Runbook

### Symptom: `no such table: jobs` (or `failed_jobs`, `job_batches`)

1. Tables are auto-created by `WorkerServiceProvider::ensureQueueTablesExist()` using `CREATE TABLE IF NOT EXISTS`.
2. This only runs for the **worker/scheduler lane** (`NATIVEPHP_JOB_TYPE=queue|scheduler`), not for artisan commands.
3. Check startup logs for `artisan 'migrate --force' starting` / `completed` / `skipped (migration state unchanged)`.
4. If schema drift: clear `persisted_data/.nativephp_migration_state` and `persisted_data/.nativephp_migrations_bootstrapped` on device, relaunch.

### Symptom: `opcache=NOT_AVAILABLE` in request timing

1. Check PhpEngine logcat: `adb logcat -d -v time -s "PhpEngine:*"`
2. Look for `execute_ex visible at 0x...` (working) or `cannot locate symbol "execute_ex"` (broken).
3. If broken: verify `php_engine.c` uses `RTLD_NOW | RTLD_GLOBAL` **without** `RTLD_NOLOAD` for the libphp.so promotion.
4. Verify libphp.so exports `execute_ex`: `nm -D libphp.so | grep execute_ex` — must show `T execute_ex`.

### Symptom: Worker thread permanently hung (scheduler tick)

1. This was Bug 9 — `fork()` deadlock from `Terminal::getWidth()` → `proc_open('stty')` in a multithreaded process.
2. Verify `common.php` has the Reflection-based `Terminal::$width`/`$height` pre-set.
3. Verify `php_engine.c` has `setenv("COLUMNS", "80", 0)` before `php_embed_init()`.
4. Check for any other `proc_open()`/`exec()`/`shell_exec()` calls in scheduled task code — all are UNSAFE in worker context.

### Symptom: Cold boot too slow

1. Confirm migration skip path is active (`migration state unchanged` in logs).
2. Confirm OPcache is loaded (`opcache=AVAILABLE` in timing headers).
3. Check if config cache exists: `bootstrap/cache/config.php`.
4. Check OPcache file cache: `storage/framework/opcache/` should contain `.bin` files after first boot.
5. Expected with OPcache: autoload ~50-100ms, total ~300-500ms per request. Without: ~300-800ms autoload, ~1300-2400ms total.

### Symptom: Worker start returns conflict but status is running

1. Treat worker start as idempotent.
2. If status already reports `running`, return success with `already_running=true`.
3. Only return conflict/error when start is rejected **and** status is not running.

### Symptom: UI route behaves like console lane

1. Check lane env in `LaravelEnvironment.kt` — HTTP lane must set `APP_RUNNING_IN_CONSOLE=false`.
2. Check `WorkerServiceProvider::isHeadlessWorkerLane()` — relies on `APP_RUNNING_IN_CONSOLE` and `NATIVEPHP_JOB_TYPE`.
3. Ensure artisan/headless code does not leak `putenv()` calls.

### Symptom: SQLITE_BUSY errors

1. Verify WAL mode: `PRAGMA journal_mode` should return `wal`.
2. `WorkerServiceProvider` sets `busy_timeout=5000` — if still failing, increase to `10000`.
3. Reduce `NATIVEPHP_WORKER_COUNT` to 1 for diagnosis.
4. Ensure jobs don't hold long database transactions.

### Symptom: Worker errors not visible in app

1. Check `worker_errors` table exists — created by `WorkerErrorReporter::ensureTableExists()`.
2. Use `WorkerErrorReporter::recent(20)` to query directly.
3. Use `WorkerMetrics::dashboard()` for a combined view.
4. Check structured log: `adb shell run-as <appId> cat app_storage/persisted_data/storage/logs/worker.jsonl`

---

## 8. Debugging Tools

### ADB Logcat Tags

| Tag           | Content                                                        |
| ------------- | -------------------------------------------------------------- |
| `PhpEngine`   | Engine init, RTLD_GLOBAL promotion, OPcache probe, PHP version |
| `Supervisor`  | Worker pool start/stop, job enqueue/complete/fail              |
| `PHP`         | PHP's `error_log()` output — includes `PerfTiming` lines       |
| `PHP-Native`  | Request timing headers (`X-PHP-Timing`)                        |
| `PHPBridge`   | JNI bridge calls, `nativephp_call()` results                   |
| `WORKER-DIAG` | Bootstrap diagnostic lines (gated by `NATIVEPHP_DEBUG=true`)   |

### Checking Performance

Request timing is emitted on every HTTP and worker request:

```
X-PHP-Timing: opcache=AVAILABLE,autoload=52.3ms,bootstrap=18.1ms,kernel_boot=145.2ms,handle=89.5ms,total=312.1ms
```

Key indicators:

- `opcache=NOT_AVAILABLE` → OPcache not loaded (see troubleshooting above)
- `autoload > 200ms` → Likely no OPcache
- `total > 1000ms` → Investigate (expected ~300-500ms with OPcache)

### Worker Metrics API

```
GET /_native/api/worker/status
→ { status, activeJobs, pendingJobs, completedJobs, failedJobs,
    schedulerRunning, uptimeSeconds, mode, config: {...}, queueStats: {...} }
```

PHP access:

```php
WorkerMetrics::snapshot();      // Live supervisor status
WorkerMetrics::queueStatus();   // Queue depths per queue name
WorkerMetrics::dashboard();     // Combined: supervisor + queue + errors + db pool
WorkerMetrics::throughput(5);   // Jobs/minute over last N minutes
```

---

## 9. Guardrails for AI Edits

1. **Prefer minimal deltas** in runtime startup code.
2. **Never remove migration safety checks** to "improve speed".
3. **Never use `putenv()`/`setenv()`** from PHP worker code — use `$_ENV`/`$_SERVER` only.
4. **Never use `fork()`/`proc_open()`/`exec()`/`shell_exec()`** from worker threads.
5. **Never use `static` variables** for cross-thread coordination — use `flock()` on lockfiles.
6. **Keep source and consumer copies synchronized** when both exist (mobile-air → packages/mobile-air → EngineeringApp).
7. **Test OPcache status** after any change to `php_engine.c` — check for `execute_ex visible at` in logcat.
8. **Do not use `RTLD_NOLOAD`** for the libphp.so RTLD_GLOBAL promotion — Bionic silently ignores flag changes.
9. Validate Kotlin/PHP/Swift files with diagnostics or lint after edits.
10. Do not rely on docs-only assumptions — confirm behavior from code paths.

---

## 10. Quick Checklist Before Handoff

- [ ] Boot migration logic uses deterministic state key
- [ ] First run/update executes `migrate --force`; unchanged schema skips quickly
- [ ] HTTP/headless lane env separation preserved
- [ ] Queue tables auto-created via IF NOT EXISTS (no race conditions)
- [ ] OPcache loads successfully (PhpEngine logs show `execute_ex visible at`)
- [ ] No `fork()`/`proc_open()` calls reachable from worker context
- [ ] `flock()` used for all cross-thread coordination (not `static` variables)
- [ ] Worker errors captured in `worker_errors` table
- [ ] Diagnostics show no new errors in edited files
- [ ] Changes synced to EngineeringApp's `nativephp/android/` if applicable
