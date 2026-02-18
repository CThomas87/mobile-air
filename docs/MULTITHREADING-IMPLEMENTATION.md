# NativePHP Android: Multithreaded Worker Implementation

Comprehensive reference for the concurrent Laravel queue-worker, scheduler, and
supervisor system implemented on NativePHP for Android. Covers every layer —
custom PHP binary, native C threading runtime, Kotlin Android service, PHP
bootstrap/service-provider integration, CMake build system, and the cross-
compilation pipeline — plus instructions for app developers who want to use
background workers in their NativePHP apps.

**Last updated:** 2026-02-18

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [How Multithreading Works — End to End](#2-how-multithreading-works--end-to-end)
3. [Custom libphp.so Build](#3-custom-libphpso-build)
4. [Native C Modules (JNI Layer)](#4-native-c-modules-jni-layer)
5. [Kotlin Android Layer](#5-kotlin-android-layer)
6. [PHP Framework Layer](#6-php-framework-layer)
7. [CMake Build System](#7-cmake-build-system)
8. [Build Script — Cross-Compiling PHP](#8-build-script--cross-compiling-php)
9. [OPcache: The RTLD_GLOBAL Problem and Solution](#9-opcache-the-rtld_global-problem-and-solution)
10. [Bugs Found & Fixed](#10-bugs-found--fixed)
11. [Performance Observations](#11-performance-observations)
12. [Configuration Reference](#12-configuration-reference)
13. [Using Workers in Your NativePHP App](#13-using-workers-in-your-nativephp-app)
14. [Suggestions for Improvements / Cleanup](#14-suggestions-for-improvements--cleanup)
15. [File Reference](#15-file-reference)

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

The worker system runs as an Android **Foreground Service** (type `dataSync`)
within the same app process. A single PHP engine is initialised once with ZTS
(Zend Thread Safety) enabled. N worker threads (default 2 queue + 1 scheduler)
each get their own TSRM interpreter context via `ts_resource(0)`.

### Key Design Decisions

| Decision                       | Rationale                                                                                |
| ------------------------------ | ---------------------------------------------------------------------------------------- |
| One process, multiple pthreads | Android restricts multi-process IPC; pthreads share the engine's OPcache SHM segment     |
| ZTS instead of forking         | Android lacks `fork()` for Zygote-compatible processes; TSRM is the only safe option     |
| Embed SAPI                     | No CLI binary — PHP runs in-process via JNI                                              |
| One-request-per-invocation     | Each job does `request_startup → execute → request_shutdown`; clean superglobals per job |
| SQLite/database queue driver   | Works offline; no network dependency                                                     |

### What We Built

Before this work, the NativePHP Android runtime had:

- **No background execution infrastructure** — no queue processing, no scheduler
- **Single-threaded PHP** — NTS (non-thread-safe) binary, one request at a time
- **No output isolation** — a single global C buffer for PHP output, not thread-safe
- **Process-global environment mutation** — `setenv()` calls for request routing

We added:

- A complete C-layer supervisor with pthread worker pool, scheduler gate, circuit breaker, and job priority scheduling
- A custom PHP 8.4.15 binary built with ZTS (Zend Thread Safety) and OPcache
- Per-thread TSRM context management and per-job isolated output buffers
- PHP bootstrap entrypoints (`queue_worker.php`, `scheduler_tick.php`, `common.php`)
- A `WorkerServiceProvider` that auto-configures everything with zero developer setup
- An Android Foreground Service (`PhpWorkerService.kt`) with WakeLock and polling
- A complete build script for cross-compiling PHP for Android arm64
- A multi-layer OPcache symbol visibility solution for Android's Bionic linker

---

## 2. How Multithreading Works — End to End

### 2.1 Startup Sequence

1. **App Launch** — Android starts `PhpWorkerService.kt` (Foreground Service)
2. **Library Loading** — Kotlin loads native libraries in order:
   - `System.loadLibrary("compat")` — `JNI_OnLoad` pre-loads libphp.so with `RTLD_GLOBAL` (see §9)
   - `System.loadLibrary("php")` — Bionic finds already-loaded libphp.so, preserves RTLD_GLOBAL
   - `System.loadLibrary("php_wrapper")` — the C supervisor/worker pool/bridge code
3. **Engine Init** — Kotlin calls `supervisor_engine_init()` via JNI:
   - `php_engine_init()` — singleton; calls `php_embed_init()` once
   - OPcache probe — `dlopen(opcache.so, RTLD_NOW | RTLD_GLOBAL)` before embed init
   - OPcache warmup — requires autoloader + framework classes to populate SHM
4. **Supervisor Start** — Kotlin calls `supervisor_start(mode, worker_count, queues, connection)`:
   - Creates `scheduler_gate` (mutex)
   - Creates `worker_pool` with N+1 pthreads (N queue + 1 scheduler)
   - Each pthread starts `worker_thread_func()`:
     - Worker 0 starts immediately
     - Worker 1+ stagger by 2 seconds each (reduces bootstrap contention)
     - Each thread calls `php_thread_attach()` → `ts_resource(0)`
5. **Polling Loop** — Kotlin periodically:
   - Every 5s: calls `supervisor_enqueue_queue_job(NULL)` → creates `php_request_context_t` → submits to pool → worker runs `queue_worker.php`
   - Every 60s: calls `supervisor_enqueue_scheduler_tick(NULL)` → gated by `scheduler_gate` → worker runs `scheduler_tick.php`
6. **Job Execution** — Inside each worker thread:
   - `php_request_execute(ctx)` does: `php_request_startup()` → set `$_SERVER` vars → `zend_execute_scripts(queue_worker.php)` → capture stdout → `php_request_shutdown()`
   - Result JSON is returned to the completed list for `supervisor_await_job()`

### 2.2 Thread Safety Model

```
Process globals (set once before threads start):
  setenv("NATIVEPHP_RUNNING", "true")
  setenv("PHP_SELF", "/native.php")
  setenv("APP_URL", "http://127.0.0.1")
  setenv("COLUMNS", "80")    ← prevents fork() in Terminal::getWidth()

Per-thread (TSRM-managed):
  $_ENV, $_SERVER, $GLOBALS        ← each thread gets its own copy
  static variables in PHP          ← per-thread (TSRM allocates per-thread storage)
  ini_set() values                 ← per-thread
  OPcache bytecode cache           ← shared SHM (read-mostly, internally locked)

Cross-thread synchronisation:
  flock(LOCK_EX) on lockfile       ← for table creation, config caching
  CREATE TABLE IF NOT EXISTS       ← atomic at SQLite level
  SQLite WAL + busy_timeout        ← concurrent reads, serialised writes
  pthread_mutex / condvar          ← native job queue, scheduler gate
```

---

## 3. Custom libphp.so Build

### 3.1 Why a Custom Build Is Required

The stock NativePHP PHP binary (8.4.5) was compiled **without ZTS** and does not
export `execute_ex` in its dynamic symbol table. Two critical capabilities require
a custom build:

1. **ZTS (Zend Thread Safety)** — Without `--enable-zts`, running PHP in multiple
   pthreads corrupts global state (segfaults, data races).
2. **OPcache symbol visibility** — OPcache hooks `execute_ex` and
   `zend_execute_ex` at load time. The stock build strips them via
   `-fvisibility=hidden` during linking.

### 3.2 Build Configuration

PHP 8.4.15 is cross-compiled for Android arm64 with:

```bash
--host=aarch64-linux-android     # Android NDK r27 toolchain
--enable-zts                     # Zend Thread Safety (TSRM)
--enable-embed=shared            # Embed SAPI (JNI integration)
--enable-opcache                 # Shared bytecode cache (built as opcache.so)
--enable-bcmath --enable-calendar --enable-exif --enable-ftp
--enable-mbstring --enable-pcntl --enable-sockets --enable-soap
--enable-fileinfo
--with-curl --with-openssl --with-zlib --with-zip --with-sodium
--with-libxml --with-sqlite3 --with-pdo-sqlite --with-iconv
```

### 3.3 Post-Build Modifications

The build script applies three critical post-build fixes:

1. **Strip `-fvisibility=hidden` from Makefile** — After `./configure` completes
   but before `make`, the script removes occurrences of `-fvisibility=hidden`
   from the generated Makefile. This ensures `execute_ex`, `zend_execute_ex`,
   and other symbols are exported in libphp.so's dynamic symbol table.

2. **Patch `php_config.h` glob/globfree C++ linkage** — PHP's `./configure`
   generates prototypes for `glob()` and `globfree()` in `php_config.h` without
   `extern "C"` guards. When `php_config.h` is included from C++ translation
   units (e.g., `libphp_wrapper.cpp`, `bridge_jni.cpp`), this causes:

   ```
   error: declaration of 'glob' has a different language linkage
   ```

   The build script wraps these prototypes in `extern "C" { }` guards.

3. **Add `DT_NEEDED: libphp.so` to opcache.so** — PHP builds opcache.so without
   an explicit dependency on libphp.so (it relies on RTLD_GLOBAL scope). The
   build script uses `patchelf --add-needed libphp.so` to inject this dependency
   so Bionic's linker directly resolves opcache.so's 426+ undefined symbols
   from libphp.so. Also removes stale RUNPATH entries from the build machine.

### 3.4 Produced Artifacts

| Artifact                 | Size    | Location in repo                                                       |
| ------------------------ | ------- | ---------------------------------------------------------------------- |
| `libphp.so`              | ~40 MB  | `resources/androidstudio/app/src/main/jniLibs/arm64-v8a/`              |
| `opcache.so`             | ~1.8 MB | `resources/androidstudio/app/src/main/jniLibs/arm64-v8a/`              |
| PHP headers              | —       | `resources/androidstudio/app/src/main/cpp/include/php/`                |
| `php_config.h` (patched) | —       | Two copies: `include/php_config.h` AND `include/php/main/php_config.h` |

### 3.5 Symbol Verification

The build script verifies the following symbols exist in libphp.so's dynamic
symbol table:

- `php_embed_init`, `php_embed_shutdown`
- `php_module_startup`, `php_module_shutdown`
- `php_request_startup`, `php_request_shutdown`
- `execute_ex`, `zend_execute_ex` (required for OPcache)
- `ts_resource` (required for ZTS threading)
- `tsrm_startup`, `ts_free_thread` (TSRM lifecycle)
- `zend_execute_scripts`, `zend_eval_string`

---

## 4. Native C Modules (JNI Layer)

All C source files live in `resources/androidstudio/app/src/main/cpp/` and are
compiled into `libphp_wrapper.so` via CMake.

### 4.1 Module Inventory

| File                           | Lines      | Purpose                                                                                                                                                                                                                                                                |
| ------------------------------ | ---------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `php_engine.c` / `.h`          | 1188 / 89  | Process-wide PHP engine singleton. `php_engine_init()` with RTLD_GLOBAL probe, OPcache dlopen, `php_embed_init()`, warmup pass, TLS-routed output. `fix_opcache_tls_cache()`, `fix_opcache_per_thread_state()`.                                                        |
| `php_thread_context.c` / `.h`  | 129        | Per-thread TSRM attachment/detachment. `ts_resource(0)` + `TSRMLS_CACHE_UPDATE()`. Defines `TSRMLS_CACHE_DEFINE()` for its own translation unit to avoid emutls descriptor conflicts.                                                                                  |
| `php_request_context.c` / `.h` | 1038 / 182 | Per-job isolated execution context. `php_request_create()`, `php_request_execute()`, `php_request_destroy()`. Per-thread output buffers, cooperative cancellation via Zend VM interrupt, HTTP `$_SERVER` injection, job status/type enums, priority field.             |
| `worker_pool.c` / `.h`         | 740        | Native pthread pool (1–8 workers). Job queue (condvar-based), completion tracking, staggered starts, circuit breaker, UI-lane priority yielding, `worker_pool_wake()` for immediate dispatch.                                                                          |
| `scheduler_gate.c` / `.h`      | 67         | Mutex ensuring `schedule:run` never overlaps. Uses atomic CAS; new ticks are rejected (not queued) while one is running.                                                                                                                                               |
| `supervisor.c` / `.h`          | 733        | Top-level orchestrator. Owns engine + pool + gate lifecycle, job ID generation, structured JSON logging, circuit breaker config, memory limit config, `supervisor_status_json()`. Default memory limit: `256M`.                                                        |
| `bridge_jni.cpp`               | 196        | JNI bridge for the `BridgeRouterKt` can/call API. Caches `nativePHPCan` and `nativePHPCall` method IDs at init; called by `libphp_wrapper.so` for native↔PHP capability routing.                                                                                       |
| `libphp_wrapper.cpp`           | 121        | libphp.so dlopen wrapper. Re-opens `libphp.so` and `libcompat.so` via `RTLD_GLOBAL` in a C++ constructor. Also re-opens `libphp_wrapper.so` itself with `RTLD_GLOBAL` to export symbols to PHP extensions.                                                             |
| `zts_guard.h`                  | 46         | `#error` if ZTS is not defined at compile time. `nativephp_verify_zts_runtime()` for belt-and-suspenders check.                                                                                                                                                        |
| `sqlite_pool.c` / `.h`         | ~280       | Lightweight SQLite connection pool. Pre-opens N WAL connections; worker threads borrow/return via condvar. **Disabled by default** (`NATIVEPHP_ENABLE_SQLITE_MODULES=OFF`). Requires `sqlite3.h` in include path.                                                      |
| `native_queue.c` / `.h`        | ~310       | Zero-PHP-overhead queue introspection. Queries `jobs` table directly via SQLite C API. **Disabled by default** (same CMake flag). When enabled, avoids PHP bootstrap for empty queues.                                                                                 |
| `compat/android_compat.cpp`    | ~110       | Bionic compatibility shim (`getdtablesize()`, `copy_file_range()` polyfills) **plus RTLD_GLOBAL preloader** for libphp.so via `JNI_OnLoad`. Primary mechanism for OPcache symbol visibility. No glob/globfree polyfill (those are handled via `php_config.h` patches). |
| `PHP.c` / `.h`                 | —          | Legacy single-request PHP execution for UI process HTTP serving.                                                                                                                                                                                                       |

### 4.2 RTLD_GLOBAL Preloader (in `android_compat.cpp`)

The most critical piece of the OPcache integration lives in `android_compat.cpp`'s
`JNI_OnLoad()` function. When `System.loadLibrary("compat")` is called (the very
first native library loaded), `JNI_OnLoad` runs and:

1. Uses `dladdr()` on a local static function (`compat_marker()`) to discover the directory containing the native libraries
2. Constructs the path to `libphp.so` in the same directory
3. Calls `dlopen(libphp.so, RTLD_NOW | RTLD_GLOBAL)` — **before** `System.loadLibrary("php")` runs
4. Verifies `execute_ex` and `zend_execute_ex` are resolvable via `dlsym(RTLD_DEFAULT, ...)`

When `System.loadLibrary("php")` subsequently runs, Bionic's linker finds the
already-loaded libphp.so soinfo and reuses it — preserving the RTLD_GLOBAL flag.
Extensions like opcache.so can then resolve their undefined symbols from global scope.

See §9 for the full explanation of why this is necessary and the bugs we fixed.

### 4.3 PhpEngine Init Sequence (described in `php_engine.h`)

> **Status:** `php_engine.c` (1188 lines) is committed and compiled into `libphp_wrapper.so`. The interface is fully defined in `php_engine.h` (89 lines, including `fix_opcache_tls_cache()` and `fix_opcache_per_thread_state()` for Android emutls).

```
php_engine_init(ini_path, ini_entries, app_base_path)
│
├── 1. pthread_mutex_lock(&s_engine_mutex)    ← singleton guard
├── 2. #ifndef ZTS → compile error            ← belt-and-suspenders
├── 3. dladdr(php_embed_init)                 ← find libphp.so directory
│
├── 4. RTLD_GLOBAL fallback (belt-and-suspenders):
│      dlopen(libphp.so, RTLD_NOW | RTLD_GLOBAL)
│      └── Primary RTLD_GLOBAL is applied by android_compat.cpp JNI_OnLoad
│          (loaded via System.loadLibrary("compat") BEFORE "php")
│          This dlopen is a harmless no-op re-open for verification
│
├── 5. OPcache probe:
│      dlopen(opcache.so, RTLD_NOW | RTLD_GLOBAL)
│      └── Loaded before php_embed_init so Zend picks it up as a
│          zend_extension during module startup
│
├── 6. Build INI entries string:
│      extension_dir=<auto-detected>
│      zend_extension=<opcache_path>
│      opcache.enable=1, opcache.enable_cli=1, ...
│      opcache.file_cache=<storage>/framework/opcache  (hybrid SHM+disk)
│      memory_limit=256M, max_execution_time=300, ...
│
├── 7. php_embed_init(1, {"php"})
│      └── tsrm_startup → sapi_startup → php_module_startup → php_request_startup
│
├── 8. php_request_shutdown(NULL)             ← shut down main thread's request
│      └── Engine (module) stays alive
│
├── 9. sapi_module.ub_write = php_request_ub_write
│      └── All future output goes through TLS-routed per-thread buffers
│
├── 10. sapi_module.log_message = android_sapi_log_message
│       └── error_log() → Android logcat (stderr is /dev/null on Android)
│
├── 11. setenv() for process-wide constants:
│       PHP_SELF, HTTP_HOST, APP_URL, ASSET_URL, NATIVEPHP_RUNNING, COLUMNS
│
├── 12. OPcache warmup (optional, best-effort):
│       php_request_startup()
│       zend_eval_string("require autoload.php; require Application.php; ...")
│       php_request_shutdown()
│       └── Populates OPcache SHM so worker threads have cached bytecode
│           on their very first real job
│
└── 13. pthread_mutex_unlock; return 0
```

### 4.4 Worker Thread Lifecycle (`worker_pool.c` + `php_thread_context.c`)

```
worker_thread_func(pool, worker_id)
│
├── 1. Staggered start:
│      if (worker_id > 0):
│          condvar_timedwait(worker_id * 2 seconds)
│      └── Reduces SQLite contention + memory spikes during bootstrap
│
├── 2. php_thread_attach()
│      ├── ts_resource(0)         ← allocates per-thread ZTS globals
│      └── TSRMLS_CACHE_UPDATE()  ← updates TLS cache pointer
│
├── 3. LOOP while pool->running:
│   │
│   ├── UI priority check:
│   │   if supervisor_is_ui_request_active():
│   │       yield 25ms (condvar_timedwait)
│   │       continue
│   │
│   ├── Wait for job:
│   │   pthread_cond_timedwait(queue_cond, 1s)
│   │
│   ├── Dequeue job (php_request_context_t)
│   │
│   ├── Execute:
│   │   ├── php_request_execute(ctx)
│   │   │   ├── php_request_startup()
│   │   │   ├── Set $_SERVER vars (thread-safe, not setenv())
│   │   │   ├── zend_execute_scripts(ZEND_REQUIRE, script_path)
│   │   │   ├── Capture stdout → ctx->stdout_buf via TLS ub_write
│   │   │   └── php_request_shutdown(NULL)
│   │   │
│   │   ├── On success:
│   │   │   consecutive_crashes = 0
│   │   │   supervisor_notify_job_completed(job_id)
│   │   │
│   │   └── On failure:
│   │       consecutive_crashes++
│   │       supervisor_notify_job_failed(job_id, error)
│   │       if (consecutive_crashes >= circuit_breaker_max):
│   │           backoff = base * 2^(crashes - max)  // capped at 5 min
│   │           condvar_timedwait(backoff seconds)
│   │
│   └── Move ctx to completed_head (for await)
│
└── 4. php_thread_detach()
       └── ts_free_thread()
```

### 4.5 Per-Job Isolation (`php_request_context.c` / `.h`)

Each job gets a `php_request_context_t` struct with:

- **Isolated output buffers** — `stdout_buf` and `stderr_buf` are thread-local;
  `php_request_ub_write()` routes output to the correct buffer via `__thread`
  pointer
- **Status tracking** — `JOB_STATUS_PENDING` → `RUNNING` → `COMPLETED`/`FAILED`/`CANCELLED`
- **Timing** — `startedAt` and `endedAt` (epoch milliseconds)
- **Cooperative cancellation** — Atomic cancelled flag + Zend VM interrupt
  (`php_request_interrupt_handler`) triggers `zend_bailout` at next opcode boundary
- **HTTP request info** — For UI-lane requests: method, URI, query string, headers
  injected into `$_SERVER` after `php_request_startup()`
- **Job priority** — Priority field for queue ordering at the native pool level
- **JSON serialisation** — `php_request_to_json(ctx)` builds the result JSON that
  Kotlin reads via `supervisor_await_job()`

### 4.6 Optional Modules

#### SQLite Connection Pool (`sqlite_pool.c`)

Pre-opens N WAL-mode SQLite connections (configured with `busy_timeout=5000`,
`synchronous=NORMAL`, `cache_size=-4000`). Worker threads borrow connections via
`sqlite_pool_acquire()` (blocking condvar with timeout) and return them via
`sqlite_pool_release()`.

**Status:** Implemented and integrated with supervisor. Behind CMake flag
`NATIVEPHP_ENABLE_SQLITE_MODULES`. Requires `sqlite3.h` in the include path.

#### Native Queue Peek (`native_queue.c`)

Provides zero-PHP-overhead queue introspection. Opens a read-only SQLite
connection and queries `SELECT COUNT(*) FROM jobs WHERE reserved_at IS NULL AND
available_at <= ?`. If the queue is empty, the caller skips the expensive PHP
bootstrap entirely.

**Status:** Implemented and wired into `supervisor.c`'s `enqueue_queue_job()`.
Behind same CMake flag.

---

## 5. Kotlin Android Layer

### 5.1 PhpWorkerService (`worker/PhpWorkerService.kt`)

Android Foreground Service (type `dataSync`) that:

- Acquires `PARTIAL_WAKE_LOCK` (6-hour timeout aligned with Android 14 FGS limit)
- Creates a persistent notification for the foreground service
- Calls `supervisor_engine_init()` via JNI on start
- Calls `supervisor_start()` with configured worker count, queues, connection
- Runs two periodic polling loops:
  - **Queue poll** (default every 5s): `supervisor_enqueue_queue_job(NULL)` + `supervisor_await_job(job_id, 60000)`
  - **Scheduler tick** (default every 60s): `supervisor_enqueue_scheduler_tick(NULL)` + await
- On foreground dispatch: `supervisor_wake_workers()` for <500ms latency
- Returns `START_STICKY` for auto-restart if killed by Android

### 5.2 Library Load Order (`PHPBridge.kt` + `PhpSupervisorBridge.kt`)

Both Kotlin bridge classes load native libraries in the same critical order:

```kotlin
init {
    System.loadLibrary("compat")       // JNI_OnLoad pre-loads libphp.so with RTLD_GLOBAL
    System.loadLibrary("php")          // Bionic finds already-loaded, keeps RTLD_GLOBAL
    System.loadLibrary("php_wrapper")  // Supervisor, worker pool, JNI bridge
}
```

The `PhpSupervisorBridge` wraps its load in a try/catch for `UnsatisfiedLinkError`
since `PHPBridge` may have already loaded the libraries.

### 5.3 PhpPeriodicWorker (WorkManager)

Alternative to FGS for periodic background work on Android 14+ where
`dataSync` FGS has a 6-hour limit. Uses `CoroutineWorker` with
`withContext(Dispatchers.IO) { .get() }` instead of `.await()` for Kotlin
compilation compatibility.

### 5.4 LaravelEnvironment.kt — `setEnvironmentVariable()` Fix

**Problem:** `setupEnvironment()` runs on a background thread ~12s after engine
init. `nativeSetEnv(APP_KEY, ...)` returns -1 (blocked — engine already running,
`setenv()` is not thread-safe). Old code threw `RuntimeException` → process crash.

**Fix:** Log warning instead of throwing. Workers read env from `.env` file.

---

## 6. PHP Framework Layer

### 6.1 WorkerServiceProvider (`src/Worker/WorkerServiceProvider.php`)

Auto-discovered by Laravel. **Requires zero developer configuration.** Handles:

#### SQLite WAL Mode

Listens on `ConnectionEstablished` event and sets PRAGMAs in the correct order:

```php
$event->connection->statement('PRAGMA busy_timeout=5000');  // FIRST
$event->connection->statement('PRAGMA journal_mode=WAL');
$event->connection->statement('PRAGMA synchronous=NORMAL');
$event->connection->statement('PRAGMA wal_autocheckpoint=100');
$event->connection->statement('PRAGMA cache_size=-8000');
```

**Critical:** `busy_timeout` MUST be set before `journal_mode=WAL`. Otherwise,
the WAL upgrade returns `SQLITE_BUSY` immediately if another thread holds a lock.

#### Lane Detection

The provider uses lane detection to avoid running worker-only code during HTTP
requests or migration commands:

- `isHeadlessWorkerLane()` — returns true for console/worker/scheduler contexts
- `isWorkerOrSchedulerLane()` — returns true only when `NATIVEPHP_JOB_TYPE` is
  `queue` or `scheduler` (not general artisan commands)

#### Serialized First-Boot Tasks

Multiple worker threads call `boot()` simultaneously. The provider uses
`flock(LOCK_EX)` (not PHP `static` — which is per-thread in ZTS) to serialise:

1. `ensureQueueTablesExist()` — `CREATE TABLE IF NOT EXISTS` for `jobs`,
   `failed_jobs`, `job_batches` (atomic at SQLite level)
2. `WorkerErrorReporter::ensureTableExists()` — creates `worker_errors` table
3. `ensureConfigCached()` — serialises the already-resolved config array to
   `bootstrap/cache/config.php` via atomic temp-file + rename (avoids
   `Artisan::call('config:cache')` which would create a fresh Application and
   cause infinite recursion)

#### Queue Defaults

- Auto-promotes `sync` → `database` driver (configurable via `override_sync_driver`)
- Sets `retry_after=90` if not configured
- Optional Redis queue backend when `redis_enabled=true`

### 6.2 WorkerConfig (`src/Worker/WorkerConfig.php`)

Static accessor class with environment-variable-first resolution:

```php
WorkerConfig::connection()    // Queue connection name
WorkerConfig::queues()        // Comma-separated queue names
WorkerConfig::workerCount()   // Number of threads (clamped to hard cap of 4)
WorkerConfig::mode()          // 0=all, 1=queue, 2=scheduler
WorkerConfig::autoStart()     // Auto-start on app launch
WorkerConfig::memoryLimit()   // Per-worker PHP memory_limit
WorkerConfig::opcacheEnabled() // OPcache SHM toggle
// ... etc
```

### 6.3 WorkerErrorReporter (`src/Worker/WorkerErrorReporter.php`)

Unified error reporting for worker threads. Captures exceptions into a
`worker_errors` SQLite table:

```php
WorkerErrorReporter::capture($exception, ['job_type' => 'queue', 'queue' => 'default']);
WorkerErrorReporter::recent(10);           // Latest 10 errors
WorkerErrorReporter::summary();            // Grouped by error_class
WorkerErrorReporter::count(withinMinutes: 5);
WorkerErrorReporter::pruneOlderThan(days: 7);
WorkerErrorReporter::flush();
```

### 6.4 WorkerMetrics (`src/Worker/WorkerMetrics.php`)

Dashboard data via two sources:

- **Native bridge**: `nativephp_supervisor_status()` / `nativephp_queue_status()` for live stats
- **Fallback**: PHP-side queries when native bridge unavailable

Key methods:

- `WorkerMetrics::snapshot()` — Live supervisor status
- `WorkerMetrics::queueStatus()` — Per-queue depths and stats
- `WorkerMetrics::dashboard()` — Composite: supervisor + queue + errors + db pool
- `WorkerMetrics::throughput($windowMinutes)` — Jobs per minute over sliding window

### 6.5 Bootstrap Entrypoints

#### `bootstrap/worker/common.php` (shared bootstrap)

Extracted from duplicate code in `queue_worker.php` and `scheduler_tick.php`.
Provides:

- **Debug logging** — `worker_diag()` function gated by `NATIVEPHP_DEBUG` env var
- **Autoloader detection** — probes 3 standard paths for `vendor/autoload.php`
- **Path resolution** — locates `bootstrap/app.php`, resolves storage/cache paths
  (supports `LARAVEL_STORAGE_PATH` and persisted_data layout)
- **Directory creation** — ensures `storage/framework/{views,cache,sessions}`,
  `storage/logs`, `bootstrap/cache` exist
- **Thread-safe superglobal assignment** — sets `$_ENV` and `$_SERVER` (never
  `putenv()`) for storage paths, bootstrap paths, artisan paths
- **fork() deadlock prevention** — pre-sets `Terminal::$width`/`$height` via
  Reflection so `Terminal::getWidth()` never calls `proc_open('stty')` (see Bug 9)
- **Memory limit enforcement** — reads `NATIVEPHP_WORKER_MEMORY_LIMIT` and calls `ini_set()`
- **Application bootstrap** — creates app, kernel, calls `kernel->bootstrap()`

#### `bootstrap/worker/queue_worker.php`

- Includes `common.php` for bootstrap
- Pops ONE job from the queue (tries each queue in order)
- Fires the job, marks as deleted/failed
- Reports failures to `WorkerErrorReporter`
- Outputs JSON: `{ "processed": bool, "job_name": string|null, "error": string|null, "duration_ms": int }`
- Uses output buffering to suppress accidental output (16KB chunk handler)

#### `bootstrap/worker/scheduler_tick.php`

- Includes `common.php` for bootstrap
- Calls `$kernel->call('schedule:run', ...)` with `NullOutput` and `--quiet`
- Reports failures to `WorkerErrorReporter`
- Outputs JSON: `{ "ran": bool, "output": string, "error": string|null, "duration_ms": int }`

### 6.6 Thread-Safety Patterns in PHP

| Pattern                    | Safe?         | Why                                                                 |
| -------------------------- | ------------- | ------------------------------------------------------------------- |
| `$_ENV['KEY'] = $val`      | ✅            | Per-thread superglobal in ZTS                                       |
| `$_SERVER['KEY'] = $val`   | ✅            | Per-thread superglobal in ZTS                                       |
| `putenv('KEY=val')`        | ❌ **NEVER**  | Mutates process-global env table; races with other threads          |
| `getenv('KEY')`            | ✅ (read)     | Reads process-global env; safe if set before threads start          |
| `static $flag`             | ⚠️ Per-thread | Cannot guard cross-thread races (TSRM allocates per-thread storage) |
| `flock(LOCK_EX)`           | ✅            | Filesystem lock; works across threads and processes                 |
| `ini_set()`                | ✅ Per-thread | Each thread has its own INI values                                  |
| `Artisan::call(...)`       | ⚠️            | Creates fresh Application instance; can cause recursion             |
| `config(['key' => 'val'])` | ✅ Per-thread | Config repository is per-thread in ZTS                              |
| `fork()`/`proc_open()`     | ❌ **NEVER**  | Deadlocks in multithreaded process (see Bug 9)                      |

---

## 7. CMake Build System

### 7.1 Build Configuration (`CMakeLists.txt`)

The CMake file at `resources/androidstudio/app/src/main/cpp/CMakeLists.txt`:

1. **Compiles `libcompat.so`** from `android_compat.cpp`, linking against
   `android`, `log`, `dl`. This library contains the RTLD_GLOBAL preloader in its
   `JNI_OnLoad`.
2. **Imports `libphp.so`** as a prebuilt IMPORTED library from `jniLibs/arm64-v8a/`.
3. **Compiles `libphp_wrapper.so`** from all C/C++ sources, linking against
   `compat`, `android`, `log`, `dl`, `php`.
4. **ZTS build check** — reads `include/php_config.h` and emits `FATAL_ERROR`
   if `/* #undef ZTS */` is found (catches accidental NTS binary swap).
5. **Glob C++ linkage fix** — auto-patches `php_config.h` at configure time
   to wrap `glob()`/`globfree()` prototypes in `extern "C" {}` guards.
6. **Optional SQLite modules** — behind `NATIVEPHP_ENABLE_SQLITE_MODULES` flag.

### 7.2 The Two php_config.h Problem

There are **two copies** of `php_config.h` in the include tree:

```
cpp/include/php_config.h           ← included via -I include/
cpp/include/php/main/php_config.h  ← included by PHP headers via relative path
```

Both are auto-generated by PHP's `./configure` and contain bare C prototypes for
`glob()` and `globfree()`. When included from C++ files, these get C++ linkage,
conflicting with `<glob.h>` which provides C linkage. **Both copies must be
patched** or the C++ compiler sees conflicting linkage declarations.

The fix exists in three places (defense in depth):

1. **Build script** — patches during `install_to_project()` and copies to both paths
2. **CMakeLists.txt** — patches at configure time (catches `native:run` regeneration)
3. **.cxx cache deletion** — when updating CMakeLists.txt, the `.cxx` folder must be
   deleted to force CMake to reconfigure

---

## 8. Build Script — Cross-Compiling PHP

`scripts/build_php_android_arm64.sh` (~616 lines) cross-compiles PHP for
Android arm64 with ZTS and OPcache. Key features:

### 8.1 Configuration

| Variable             | Default     | Description                                               |
| -------------------- | ----------- | --------------------------------------------------------- |
| `PHP_VERSION`        | `php-8.4.5` | PHP version tag (any php-8.4.x)                           |
| `API`                | `24`        | Android API level                                         |
| `ANDROID_NDK_HOME`   | auto-detect | NDK r27 path                                              |
| `DEPS_PREFIX`        | auto-detect | Pre-built deps (iconv, curl, openssl, etc.)               |
| `SKIP_BUILD`         | `0`         | Set to `1` to reuse existing build; verify + install only |
| `INSTALL_TO_PROJECT` | `1`         | Copy artifacts into the mobile-air repo                   |
| `EMIT_JNILIBS_ZIP`   | `1`         | Create pipeline artifact zip                              |

### 8.2 Pipeline Stages

1. **`prepare_source`** — Clone PHP source at the specified tag
2. **`patch_source_for_android`** — Android-specific patches
3. **`configure_env`** — Set up NDK toolchain, CFLAGS, configure args
4. **`build_php`** — `./configure` → strip `-fvisibility=hidden` from Makefile → `make -j$(nproc)`
5. **`find_outputs`** — Locate libphp.so and opcache.so in build tree
6. **`verify_symbols`** — Check for required symbols in `.dynsym` table
7. **`verify_pair_origin`** — Confirm both .so files come from the same build
8. **`patch_opcache_dt_needed`** — Add `DT_NEEDED: libphp.so` to opcache.so via patchelf, remove stale RUNPATH
9. **`stage_pipeline_layout`** — Create `jniLibs/arm64-v8a/` staging area
10. **`install_to_project`** — Copy to mobile-air repo:
    - `libphp.so` + `opcache.so` → `jniLibs/arm64-v8a/`
    - Headers → `cpp/include/php/`
    - Patched `php_config.h` → **both** copy locations
11. **`emit_jnilibs_zip`** — ZIP artifact for CI pipelines

### 8.3 Self-Healing Features

- **CRLF self-healing** — Detects `\r` bytes and re-executes via `tr -d '\r'`
  (Git on Windows often injects CRLF)
- **DEPS_PREFIX auto-detection** — Probes `~/php-deps/build-arm64-v8a`,
  `$WORK_DIR/deps/build-arm64-v8a`, `/opt/php-deps/build-arm64-v8a`

---

## 9. OPcache: The RTLD_GLOBAL Problem and Solution

This section documents the most complex bug we encountered and the multi-layer
solution we implemented.

### 9.1 The Problem

OPcache (`opcache.so`) is a PHP extension that dramatically improves performance
by caching compiled bytecode in shared memory. It works by hooking two Zend
Engine functions: `execute_ex` and `zend_execute_ex`. At load time, it resolves
these symbols from the host process's global symbol table.

On Android, the loading chain is:

```
System.loadLibrary("php")  →  dlopen(libphp.so, RTLD_LOCAL)
```

Android's `System.loadLibrary()` always uses `RTLD_LOCAL`, which means libphp.so's
symbols are **not visible** in the global symbol table. When opcache.so is later
loaded via `dlopen()`, it has 426+ undefined symbols (including `execute_ex` and
`zend_execute_ex`) that cannot be resolved → `dlopen` fails:

```
dlopen probe for opcache.so failed: cannot locate symbol "execute_ex"
```

### 9.2 Why Post-Load Promotion Doesn't Work on API 36+

The obvious fix is to re-open libphp.so with `RTLD_GLOBAL` after it's already loaded:

```c
dlopen(libphp_path, RTLD_NOW | RTLD_GLOBAL);
```

On desktop Linux (glibc), this works — the linker finds the already-loaded library
and promotes its symbols to global scope. But on **Android's Bionic linker**
(specifically tested on API 36 / Android 16), this **silently fails**:

- `dlopen()` returns a non-NULL handle (looks like success)
- But the library's symbols remain in RTLD_LOCAL scope
- opcache.so still fails to load

This was especially hard to debug because `dlsym(RTLD_DEFAULT, "execute_ex")` also
returned a non-NULL pointer, appearing to confirm that promotion worked. However,
**Bionic's `RTLD_DEFAULT` searches ALL loaded objects** including RTLD_LOCAL ones — so
this check is a false positive and does not prove the symbol is in global scope.

An earlier fix attempt also used `RTLD_NOLOAD | RTLD_GLOBAL`, which on Bionic
returns the existing handle without modifying any flags at all. This was Bug 10.

### 9.3 The Solution: Three-Layer Approach

We implemented a belt-and-suspenders strategy with three independent layers:

#### Layer 1 — `android_compat.cpp` JNI_OnLoad (Primary, Runtime)

The key insight: load libphp.so with `RTLD_GLOBAL` **before** `System.loadLibrary("php")`
loads it with `RTLD_LOCAL`. When Bionic later processes `System.loadLibrary("php")`,
it finds the already-loaded soinfo with `RTLD_GLOBAL` and preserves that flag.

This is implemented in `android_compat.cpp`'s `JNI_OnLoad()`:

```c
// In JNI_OnLoad (runs when System.loadLibrary("compat") is called):
void *handle = dlopen(php_path, RTLD_NOW | RTLD_GLOBAL);
// Now when System.loadLibrary("php") runs, Bionic finds the already-loaded
// libphp.so and keeps RTLD_GLOBAL.
```

The Kotlin load order in `PHPBridge.kt`:

```kotlin
System.loadLibrary("compat")       // → JNI_OnLoad loads libphp.so RTLD_GLOBAL
System.loadLibrary("php")          // → Bionic finds already-loaded, preserves RTLD_GLOBAL
System.loadLibrary("php_wrapper")  // → supervisor, bridge, etc.
```

#### Layer 2 — patchelf DT_NEEDED (Belt-and-Suspenders, Build-Time)

The build script uses `patchelf --add-needed libphp.so` to add an explicit
`DT_NEEDED` entry to opcache.so. This makes Bionic resolve opcache.so's undefined
symbols directly from libphp.so via the dependency chain, regardless of
RTLD_GLOBAL scope. Also removes stale RUNPATH entries from the build machine.

#### Layer 3 — `php_engine.c` dlopen fallback (Legacy, Older API Levels)

A belt-and-suspenders `dlopen(libphp.so, RTLD_NOW | RTLD_GLOBAL)` call in
`php_engine.c` acts as a fallback for devices where post-load promotion does
work (pre-API 36 Bionic).

### 9.4 How to Verify OPcache is Working

Check logcat for these messages:

```
# Success:
Compat: libphp.so loaded with RTLD_GLOBAL — execute_ex=0x..., zend_execute_ex=0x...
PhpEngine: dlopen probe for opcache.so succeeded

# Failure:
PhpEngine: dlopen probe for opcache.so failed: cannot locate symbol "execute_ex"
```

On HTTP requests, check the timing header:

```
X-PHP-Timing: opcache=AVAILABLE,autoload=52ms,...    ← working
X-PHP-Timing: opcache=NOT_AVAILABLE,autoload=800ms,... ← not working
```

### 9.5 Historical Note: php_preloader.c (Removed)

The first fix attempt created a separate `php_preloader.c` library with the same
JNI_OnLoad logic. This worked in principle but crashed the app on launch because
the separate `libphp_preloader.so` wasn't being packaged into the deployed APK
(the NativePHP template-copy process didn't include the new library target).

The solution was to move the preloader logic into the already-existing
`android_compat.cpp` (compiled into `libcompat.so`), which is already built,
linked, and deployed. **`php_preloader.c` has been removed from the repository.**
The RTLD_GLOBAL preloader lives exclusively in `compat/android_compat.cpp`'s
`JNI_OnLoad`. See §4.1 and §4.2 for the current canonical implementation.

---

## 10. Bugs Found & Fixed

### Bug 1: Process Crash from `nativeSetEnv` RuntimeException

- **Root Cause:** `LaravelEnvironment.setupEnvironment()` runs on a background
  thread ~12s after engine init. `nativeSetEnv(APP_KEY)` returns -1 (blocked;
  engine already running means `setenv()` is unsafe). Old code threw
  `RuntimeException` → process killed → all workers die.
- **Fix:** Log warning instead of throwing. Workers read env from `.env` file.
- **File:** `LaravelEnvironment.kt`

### Bug 2: SQLite PRAGMA Ordering

- **Root Cause:** `PRAGMA journal_mode=WAL` was set before `PRAGMA busy_timeout`.
  If another thread held a lock, WAL upgrade returned `SQLITE_BUSY` immediately.
- **Fix:** Set `busy_timeout=5000` FIRST, then `journal_mode=WAL`.
- **File:** `WorkerServiceProvider.php` → `configureSqliteWal()`

### Bug 3: Config Cache Infinite Recursion

- **Root Cause:** `Artisan::call('config:cache')` creates a fresh Application →
  boots WorkerServiceProvider → calls `ensureConfigCached()` → calls
  `config:cache` → infinite recursion.
- **Fix (v1):** `static $inProgress` guard. **(Insufficient — per-thread in ZTS.)**
- **Fix (v2):** Replaced with lightweight config serialisation that reads the
  already-resolved config array and writes to `bootstrap/cache/config.php` via
  temp file + rename. No fresh Application, no Artisan call.
- **File:** `WorkerServiceProvider.php` → `ensureConfigCached()`

### Bug 4: TOCTOU Race on Queue Table Creation

- **Root Cause:** 3 threads simultaneously call `Schema::hasTable('jobs')` → all
  see false → all call `Schema::create('jobs')` → 2 of 3 fail with "table already
  exists".
- **Fix:** Replaced with `CREATE TABLE IF NOT EXISTS` (atomic at SQLite level) +
  `flock(LOCK_EX)` serialisation.
- **File:** `WorkerServiceProvider.php` → `ensureQueueTablesExist()`

### Bug 5: OPcache `execute_ex` Symbol Not Found (RTLD_LOCAL)

- **Root Cause:** Android's `System.loadLibrary()` loads libphp.so with
  `RTLD_LOCAL`. OPcache needs `execute_ex` from libphp.so but can't resolve it.
- **Initial fix attempt:** Re-open libphp.so with `RTLD_GLOBAL | RTLD_NOLOAD` —
  appeared to work but actually didn't promote flags (see Bug 10).
- **Final fix:** Three-layer approach described in §9.
- **File:** `android_compat.cpp`, `php_engine.c`, build script

### Bug 6: ZTS `static` Variables Are Per-Thread

- **Root Cause:** PHP `static` variables are per-thread in ZTS mode (TSRM
  allocates separate storage for each thread). Guards like `static $done = false`
  cannot prevent cross-thread races.
- **Fix:** All cross-thread serialisation uses `flock(LOCK_EX)` on a lockfile.
- **File:** `WorkerServiceProvider.php` → `serializedFirstBootTasks()`

### Bug 7: glob/globfree C++ Linkage Conflict

- **Root Cause:** `php_config.h` (auto-generated) declares `glob()` and
  `globfree()` as bare C prototypes. When included from C++ translation units,
  they get C++ linkage, conflicting with `<glob.h>` which provides C linkage.
  Made worse by TWO copies of `php_config.h` in the include tree.
- **Fix:** Wrap prototypes in `extern "C" {}` guards. Applied in build script,
  CMakeLists.txt (at configure time), and manually for both copies.
- **Files:** `build_php_android_arm64.sh`, `CMakeLists.txt`, both `php_config.h` copies

### Bug 8: PhpPeriodicWorker `.await()` Compilation Error

- **Root Cause:** Kotlin's `.await()` extension on `ListenableFuture` requires
  `kotlinx-coroutines-guava` dependency. The project didn't include it.
- **Fix:** Replaced with `withContext(Dispatchers.IO) { .get() }`.
- **File:** `PhpPeriodicWorker.kt`

### Bug 9: Scheduler Tick Permanent Hang — `fork()` Deadlock

- **Root Cause:** `schedule:run` → `Task::render()` → `Terminal::getWidth()` →
  `readFromProcess(['stty', '-a'])` → `proc_open()` → `fork()`.
  In a process with ~65 threads, the forked child inherits locked mutexes
  from all threads. The child deadlocks before reaching `exec('stty')`.
  The parent blocks forever on `read()` from the error pipe. Worker thread
  hangs permanently, scheduler gate remains "busy", all future ticks skipped.

  **Evidence discovered during debugging:**
  - Child PID showed `Threads: 1`, `cmdline: com.openviewgroup.engineeringapp`
    (exec never happened), `voluntary_ctxt_switches: 1` (barely ran)
  - Worker thread stuck at `step=7 BEFORE php_execute_script` for 20+ minutes
  - Scheduler gate remained "busy" — all future ticks skipped
  - Queue jobs on other workers continued fine (no `Task::render()` call)

- **Fix (primary):** In `common.php`, pre-set `Terminal::$width`/`$height` via
  Reflection after autoloader loads. This skips `initDimensions()` entirely.
- **Fix (backup):** In `php_engine.c`, `setenv("COLUMNS", "80", 0)` during
  engine init (single-threaded). `Terminal::getWidth()` checks `getenv('COLUMNS')`
  first and returns early.
- **Files:** `bootstrap/worker/common.php`, `php_engine.c`

### Bug 10: RTLD_NOLOAD Silently Ignores Flag Changes on Bionic

- **Root Cause:** The Bug 5 fix used `RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD`.
  On Bionic, `RTLD_NOLOAD` returns the existing handle **without modifying flags**.
  The library stays `RTLD_LOCAL` and opcache.so still can't resolve symbols.
  `dlsym(RTLD_DEFAULT)` was a false positive — it searches ALL loaded objects
  including RTLD_LOCAL ones on Bionic.
- **Fix:** Remove `RTLD_NOLOAD`. Then discovered that even without `RTLD_NOLOAD`,
  API 36+ Bionic doesn't reliably promote. Final fix: pre-load with RTLD_GLOBAL
  in `android_compat.cpp`'s `JNI_OnLoad` (see §9).
- **Files:** `android_compat.cpp`, `php_engine.c`

---

## 11. Performance Observations

| Metric                | Without OPcache         | With OPcache (expected) |
| --------------------- | ----------------------- | ----------------------- |
| Autoloader load       | ~300-880ms              | ~50-100ms               |
| Total per-request     | ~1300-2400ms            | ~300-500ms              |
| First boot total      | ~10s from process start | ~4-6s                   |
| Memory per thread     | ~24MB                   | ~20MB (shared bytecode) |
| Job execution (first) | ~4000ms                 | ~1500-2000ms            |

### Key Performance Levers

1. **OPcache SHM** — Largest single improvement. Eliminates per-thread recompilation of ~200+ Laravel framework files.
2. **OPcache File Cache** — Hybrid SHM+disk mode. Cold boot avoids recompiling files from SHM loss after process restart.
3. **Staggered worker starts** — 2s per worker reduces SQLite contention during bootstrap.
4. **Config caching** — Serialized config avoids re-parsing on each worker request.
5. **OPcache warmup** — Pre-compiles framework classes before workers start, so first real job is fast.
6. **Native queue peek** — Skips expensive PHP bootstrap when queue is empty (requires SQLite modules enabled).

---

## 12. Configuration Reference

### `config/nativephp-worker.php`

Published via: `php artisan vendor:publish --tag=nativephp-worker-config`

| Key                              | Env Override                             | Default                     | Description                           |
| -------------------------------- | ---------------------------------------- | --------------------------- | ------------------------------------- |
| `connection`                     | `NATIVEPHP_QUEUE_CONNECTION`             | app default                 | Queue connection name                 |
| `queues`                         | `NATIVEPHP_QUEUE_NAMES`                  | `"default"`                 | Comma-separated queue names           |
| `worker_count`                   | `NATIVEPHP_WORKER_COUNT`                 | 2 (Android) / 1 (iOS)       | Concurrent worker threads             |
| `mode`                           | `NATIVEPHP_WORKER_MODE`                  | `0`                         | 0=all, 1=queue only, 2=scheduler only |
| `scheduler_interval`             | `NATIVEPHP_SCHEDULER_INTERVAL`           | `60`                        | Seconds between `schedule:run`        |
| `queue_poll_interval`            | `NATIVEPHP_QUEUE_POLL_INTERVAL`          | `5`                         | Seconds between queue checks          |
| `auto_start`                     | `NATIVEPHP_WORKER_AUTO_START`            | auto-detect                 | Auto-start on app launch              |
| `override_sync_driver`           | `NATIVEPHP_OVERRIDE_SYNC_DRIVER`         | `true`                      | Auto-promote `sync` → `database`      |
| `max_worker_count`               | —                                        | `4`                         | Hard cap on concurrent threads        |
| `circuit_breaker_threshold`      | `NATIVEPHP_CIRCUIT_BREAKER_THRESHOLD`    | `3`                         | Consecutive crashes before backoff    |
| `circuit_breaker_backoff`        | `NATIVEPHP_CIRCUIT_BREAKER_BACKOFF`      | `5`                         | Base backoff seconds (doubles)        |
| `memory_limit`                   | `NATIVEPHP_WORKER_MEMORY_LIMIT`          | `"512M"`                    | Per-worker PHP memory limit           |
| `immediate_dispatch`             | `NATIVEPHP_WORKER_IMMEDIATE_DISPATCH`    | `true`                      | Wake workers immediately on dispatch  |
| `opcache_enabled`                | `NATIVEPHP_OPCACHE_ENABLED`              | `true`                      | Enable OPcache SHM                    |
| `opcache_memory_mb`              | `NATIVEPHP_OPCACHE_MEMORY_MB`            | `32`                        | OPcache memory in MB                  |
| `opcache_file_cache`             | `NATIVEPHP_OPCACHE_FILE_CACHE`           | `true`                      | Persist bytecode to disk              |
| `log_enabled`                    | `NATIVEPHP_WORKER_LOG_ENABLED`           | `true`                      | Structured worker.jsonl log           |
| `log_max_size_kb`                | `NATIVEPHP_WORKER_LOG_MAX_SIZE`          | `1024`                      | Max log size before rotation          |
| `config_cache_enabled`           | `NATIVEPHP_CONFIG_CACHE_ENABLED`         | `true`                      | Build-time config cache               |
| `redis_enabled`                  | `NATIVEPHP_REDIS_ENABLED`                | `false`                     | Use Redis instead of SQLite           |
| `redis_connection`               | `NATIVEPHP_REDIS_CONNECTION`             | `default`                   | Redis connection name                 |
| `android_execution_strategy`     | `NATIVEPHP_ANDROID_EXECUTION_STRATEGY`   | `auto`                      | `auto`/`foreground`/`workmanager`     |
| `workmanager_interval_minutes`   | `NATIVEPHP_WORKMANAGER_INTERVAL_MINUTES` | `15`                        | WorkManager repeat interval           |
| `db_pool_size`                   | `NATIVEPHP_DB_POOL_SIZE`                 | `4`                         | Pre-opened SQLite connections         |
| `max_background_runtime_android` | `NATIVEPHP_MAX_BG_RUNTIME_ANDROID`       | `21600` (6h)                | Android FGS time limit                |
| `max_background_runtime_ios`     | `NATIVEPHP_MAX_BG_RUNTIME_IOS`           | `25`                        | iOS BGProcessingTask limit            |
| `priority_map`                   | —                                        | `{high:5,default:0,low:-5}` | Queue name → native priority          |

---

## 13. Using Workers in Your NativePHP App

### 13.1 Zero-Configuration Setup

Workers work **out of the box** with vanilla Laravel queues. No configuration or
migration changes required. The `WorkerServiceProvider` automatically:

- Creates the `jobs`, `failed_jobs`, and `job_batches` tables if they don't exist
- Switches the `sync` queue driver to `database` so jobs are persisted
- Configures SQLite WAL mode for concurrent access
- Starts 2 queue workers + 1 scheduler thread

### 13.2 Dispatching Jobs

Use standard Laravel job dispatch — nothing NativePHP-specific:

```php
// In a controller, Livewire component, or route:
use App\Jobs\SyncData;

SyncData::dispatch($payload);

// With queue priority:
SyncData::dispatch($payload)->onQueue('high');

// Delayed:
SyncData::dispatch($payload)->delay(now()->addMinutes(5));
```

Jobs will be picked up by the background worker threads within the configured
poll interval (default 5 seconds). In the foreground, if `immediate_dispatch`
is enabled (default), jobs run within <500ms.

### 13.3 Scheduled Tasks

Define your schedule in `routes/console.php` (Laravel 11+) or
`app/Console/Kernel.php` as usual:

```php
// routes/console.php
use Illuminate\Support\Facades\Schedule;

Schedule::command('sync:check')->everyFiveMinutes();
Schedule::job(new CleanupJob)->daily();
Schedule::call(function () {
    // Inline scheduled tasks work too
    DB::table('old_records')->where('created_at', '<', now()->subMonth())->delete();
})->weekly();
```

The scheduler tick runs every 60 seconds by default (configurable via
`NATIVEPHP_SCHEDULER_INTERVAL`).

**Important:** Scheduled tasks must **not** call `fork()`, `proc_open()`,
`exec()`, or `shell_exec()`. These cause deadlocks in the multithreaded process
(see Bug 9).

### 13.4 Customising Worker Behaviour

Add to your project's `.env` file:

```env
# Increase worker threads (max 4)
NATIVEPHP_WORKER_COUNT=3

# Custom queue names
NATIVEPHP_QUEUE_NAMES=high,default,low

# Reduce poll interval for faster job pickup
NATIVEPHP_QUEUE_POLL_INTERVAL=2

# Run scheduler every 30 seconds instead of 60
NATIVEPHP_SCHEDULER_INTERVAL=30

# Enable debug logging (verbose worker diagnostics)
NATIVEPHP_DEBUG=true

# Increase per-worker memory limit
NATIVEPHP_WORKER_MEMORY_LIMIT=768M
```

Or publish and edit the config file:

```bash
php artisan vendor:publish --tag=nativephp-worker-config
```

### 13.5 Queue Priority

Configure queue priority in `config/nativephp-worker.php`:

```php
'priority_map' => [
    'critical' => 10,
    'high'     => 5,
    'default'  => 0,
    'low'      => -5,
    'bulk'     => -10,
],

'queues' => 'critical,high,default,low,bulk',
```

Jobs dispatched to higher-priority queues are dequeued first at the native
thread pool level.

### 13.6 Monitoring Worker Status

**From PHP:**

```php
use Native\Mobile\Worker\WorkerErrorReporter;
use Native\Mobile\Worker\WorkerMetrics;

// Get recent errors
$errors = WorkerErrorReporter::recent(20);

// Get error summary grouped by class
$summary = WorkerErrorReporter::summary();

// Count errors in the last 5 minutes
$count = WorkerErrorReporter::count(withinMinutes: 5);

// Live supervisor dashboard data
$dashboard = WorkerMetrics::dashboard();

// Jobs per minute over the last 5 minutes
$throughput = WorkerMetrics::throughput(5);
```

**From HTTP API:**

```
GET /_native/api/worker/status
→ { status, activeJobs, pendingJobs, completedJobs, failedJobs,
    schedulerRunning, uptimeSeconds, mode, config, queueStats }
```

**From Blade:**

```blade
{{-- Drop-in worker dashboard component --}}
<x-nativephp-worker-dashboard />
```

**From ADB:**

```bash
# Live worker logs
php artisan native:tail --type=worker

# Logcat tags
adb logcat -v time -s "PhpEngine:*" "Supervisor:*" "Compat:*" "PHP:*"
```

### 13.7 Writing Thread-Safe Jobs

Jobs run in a ZTS PHP environment. Follow these rules:

```php
class MyJob implements ShouldQueue
{
    use Dispatchable, InteractsWithQueue, Queueable, SerializesModels;

    public function handle()
    {
        // ✅ SAFE: Standard Laravel APIs
        DB::table('users')->where('id', 1)->update(['synced' => true]);
        Cache::put('key', 'value', 60);
        Log::info('Job processed');

        // ✅ SAFE: Use $_ENV for thread-local env vars
        $_ENV['MY_VAR'] = 'value';

        // ✅ SAFE: File operations with flock() for coordination
        $fp = fopen(storage_path('my.lock'), 'c+');
        flock($fp, LOCK_EX);
        // ... critical section ...
        flock($fp, LOCK_UN);
        fclose($fp);

        // ❌ DANGEROUS: Never call putenv()
        // putenv('MY_VAR=value');

        // ❌ DANGEROUS: Never use static for cross-thread guards
        // static $done = false; // Per-thread in ZTS!

        // ❌ DANGEROUS: Never call fork/exec
        // shell_exec('some-command');
        // proc_open(...);
    }
}
```

### 13.8 Handling Failures

Jobs that throw exceptions are automatically:

1. Marked as failed in the `jobs`/`failed_jobs` tables
2. Recorded in the `worker_errors` table via `WorkerErrorReporter`
3. Logged to Android logcat

The circuit breaker prevents crash loops: after 3 consecutive failures on the
same thread, the thread backs off exponentially (5s → 10s → 20s → 40s, capped
at 5 minutes). A single successful job resets the counter.

### 13.9 Events

Listen for worker lifecycle events in your app:

| Event                                  | Fired when            | Properties                                                  |
| -------------------------------------- | --------------------- | ----------------------------------------------------------- |
| `Events\Worker\WorkerStarted`          | Supervisor starts     | `workerCount`, `queues`, `mode`, `platform`, `isForeground` |
| `Events\Worker\WorkerStopped`          | Supervisor stops      | `completedJobs`, `failedJobs`, `uptimeSeconds`, `reason`    |
| `Events\Worker\JobCompleted`           | Queue job succeeds    | `jobId`, `jobName`, `durationMs`, `stdout`                  |
| `Events\Worker\JobFailed`              | Queue job fails       | `jobId`, `jobName`, `durationMs`, `error`, `stderr`         |
| `Events\Worker\CircuitBreakerTripped`  | Crash threshold hit   | `consecutiveCrashes`, `backoffSeconds`, `lastError`         |
| `Events\Worker\SchedulerTickCompleted` | schedule:run finishes | `durationMs`, `exitCode`                                    |

---

## 14. Suggestions for Improvements / Cleanup

### 14.1 Completed Items

| #   | Item                                           | Status                                                                   |
| --- | ---------------------------------------------- | ------------------------------------------------------------------------ |
| 1   | Custom PHP 8.4.15 ZTS + OPcache build          | ✅ Built, ZTS confirmed, OPcache compiled                                |
| 2   | Build-Time Config Caching                      | ✅ `ensureConfigCached()` with atomic temp+rename                        |
| 3   | Stagger Worker Thread Starts                   | ✅ 2s per worker_id in `worker_pool.c`                                   |
| 4   | Gate Diagnostic Logging behind NATIVEPHP_DEBUG | ✅ `worker_diag()` in `common.php`                                       |
| 5   | Consolidate Bootstrap to `common.php`          | ✅ ~60 lines of duplication eliminated                                   |
| 6   | Worker Pool Sizing by Device Memory            | ✅ `PhpWorkerService.kt` reads device memory                             |
| 7   | OPcache Warmup After Engine Init               | ✅ In `php_engine.c` after init                                          |
| 8   | SQLite Connection Pool                         | ✅ `sqlite_pool.c` (behind CMake flag)                                   |
| 9   | Native Queue Peek                              | ✅ `native_queue.c` (behind CMake flag, wired to supervisor)             |
| 10  | Unified Error Reporting                        | ✅ `WorkerErrorReporter.php`                                             |
| 11  | Wire SQLite pool + native queue to supervisor  | ✅ Integrated in `supervisor.c`                                          |
| 12  | iOS Worker Support                             | ✅ `IosWorkerScheduler.php` + `ios_worker.php`                           |
| 13  | WorkManager for Android 14+                    | ✅ `PhpPeriodicWorker.kt` + config flag                                  |
| 14  | OPcache File Cache (hybrid SHM+disk)           | ✅ In `php_engine.c` + `WorkerServiceProvider`                           |
| 15  | PHP connection pool with native fallback       | ✅ `NativeDbPool.php`                                                    |
| 16  | Metrics/telemetry dashboard                    | ✅ `WorkerMetrics.php` + API + Blade component                           |
| 17  | Job priority scheduling                        | ✅ Priority field in request context, sorted insertion, config map       |
| 18  | RTLD_GLOBAL fix for OPcache                    | ✅ Three-layer approach (compat JNI_OnLoad + patchelf + engine fallback) |

### 14.2 Cleanup Tasks

#### High Priority

1. ~~**Delete `php_preloader.c`**~~ — ✅ **DONE** (2026-02-17). File deleted.

2. ~~**Update stale documentation references to "php_preloader"**~~ — ✅ **DONE** (2026-02-18). All four docs updated:
   - `AGENT-GUIDE-NATIVEPHP-WORKERS.md` — source table, invariant #5, OPcache section fixed
   - `MULTITHREADING-IMPLEMENTATION.md` — §9.5, §4.1 table, §4.2, §4.3, §15 table updated
   - `architecture-concurrent-runtime.md` — Files Changed/Created section updated
   - **Note (2026-02-18):** `php_engine.c` is now committed (1188 lines). Its inline comment at line ~779 still says "php_preloader.c" — updated in the source file to reference `compat/android_compat.cpp`.

3. ~~**Verify OPcache loads on device**~~ — ✅ **VERIFIED** (2026-02-17). Confirmed
   on emulator (API 36). The critical fix was applying `patchelf --add-needed
libphp.so` to opcache.so — **both** in the mobile-air repo AND in the
   EngineeringApp's `nativephp/android/` template copy. The `native:run` command
   copies jniLibs from the template at first build but does NOT re-copy them on
   subsequent builds, so the EngineeringApp's copy must also be patched (or the
   `nativephp/android/` directory deleted to force a fresh copy).

   Logcat confirmation:

   ```
   Compat: libphp.so loaded with RTLD_GLOBAL — execute_ex=0x..., zend_execute_ex=0x...
   PhpEngine: dlopen probe for opcache.so succeeded
   PhpEngine: OPcache warmup completed
   ```

   **Key finding:** On API 36, Layers 1 and 3 (RTLD_GLOBAL pre-load and fallback)
   are necessary but **not sufficient** — Bionic's dependency resolver for shared
   libraries does NOT use global scope even when `dlsym(RTLD_DEFAULT)` can find
   the symbols. **Layer 2 (patchelf DT_NEEDED) is the actual fix** that makes
   opcache.so load successfully on API 36+.

#### Medium Priority

4. **Upgrade PHP to latest 8.4.x patch** — The build script defaults to 8.4.5 but
   has been tested with 8.4.15. Track stable releases for security patches.

5. **Build script CI integration** — Add a Dockerfile and GitHub Actions workflow
   for the cross-compilation pipeline. Currently only tested in WSL.

6. **Enable SQLite modules by default** — `sqlite_pool.c` and `native_queue.c` are
   behind a CMake flag. Consider enabling by default once stable.

7. **Profile OPcache cold boot improvement** — With OPcache file cache enabled,
   measure the actual cold-boot time improvement vs. the first-boot penalty of
   populating the file cache.

#### Low Priority

8. **Connection pool PHP ↔ native integration** — `NativeDbPool.php` detects
   native C pool availability but the plumbing between PHP's PDO and the C pool
   is not yet complete. Currently falls back to PHP-side PDO connections.

9. **Reduce APK size** — libphp.so is ~40MB unstripped. `strip` reduces it
   significantly but may break OPcache symbol resolution. Test carefully.

10. **Multi-architecture support** — Currently arm64 only. x86_64 support would
    enable the Android emulator on Intel/AMD host machines.

---

## 15. File Reference

### C/C++ (Native Layer)

| File                           | Lines      | Description                                                                                                                                                           |
| ------------------------------ | ---------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `php_engine.c` / `.h`          | 1188 / 89  | Engine singleton. RTLD_GLOBAL probe, OPcache dlopen, `php_embed_init()`, warmup pass. Declares `fix_opcache_tls_cache()`, `fix_opcache_per_thread_state()`.           |
| `php_thread_context.c` / `.h`  | 129        | TSRM attach/detach per worker thread. Defines `TSRMLS_CACHE_DEFINE()` for its own TU to avoid emutls conflicts.                                                       |
| `php_request_context.c` / `.h` | 1038 / 182 | Per-job execution context. Per-thread output buffers, cooperative cancellation, HTTP `$_SERVER` injection, status/type enums, priority field.                         |
| `worker_pool.c` / `.h`         | 740        | Pthread pool, priority queue, circuit breaker, stagger, UI-lane yield                                                                                                 |
| `supervisor.c` / `.h`          | 733        | Orchestrator, SQLite pool lifecycle, native queue peek, JNI API. Default memory_limit: `256M`.                                                                        |
| `scheduler_gate.c` / `.h`      | 67         | Mutex gate for schedule:run exclusivity                                                                                                                               |
| `bridge_jni.cpp`               | 196        | JNI bridge for BridgeRouterKt can/call API. Replaces removed `php_bridge.c`.                                                                                          |
| `libphp_wrapper.cpp`           | 121        | dlopen wrapper: re-opens libphp.so + libcompat.so + libphp_wrapper.so with RTLD_GLOBAL via C++ constructor.                                                           |
| `zts_guard.h`                  | 46         | Compile-time/runtime ZTS verification                                                                                                                                 |
| `sqlite_pool.c` / `.h`         | ~280       | SQLite connection pool (optional, integrated with supervisor)                                                                                                         |
| `native_queue.c` / `.h`        | ~310       | Native queue peek (optional, integrated with supervisor)                                                                                                              |
| `compat/android_compat.cpp`    | ~110       | Bionic compatibility shim (`getdtablesize()`, `copy_file_range()`) + **RTLD_GLOBAL preloader** (JNI_OnLoad). No glob polyfill — those are patched via `php_config.h`. |
| `CMakeLists.txt`               | ~225       | Build config, ZTS check, glob patch, optional SQLite modules                                                                                                          |

All C files live in: `resources/androidstudio/app/src/main/cpp/`

### Kotlin (Android Layer)

| File                     | Description                                                        |
| ------------------------ | ------------------------------------------------------------------ |
| `PHPBridge.kt`           | JNI bridge, library load order (compat→php→php_wrapper)            |
| `PhpWorkerService.kt`    | Foreground service, wake lock, supervisor lifecycle, polling loops |
| `PhpPeriodicWorker.kt`   | WorkManager alternative for Android 14+                            |
| `PhpSupervisorBridge.kt` | JNI bridge declarations (includes priority param)                  |
| `LaravelEnvironment.kt`  | Env setup, setEnvironmentVariable fix                              |

### Swift (iOS Layer)

| File                                 | Description                   |
| ------------------------------------ | ----------------------------- |
| `Worker/PhpEngine.swift`             | Swift bridge to C supervisor  |
| `Worker/PhpSupervisor.swift`         | In-process supervisor for iOS |
| `Worker/BackgroundTaskManager.swift` | BGProcessingTask registration |

### PHP (Framework Layer)

| File                                   | Description                                                               |
| -------------------------------------- | ------------------------------------------------------------------------- |
| `src/Worker/WorkerServiceProvider.php` | Auto-config: WAL, table creation, flock, config cache, OPcache file cache |
| `src/Worker/WorkerConfig.php`          | Configuration accessor (priority map, execution strategy)                 |
| `src/Worker/WorkerErrorReporter.php`   | Unified error reporting to `worker_errors` table                          |
| `src/Worker/WorkerMetrics.php`         | Metrics: snapshot, queueStatus, dashboard, throughput                     |
| `src/Worker/NativeDbPool.php`          | PHP connection pool with native C detection + PDO fallback                |
| `src/Worker/IosWorkerScheduler.php`    | iOS BGProcessingTask time-constrained execution (~25s window)             |
| `bootstrap/worker/common.php`          | Shared bootstrap: autoloader, paths, fork() prevention, superglobals      |
| `bootstrap/worker/queue_worker.php`    | Pop-one-job entrypoint                                                    |
| `bootstrap/worker/scheduler_tick.php`  | schedule:run entrypoint                                                   |
| `bootstrap/ios/ios_worker.php`         | iOS BGProcessingTask entrypoint                                           |
| `config/nativephp-worker.php`          | Default worker configuration (40+ keys)                                   |

### Build System

| File                                 | Description                                                       |
| ------------------------------------ | ----------------------------------------------------------------- |
| `scripts/build_php_android_arm64.sh` | Cross-compile PHP for Android arm64 with ZTS + OPcache + patchelf |
| `scripts/verify_zts.sh`              | Verify ZTS symbols in built binary                                |
