# NativePHP Android: Multithreaded Worker Implementation

Summary of how concurrent Laravel queue workers, scheduler, and supervisor were
implemented on NativePHP for Android, including all changes to libphp.so, the
native C layer, Kotlin bridge, PHP service providers, and bootstrap scripts.

---

## 1. Architecture Overview

```
┌──────────────────────────────────────────────────────────────────────┐
│  Android App Process                                                 │
│                                                                      │
│  ┌─────────────────────┐   ┌──────────────────────────────────────┐  │
│  │ UI Process (Main)   │   │ Worker Service Process (FGS)        │  │
│  │  - WebView / HTTP   │   │                                      │  │
│  │  - LaravelEnvironment│  │  ┌────────────────────────────────┐  │  │
│  │  - phpBridge (JNI)  │   │  │ Supervisor (C)                 │  │  │
│  │  - Single PHP thread│   │  │  ├─ PhpEngine (singleton init) │  │  │
│  └─────────────────────┘   │  │  ├─ WorkerPool (N pthreads)   │  │  │
│                             │  │  │   ├─ Worker 0 (TSRM ctx)  │  │  │
│                             │  │  │   ├─ Worker 1 (TSRM ctx)  │  │  │
│                             │  │  │   └─ ...                   │  │  │
│                             │  │  └─ SchedulerGate (mutex)     │  │  │
│                             │  └────────────────────────────────┘  │  │
│                             │                                      │  │
│                             │  PhpWorkerService.kt (Kotlin FGS)    │  │
│                             │  - WakeLock, Notification, Polling    │  │
│                             └──────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

The worker service runs as a **separate Android process** from the UI process.
Within that process, a single PHP engine is initialized once, and N worker threads
(default 2 queue + 1 scheduler) each get their own TSRM interpreter context
via `ts_resource(0)`.

---

## 2. Native C Modules (JNI Layer)

All C source files live in `resources/androidstudio/app/src/main/cpp/` and are
compiled into `libphp_wrapper.so` via CMake:

### 2.1 Module Inventory

| File                    | Purpose                                                                                                                                                                            |
| ----------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `php_engine.c`          | Process-wide PHP engine singleton. Calls `php_embed_init()` once (ZTS mode), probes/loads OPcache, sets INI entries, RTLD_GLOBAL promotion.                                        |
| `php_thread_context.c`  | Per-thread TSRM attachment/detachment. Each worker thread calls `php_thread_attach()` → `ts_resource(0)` + `TSRMLS_CACHE_UPDATE()`.                                                |
| `php_request_context.c` | Per-job isolated execution. Creates isolated stdout/stderr buffers, manages `php_request_startup()` / `php_request_shutdown()` per job, injects `$_SERVER` env vars thread-safely. |
| `worker_pool.c`         | Native pthread pool (1-8 workers). Manages job queue, completion tracking, circuit breaker per thread, and `worker_pool_wake()` for immediate dispatch.                            |
| `scheduler_gate.c`      | Mutex ensuring `schedule:run` never overlaps. New ticks are rejected (not queued) while one is running.                                                                            |
| `supervisor.c`          | Top-level orchestrator. Owns engine lifecycle, pool, gate, job ID generation, structured logging, and the JNI-callable API.                                                        |
| `php_bridge.c`          | JNI bridge between Kotlin and PHP. Includes `native_set_env()` which blocks `setenv()` once engine is running (thread-safety guard).                                               |
| `zts_guard.h`           | Compile-time `#error` if ZTS is not defined. Runtime check via `nativephp_verify_zts_runtime()`.                                                                                   |
| `PHP.c`                 | Legacy single-request PHP execution (UI process HTTP serving).                                                                                                                     |
| `CMakeLists.txt`        | Builds all above into `libphp_wrapper.so`, links against `libphp.so`, `liblog`, `libdl`. Also checks `php_config.h` for `#define ZTS 1` at build time.                             |

### 2.2 PhpEngine Init Sequence (`php_engine.c`)

```
php_engine_init()
├── pthread_mutex_lock (singleton guard)
├── Verify ZTS at compile time (#ifndef ZTS → error)
├── dladdr(php_embed_init) → find libphp.so directory
├── dlopen(libphp.so, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD)  ← CRITICAL
│   └── Promotes symbols to global scope (opcache.so needs execute_ex)
├── dlopen(opcache.so, RTLD_NOW | RTLD_GLOBAL)                ← probe load
├── Build INI entries (extension_dir, zend_extension=opcache.so, OPcache config)
├── php_embed_init(1, argv)
│   └── tsrm_startup → sapi_startup → php_module_startup → php_request_startup
├── php_request_shutdown(NULL)  ← shut down main thread's request
├── sapi_module.ub_write = php_request_ub_write  ← TLS-routed output
├── setenv() for static vars (PHP_SELF, HTTP_HOST, NATIVEPHP_RUNNING, etc.)
├── s_engine_initialized = 1
└── pthread_mutex_unlock
```

### 2.3 Worker Thread Lifecycle (`worker_pool.c` + `php_thread_context.c`)

```
worker_thread_func(worker_id)
├── php_thread_attach()
│   ├── ts_resource(0)       ← allocates per-thread ZTS globals
│   └── TSRMLS_CACHE_UPDATE()
├── LOOP while pool->running:
│   ├── pthread_cond_wait on queue_cond (sleep until job available)
│   ├── Dequeue job (php_request_context_t)
│   ├── php_request_execute(ctx)
│   │   ├── php_request_startup()
│   │   ├── Set $_SERVER vars (thread-safe, no setenv)
│   │   ├── zend_execute_scripts(ZEND_REQUIRE, script_path)
│   │   ├── Capture stdout into ctx->stdout_buf
│   │   └── php_request_shutdown(NULL)
│   ├── Move ctx to completed_head (for await)
│   └── supervisor_notify_job_completed/failed
└── php_thread_detach()
    └── ts_free_thread()
```

---

## 3. libphp.so Changes

### 3.1 ZTS (Zend Thread Safety) Requirement

PHP must be compiled with `--enable-zts` for the multi-worker architecture.
The stock NativePHP libphp.so was NTS. We built a custom PHP 8.4.15 with:

```bash
# Key configure flags for ZTS build
--enable-zts \
--enable-embed=shared \     # embed SAPI for Android JNI
--enable-opcache \          # shared bytecode cache across threads
--enable-bcmath --enable-calendar --enable-exif --enable-ftp \
--enable-mbstring --enable-pcntl --enable-sockets --enable-soap \
--enable-fileinfo \
--with-curl --with-openssl --with-zlib --with-zip --with-sodium \
--with-libxml --with-sqlite3 --with-pdo-sqlite --with-iconv
```

Build script: `scripts/build_php_android_arm64.sh`

### 3.2 RTLD_GLOBAL Symbol Promotion (`php_engine.c`)

**Problem:** Android's `System.loadLibrary()` uses `RTLD_LOCAL`, so symbols from
`libphp.so` (e.g., `execute_ex`, `zend_execute_ex`) are not visible to
subsequently `dlopen()`'d shared libraries like `opcache.so`.

**Fix:** Before probing `opcache.so`, we re-open the already-loaded `libphp.so`
with `RTLD_GLOBAL | RTLD_NOLOAD` to promote its symbols:

```c
void *php_global = dlopen(di.dli_fname, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
```

**Current Status:** This fix works correctly with the custom PHP 8.4.15 build
(which exports `execute_ex` in its dynamic symbol table). The stock PHP 8.4.5
binary does NOT export `execute_ex` even with RTLD_GLOBAL, so OPcache fails
with the stock binary. **OPcache requires the custom build.**

### 3.3 OPcache Shared Bytecode Cache

OPcache is critical for multi-worker performance: without it, each worker thread
re-parses and re-compiles ~200+ PHP files per job (~2-4MB extra memory per thread).

INI configuration (set in `php_engine.c` at init):

```ini
opcache.enable=1
opcache.enable_cli=1           ; Required for embed SAPI
opcache.memory_consumption=32  ; 32MB shared segment
opcache.interned_strings_buffer=8
opcache.max_accelerated_files=4000
opcache.validate_timestamps=0  ; Immutable on device
opcache.save_comments=1        ; Required for annotations
opcache.file_update_protection=0
```

---

## 4. Kotlin Layer

### 4.1 PhpWorkerService (`worker/PhpWorkerService.kt`)

Android Foreground Service (type `dataSync`) that:

- Acquires `PARTIAL_WAKE_LOCK` (6-hour timeout aligned with Android 14 FGS limit)
- Creates persistent notification
- Calls `supervisor_engine_init()` via JNI
- Calls `supervisor_start()` with configured worker count, queues, connection
- Periodically enqueues scheduler ticks (default every 60s)
- Periodically enqueues queue jobs (default every 5s, with immediate wake)
- Returns `START_STICKY` for auto-restart if killed

### 4.2 LaravelEnvironment.kt — `setEnvironmentVariable()` Fix

**Problem:** `LaravelEnvironment.setupEnvironment()` runs on a background thread
and calls `nativeSetEnv(APP_KEY, ...)` ~12 seconds after engine init. By that
time, worker threads are already running. The C layer's `native_set_env()`
returns -1 (blocked because `setenv()` is not thread-safe per POSIX). The old
code threw `RuntimeException` on non-zero result, which caused a `FATAL EXCEPTION`
→ `SIGKILL` → all workers die.

**Fix:** Changed from throwing to logging a warning:

```kotlin
private fun setEnvironmentVariable(name: String, value: String) {
    try {
        val result = nativeSetEnv(name, value, 1)
        if (result != 0) {
            // Engine already running — workers read from .env file
            Log.w(TAG, "Could not set env var (engine running): $name")
        }
    } catch (e: Exception) {
        Log.e(TAG, "Failed to set environment variable: $name", e)
        throw e
    }
}
```

Workers obtain environment variables from the `.env` file via Laravel's Dotenv
loader, so the process-level `setenv()` call is not required.

---

## 5. PHP Layer

### 5.1 WorkerServiceProvider (`src/Worker/WorkerServiceProvider.php`)

Auto-discovered by Laravel. Requires zero developer configuration. Handles:

#### SQLite WAL Mode

```php
// Set busy_timeout FIRST so WAL upgrade can retry instead of SQLITE_BUSY
$event->connection->statement('PRAGMA busy_timeout=5000');
$event->connection->statement('PRAGMA journal_mode=WAL');
$event->connection->statement('PRAGMA synchronous=NORMAL');
$event->connection->statement('PRAGMA wal_autocheckpoint=100');
$event->connection->statement('PRAGMA cache_size=-8000');
```

**Key fix:** `busy_timeout` must be set BEFORE `journal_mode=WAL`. The original
code set WAL first, which could return `SQLITE_BUSY` immediately if another
thread held a lock.

#### Serialized First-Boot Tasks (`serializedFirstBootTasks()`)

**Problem:** 3 worker threads call `boot()` simultaneously. Without serialization:

- `ensureQueueTablesExist()` has TOCTOU race (`Schema::hasTable()` then `Schema::create()`)
- `ensureConfigCached()` calls `Artisan::call('config:cache')` which creates a
  fresh Application per call — 6+ concurrent Application bootstraps total
- PHP `static` variables are **per-thread in ZTS mode** — they cannot guard
  cross-thread races

**Fix:** File-based `flock(LOCK_EX)` ensures only ONE thread does first-boot:

```php
protected function serializedFirstBootTasks(): void
{
    $lockPath = $this->app->storagePath() . '/framework/.worker_init.lock';
    $fp = @fopen($lockPath, 'c+');
    if (flock($fp, LOCK_EX)) {
        try {
            $this->ensureQueueTablesExist();
            $this->ensureConfigCached();
        } finally {
            flock($fp, LOCK_UN);
        }
    }
    fclose($fp);
}
```

#### Atomic Table Creation (`ensureQueueTablesExist()`)

Replaced `Schema::hasTable()` + `Schema::create()` (TOCTOU race) with raw
`CREATE TABLE IF NOT EXISTS` SQL statements that are atomic at the SQLite level:

```php
$db->statement('CREATE TABLE IF NOT EXISTS jobs (...)');
$db->statement('CREATE TABLE IF NOT EXISTS failed_jobs (...)');
$db->statement('CREATE TABLE IF NOT EXISTS job_batches (...)');
```

#### Disabled Runtime Config Caching (`ensureConfigCached()`)

`Artisan::call('config:cache')` internally calls `getFreshConfiguration()` which
creates a **fresh Application instance** — re-triggering the entire service
provider boot chain. With 3 threads doing this simultaneously, it caused 6+
concurrent bootstraps fighting over SQLite and file writes.

**Fix:** `ensureConfigCached()` is now a no-op at runtime. Config caching should
be done at build time via `native:build` or during `runBaseArtisanCommands()`.

### 5.2 Bootstrap Entrypoints

#### `bootstrap/worker/queue_worker.php`

- Pops and executes ONE queue job per invocation
- Thread-safe: sets `$_ENV`/`$_SERVER` (per-thread superglobals), NOT `putenv()`
- Autodetects paths (autoload.php, bootstrap/app.php)
- Ensures writable storage directories exist
- Configurable memory limit via `NATIVEPHP_WORKER_MEMORY_LIMIT`
- Outputs JSON result: `{ processed, job_name, error, duration_ms }`

#### `bootstrap/worker/scheduler_tick.php`

- Runs `schedule:run` once per invocation
- Same thread-safe bootstrap as queue_worker.php
- Uses `NullOutput` to suppress scheduler's console output
- Outputs JSON result: `{ ran, output, error, duration_ms }`

### 5.3 Thread-Safety Patterns in PHP

| Pattern               | Why                                                                |
| --------------------- | ------------------------------------------------------------------ |
| `$_ENV['KEY'] = $val` | Per-thread superglobal (safe in ZTS)                               |
| `putenv('KEY=val')`   | **NEVER** — mutates process-global env table                       |
| `getenv('KEY')`       | Reads process-global env (safe for read, set before threads start) |
| `static $flag`        | **Per-thread in ZTS** — cannot guard cross-thread races            |
| `flock(LOCK_EX)`      | Cross-thread safe (filesystem-level lock)                          |
| `ini_set()`           | Per-thread in ZTS (each thread has own INI values)                 |

---

## 6. Configuration

### `config/nativephp-worker.php`

Key settings:

- `connection` — Queue connection name (default: app's default)
- `queues` — Comma-separated queue names (default: "default")
- `worker_count` — Number of concurrent worker threads (default: platform default, typically 2)
- `mode` — 0=all, 1=queue only, 2=scheduler only
- `auto_start` — Auto-start workers when app boots (default: true)
- `override_sync_driver` — Auto-switch `sync` → `database` (default: true)
- `opcache.enabled` — Enable OPcache shared memory (default: true)
- `opcache.memory_mb` — OPcache memory consumption (default: 32)
- `circuit_breaker.max_crashes` — Consecutive crash threshold before backoff (default: 3)
- `circuit_breaker.base_backoff_seconds` — Exponential backoff base (default: 5)
- `memory_limit` — Per-worker PHP memory limit (default: "256M")

---

## 7. Bugs Found & Fixed

### Bug 1: Process Crash from `nativeSetEnv` RuntimeException

- **Root Cause:** `LaravelEnvironment.setupEnvironment()` runs on a background thread ~12s after engine init. `nativeSetEnv(APP_KEY)` returns -1 (blocked — engine already running). Old code threw `RuntimeException` → `FATAL EXCEPTION` → `SIGKILL`.
- **Fix:** Log warning instead of throwing. Workers read env from `.env` file.
- **File:** `LaravelEnvironment.kt` line ~1182

### Bug 2: SQLite PRAGMA Ordering

- **Root Cause:** `PRAGMA journal_mode=WAL` was set before `PRAGMA busy_timeout`. If another thread held a lock, WAL upgrade returned `SQLITE_BUSY` immediately (no retry).
- **Fix:** Set `busy_timeout=5000` FIRST, then `journal_mode=WAL`.
- **File:** `WorkerServiceProvider.php` → `configureSqliteWal()`

### Bug 3: Config Cache Infinite Recursion

- **Root Cause:** `Artisan::call('config:cache')` creates a fresh Application → boots WorkerServiceProvider → calls `ensureConfigCached()` → calls `config:cache` → infinite loop.
- **Fix (v1):** Added `static $inProgress` guard. **(Insufficient for ZTS — per-thread.)**
- **Fix (v2):** Disabled runtime config:cache entirely. It's a no-op now.
- **File:** `WorkerServiceProvider.php` → `ensureConfigCached()`

### Bug 4: TOCTOU Race on Queue Table Creation

- **Root Cause:** 3 threads simultaneously: `Schema::hasTable('jobs')` → false → `Schema::create('jobs')` → 2 of 3 get "table already exists" error.
- **Fix:** Replaced with `CREATE TABLE IF NOT EXISTS` (atomic at SQLite level) + `flock(LOCK_EX)` serialization.
- **File:** `WorkerServiceProvider.php` → `ensureQueueTablesExist()`

### Bug 5: OPcache `execute_ex` Symbol Not Found

- **Root Cause:** Android's `System.loadLibrary()` loads libphp.so with `RTLD_LOCAL`. opcache.so needs `execute_ex` from libphp.so but can't see it.
- **Fix:** Re-open libphp.so with `RTLD_GLOBAL | RTLD_NOLOAD` before loading opcache.so.
- **Caveat:** Only works with custom PHP build that exports `execute_ex` in its dynamic symbol table. Stock PHP 8.4.5 doesn't export it.
- **File:** `php_engine.c` lines ~155-170

### Bug 6: ZTS `static` Variables Are Per-Thread

- **Root Cause:** The `static $inProgress` guard in `ensureConfigCached()` was intended to prevent cross-thread races, but in ZTS mode, PHP `static` variables are per-thread (each thread gets its own copy via TSRM).
- **Fix:** Replaced with `flock(LOCK_EX)` filesystem lock which is truly cross-thread.
- **File:** `WorkerServiceProvider.php` → `serializedFirstBootTasks()`

---

## 8. Performance Observations

| Metric                             | First Boot                     | Subsequent Boots            |
| ---------------------------------- | ------------------------------ | --------------------------- |
| Autoloader load                    | ~2500ms                        | ~300ms                      |
| App creation                       | ~500ms (after autoloader)      | ~80ms                       |
| `kernel->bootstrap()`              | ~7400ms (3 threads concurrent) | ~1500ms                     |
| Total worker ready                 | ~10s from process start        | ~2s                         |
| Memory per thread                  | ~24MB (without OPcache)        | Expected ~20MB with OPcache |
| Job execution (`SimulateDataSync`) | ~4000ms                        | -                           |

OPcache would significantly reduce first-boot and subsequent bootstrap times by
caching compiled bytecode in shared memory across all threads.

---

## 9. Suggestions for Improvements / Cleanup

### 9.1 High Priority

1. **Deploy Custom PHP 8.4.15 Build**
   - The current stock PHP 8.4.5 doesn't export `execute_ex`, so OPcache is
     unavailable. The custom build script (`scripts/build_php_android_arm64.sh`)
     exists but has had build environment issues (missing iconv, WSL path issues).
   - **Impact:** OPcache would reduce per-thread memory by ~4MB and bootstrap
     time by ~40-60%.

2. **Build-Time Config Caching**
   - `ensureConfigCached()` is currently a no-op at runtime. Config caching should
     be added to the `native:build` / `native:run` pipeline — specifically in
     `runBaseArtisanCommands()` in LaravelEnvironment.kt.
   - Run `php artisan config:cache` once during build, and workers will
     automatically use the cached config file.

3. **Stagger Worker Thread Starts**
   - Currently all 3 threads boot simultaneously, causing SQLite contention during
     bootstrap. Staggering starts by ~2 seconds per thread would reduce contention
     and memory spikes.
   - Implement in `worker_pool.c` → `worker_thread_func()` with a
     `usleep(worker_id * 2000000)` before first `php_request_execute`.

### 9.2 Medium Priority

4. **Remove Diagnostic Logging**
   - `[WORKER-DIAG]` / `[ARTISAN-DIAG]` error_log lines in `queue_worker.php` and
     `scheduler_tick.php` are invaluable for debugging but noisy in production.
   - Gate behind `NATIVEPHP_DEBUG` env var or remove before release.

5. **Consolidate Bootstrap Path Detection**
   - Both `queue_worker.php` and `scheduler_tick.php` have identical 40-line
     bootstrap blocks (autoloader detection, storage path setup, directory
     creation). Extract to a shared `bootstrap/worker/common.php`.

6. **Worker Pool Sizing by Device Capability**
   - Current default is 2 queue workers + 1 scheduler (3 threads total).
   - On low-memory devices (<3GB RAM), reduce to 1 queue worker.
   - Use `ActivityManager.getMemoryInfo()` on Kotlin side to determine count.

7. **Warm OPcache After Engine Init**
   - After `php_engine_init()`, run a throwaway PHP request that `require`s the
     autoloader and key framework files. This populates OPcache's shared memory
     segment before worker threads start, so all threads benefit from cached
     bytecode on their first real job.

### 9.3 Low Priority / Future

8. **Replace FGS with WorkManager for Android 14+**
   - `dataSync` foreground services have a 6-hour limit on Android 14+.
   - For long-running apps, consider migrating to `WorkManager` with periodic
     work requests, or using the `specialUse` FGS type (requires Play Store
     exemption).

9. **Connection Pooling for SQLite**
   - Each worker thread currently opens its own SQLite connection.
   - Consider a shared connection pool with `WAL` mode to reduce file handle
     overhead and improve write concurrency.

10. **Pre-compiled PHP Scripts (file cache)**
    - Beyond OPcache SHM, PHP's `opcache.file_cache` could persist compiled
      bytecode to disk. On cold boot, this avoids recompiling all ~200+ files.
    - Set `opcache.file_cache=/data/data/.../app_storage/opcache_files/`.

11. **Native Queue Popping**
    - Currently each job requires a full Laravel bootstrap → pop → execute → shutdown
      cycle. A native-level SQLite queue popper in C could check for pending
      jobs without bootstrapping PHP, and only spin up a PHP request when work
      exists.

12. **Unified Error Reporting**
    - Worker failures are currently logged to logcat and a JSON file
      (`worker.log`). Consider feeding errors back to the Laravel app via
      a SQLite `worker_errors` table that the UI can query.

---

## 10. File Reference

### C/C++ (Native Layer)

| File                                                                                    | Lines | Description                                      |
| --------------------------------------------------------------------------------------- | ----- | ------------------------------------------------ |
| [php_engine.c](resources/androidstudio/app/src/main/cpp/php_engine.c)                   | 384   | Engine singleton, RTLD_GLOBAL fix, OPcache probe |
| [php_thread_context.c](resources/androidstudio/app/src/main/cpp/php_thread_context.c)   | 120   | TSRM attach/detach per worker thread             |
| [php_request_context.c](resources/androidstudio/app/src/main/cpp/php_request_context.c) | 926   | Per-job execution, output buffers, cancellation  |
| [worker_pool.c](resources/androidstudio/app/src/main/cpp/worker_pool.c)                 | 567   | Pthread pool, job queue, circuit breaker         |
| [supervisor.c](resources/androidstudio/app/src/main/cpp/supervisor.c)                   | 609   | Top-level orchestrator, JNI API                  |
| [scheduler_gate.c](resources/androidstudio/app/src/main/cpp/scheduler_gate.c)           | ~80   | Mutex gate for schedule:run exclusivity          |
| [php_bridge.c](resources/androidstudio/app/src/main/cpp/php_bridge.c)                   | 1325  | JNI bridge, nativeSetEnv guard                   |
| [zts_guard.h](resources/androidstudio/app/src/main/cpp/zts_guard.h)                     | 45    | Compile-time/runtime ZTS verification            |
| [CMakeLists.txt](resources/androidstudio/app/src/main/cpp/CMakeLists.txt)               | 124   | Build config, ZTS check at cmake time            |

### Kotlin (Android Layer)

| File                                                                                                                 | Description                                         |
| -------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------- |
| [PhpWorkerService.kt](resources/androidstudio/app/src/main/java/com/nativephp/mobile/worker/PhpWorkerService.kt)     | Foreground service, wake lock, supervisor lifecycle |
| [LaravelEnvironment.kt](resources/androidstudio/app/src/main/java/com/nativephp/mobile/bridge/LaravelEnvironment.kt) | Env setup, setEnvironmentVariable fix               |

### PHP (Framework Layer)

| File                                                              | Description                                           |
| ----------------------------------------------------------------- | ----------------------------------------------------- |
| [WorkerServiceProvider.php](src/Worker/WorkerServiceProvider.php) | Auto-config: WAL, table creation, flock serialization |
| [WorkerConfig.php](src/Worker/WorkerConfig.php)                   | Configuration accessor class                          |
| [queue_worker.php](bootstrap/worker/queue_worker.php)             | Pop-one-job entrypoint                                |
| [scheduler_tick.php](bootstrap/worker/scheduler_tick.php)         | schedule:run entrypoint                               |
| [nativephp-worker.php](config/nativephp-worker.php)               | Default worker configuration                          |

### Build

| File                                                             | Description                                                |
| ---------------------------------------------------------------- | ---------------------------------------------------------- |
| [build_php_android_arm64.sh](scripts/build_php_android_arm64.sh) | Cross-compile PHP 8.4.x for Android arm64 with ZTS+OPcache |
