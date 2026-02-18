# NativePHP Android Multithreading — Implementation Walkthrough & PR Roadmap

A step-by-step narrative of how the concurrent worker system was built, followed
by a safe breakdown into individually-mergeable pull requests. Each PR is
independently buildable, non-breaking, and reviewable in isolation.

**Last updated:** 2026-02-18

---

## Table of Contents

1. [How We Got Here — Implementation Narrative](#1-how-we-got-here--implementation-narrative)
2. [The Problem We Solved](#2-the-problem-we-solved)
3. [PR Roadmap](#3-pr-roadmap)
   - [PR 01 — Custom ZTS PHP Binary & Build Script](#pr-01--custom-zts-php-binary--build-script)
   - [PR 02 — RTLD_GLOBAL / OPcache Compat Layer](#pr-02--rtld_global--opcache-compat-layer)
   - [PR 03 — Core C Engine & Per-Thread Context](#pr-03--core-c-engine--per-thread-context)
   - [PR 04 — Worker Pool, Supervisor & Scheduler Gate](#pr-04--worker-pool-supervisor--scheduler-gate)
   - [PR 05 — Android Foreground Service & Manifest](#pr-05--android-foreground-service--manifest)
   - [PR 06 — PHP Bootstrap Entrypoints](#pr-06--php-bootstrap-entrypoints)
   - [PR 07 — PHP Framework Layer (WorkerServiceProvider)](#pr-07--php-framework-layer-workerserviceprovider)
   - [PR 08 — Metrics, Dashboard & Status API](#pr-08--metrics-dashboard--status-api)
   - [PR 09 — Developer Tools (Artisan Commands)](#pr-09--developer-tools-artisan-commands)
   - [PR 10 — Worker Events](#pr-10--worker-events)
   - [PR 11 — iOS Worker Support](#pr-11--ios-worker-support)
   - [PR 12 — Optional C Modules (SQLite Pool + Native Queue)](#pr-12--optional-c-modules-sqlite-pool--native-queue)
   - [PR 13 — WorkManager Integration (Android 14+)](#pr-13--workmanager-integration-android-14)
   - [PR 14 — Build Config Cleanup](#pr-14--build-config-cleanup)
   - [PR 15 — Documentation Pass](#pr-15--documentation-pass)
4. [Dependency Graph](#4-dependency-graph)
5. [Risk Register](#5-risk-register)
6. [Testing Checkpoints](#6-testing-checkpoints)

---

## 1. How We Got Here — Implementation Narrative

### Stage 1: Recognising the Constraint

The stock NativePHP Android runtime ran PHP via the Embed SAPI in a single
thread. Every HTTP request (WebView → JNI → PHP → response) used the same global
C buffers and the same NTS (non-thread-safe) PHP binary. Adding background workers
by calling them on a second thread would immediately corrupt PHP's global state.

Three constraints had to be solved simultaneously before any background execution
was possible:

1. **Thread safety in PHP itself** — Required a ZTS binary (`--enable-zts`).
2. **OPcache symbol visibility** — OPcache hooks `execute_ex` at load time by
   resolving it from the host process's symbol table. Android's Bionic linker
   uses `RTLD_LOCAL` for all `System.loadLibrary()` calls, so `execute_ex` wasn't
   in global scope and OPcache silently failed to load.
3. **Per-thread output isolation** — The global `g_collected_output` buffer in
   `php_bridge.c` is not thread-safe. Every worker thread needs its own stdout
   capture path.

### Stage 2: Building the PHP Binary

We cross-compiled PHP 8.4.15 for Android arm64 using NDK r27. Three post-build
fixes were critical:

- **Strip `-fvisibility=hidden` from the Makefile** — Without this, `execute_ex`
  and other symbols are stripped from the dynamic symbol table. OPcache can't hook
  them without explicit visibility.
- **Patch `glob()`/`globfree()` prototypes in `php_config.h`** — PHP's
  `./configure` generates bare C prototypes. When included from C++ translation
  units (our bridge files), the mismatched linkage causes a compiler error. The
  fix wraps them in `extern "C" {}`.
- **Add `DT_NEEDED: libphp.so` to `opcache.so`** — opcache.so must be able to
  resolve its 426+ undefined symbols from libphp.so. Injecting a direct dependency
  edge via `patchelf` makes this work even on API 36 where RTLD_GLOBAL promotion
  via re-open is unreliable.

### Stage 3: The RTLD_GLOBAL Problem (Biggest Bug)

OPcache crashed on API 36+ with `cannot locate symbol "execute_ex"`. The root
cause: `System.loadLibrary("php")` uses `RTLD_LOCAL` (Bionic default). Even
calling `dlopen(libphp.so, RTLD_NOW | RTLD_GLOBAL)` afterward doesn't promote an
already-loaded RTLD_LOCAL library on API 36+ — Bionic silently ignores the flag
change.

**The fix:** A new library `libcompat.so` compiled from `compat/android_compat.cpp`.
Its `JNI_OnLoad` runs when `System.loadLibrary("compat")` is called — _before_
`System.loadLibrary("php")`. At that point `libphp.so` hasn't been loaded yet, so
the `dlopen(libphp.so, RTLD_NOW | RTLD_GLOBAL)` call in `JNI_OnLoad` loads it
fresh with RTLD_GLOBAL. When `System.loadLibrary("php")` runs next, Bionic finds
the already-loaded soinfo and reuses it — preserving the RTLD_GLOBAL flag.

Three-layer defense:

1. `libcompat.so` `JNI_OnLoad` — primary (works on all API levels)
2. patchelf `DT_NEEDED` edge — belt-and-suspenders (works on API 36+ dependency resolution)
3. `php_engine_init()` fallback `dlopen` — legacy (works on pre-API 36)

### Stage 4: The C Threading Runtime

With a ZTS binary and OPcache working, we built a clean C layer:

- **`php_engine.c`** — singleton engine manager. Calls `php_embed_init()` once
  per process; manages TSRM startup, OPcache probe, and an optional warmup pass.
- **`php_thread_context.c`** — per-thread TSRM attachment. Each worker pthread
  calls `ts_resource(0)` + `TSRMLS_CACHE_UPDATE()` before executing any PHP.
- **`php_request_context.c`** — per-job isolation. Each job gets its own stdout
  buffer (TLS-routed via `ub_write`), status tracking, timing, and an atomic
  cancellation flag wired to the Zend VM interrupt mechanism.
- **`worker_pool.c`** — N pthreads waiting on a condvar job queue. Includes a
  priority queue (high/default/low), staggered starts (2s per worker_id) to
  reduce bootstrap contention, and a circuit breaker for crash-loop protection.
- **`supervisor.c`** — top-level orchestrator. Owns engine, pool, and gate
  lifetimes; generates job IDs; writes structured JSON-line logs.
- **`scheduler_gate.c`** — a simple atomic CAS mutex ensuring `schedule:run`
  never runs concurrently.

### Stage 5: The Kotlin Service Layer

`PhpWorkerService.kt` is an Android Foreground Service (type `dataSync`). It:

- Acquires a `PARTIAL_WAKE_LOCK` capped at 6 hours
- Posts a persistent system notification (required for FGS)
- Calls `supervisor_engine_init()` via `PhpSupervisorBridge`
- Runs two `CoroutineScope` loops: queue poll every 5s, scheduler tick every 60s

A deferred start was added to `MainActivity.kt` — the worker service only starts
after `onFirstPageRendered()` fires (hooked in `PHPWebViewClient`). This prevents
the blank-screen problem where PHP worker boot contention delayed the first HTTP
response to the WebView.

`PHPBridge.kt` was changed from `newSingleThreadExecutor` to `newFixedThreadPool(2)`
to allow parallel HTTP handling alongside the worker service's JNI calls.

### Stage 6: The PHP Side

`bootstrap/worker/common.php` is shared between queue and scheduler. It:

- Detects and requires `vendor/autoload.php`
- Pre-sets `Terminal::$width`/`$height` via Reflection to prevent `fork()` from
  `proc_open('stty')` — which deadlocks in a multithreaded process (the hardest
  bug to diagnose; symptoms are a permanently hung scheduler thread)
- Assigns `$_ENV`/`$_SERVER` (never `putenv()`) for thread-safety
- Bootstraps Laravel's kernel

`WorkerServiceProvider` auto-discovered via Composer runs on every boot. It:

- Sets SQLite WAL mode with the correct PRAGMA order (`busy_timeout` first)
- Auto-creates queue tables (`IF NOT EXISTS`) under `flock()` to serialize
  multi-thread first-boot
- Builds a lightweight config cache via `flock()` + atomic temp-file rename
  (avoids `Artisan::call('config:cache')` which would cause infinite recursion)

### Stage 7: Optional Enhancements

Two C modules were added behind a CMake feature flag
(`NATIVEPHP_ENABLE_SQLITE_MODULES=OFF` by default):

- **`sqlite_pool.c`** — pre-opens N WAL SQLite connections; workers borrow and
  return via condvar.
- **`native_queue.c`** — queries the `jobs` table directly without PHP bootstrap.
  If the queue is empty, the entire PHP exec path is skipped.

WorkManager integration (`PhpPeriodicWorker.kt`) provides a battery-friendly
alternative to FGS on Android 14+ where `dataSync` FGS has a hard 6-hour limit.

---

## 2. The Problem We Solved

### Before

```
Android App Process
  └─ UI Thread (main)
       └─ WebView HTTP request
            └─ JNI → PHP NTS binary (single thread, global buffers)
                 └─ Response
No background execution. No queue processing. No scheduler.
```

### After

```
Android App Process
  ├─ UI Thread
  │    └─ WebView HTTP → PHPBridge (2-thread executor) → JNI → libphp_wrapper.so
  │                                                               └─ per-request context
  │
  └─ PhpWorkerService (FGS, WakeLock)
       └─ PhpSupervisorBridge → supervisor.c
            ├─ php_engine_init() [once]
            ├─ worker_pool (2 pthreads, condvar queue)
            │    ├─ Worker 0: queue_worker.php [one job/invocation]
            │    └─ Worker 1: queue_worker.php [one job/invocation]
            └─ scheduler_gate → scheduler_tick.php [every 60s, exclusive]

Shared: OPcache SHM (cross-thread read-mostly) • SQLite WAL
```

---

## 3. PR Roadmap

Each PR is described with:

- **Files changed/added** — exhaustive list
- **Testing** — what to verify before merging
- **Risk** — potential for regression
- **Rollback** — what breaks if reverted

---

### PR 01 — Custom ZTS PHP Binary & Build Script

**Goal:** Land the ZTS PHP build tooling and artifacts. No runtime behaviour
change — the new binary is identical in API to the NTS one from the user's
perspective. Workers are not yet wired up.

#### Files

| Action  | File                                                                               |
| ------- | ---------------------------------------------------------------------------------- |
| ADD     | `scripts/build_php_android_arm64.sh`                                               |
| ADD     | `scripts/verify_zts.sh`                                                            |
| REPLACE | `resources/androidstudio/app/src/main/jniLibs/arm64-v8a/libphp.so`                 |
| ADD     | `resources/androidstudio/app/src/main/jniLibs/arm64-v8a/opcache.so`                |
| ADD     | `resources/androidstudio/app/src/main/cpp/include/php/` (full header tree)         |
| ADD     | `resources/androidstudio/app/src/main/cpp/include/php_config.h` (patched)          |
| ADD     | `resources/androidstudio/app/src/main/cpp/include/php/main/php_config.h` (patched) |

#### Build script inputs

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk-r27
export PKG_CONFIG_PATH=/path/to/php-deps/lib/pkgconfig
export DEPS_PREFIX=/path/to/php-deps
export EXTRA_CONFIGURE_FLAGS="--enable-bcmath ..."
PHP_VERSION=php-8.4.15 INSTALL_TO_PROJECT=1 EMIT_JNILIBS_ZIP=0 \
  bash scripts/build_php_android_arm64.sh
```

#### Testing

- `adb logcat -s "AndroidRuntime"` — no `UnsatisfiedLinkError` on startup
- Request timing header `opcache=NOT_AVAILABLE` is expected at this stage (OPcache fix is PR 02)
- `nm -D libphp.so | grep execute_ex` must show `T execute_ex` (not `t`)
- `nm -D libphp.so | grep ts_resource` must show `T ts_resource`

#### Risk: LOW

The new binary is API-compatible. If this PR causes a runtime regression, revert
by restoring the previous `libphp.so`.

---

### PR 02 — RTLD_GLOBAL / OPcache Compat Layer

**Goal:** Make OPcache load successfully on all Android API levels, including
API 36+ where post-load RTLD_GLOBAL promotion does not work.

#### Files

| Action | File                                                                                                                 |
| ------ | -------------------------------------------------------------------------------------------------------------------- |
| ADD    | `resources/androidstudio/app/src/main/cpp/compat/android_compat.cpp`                                                 |
| ADD    | `resources/androidstudio/app/src/main/cpp/zts_guard.h`                                                               |
| MODIFY | `resources/androidstudio/app/src/main/cpp/CMakeLists.txt` (add `libcompat.so` target; ZTS compile check; glob patch) |
| MODIFY | `resources/androidstudio/app/src/main/java/.../bridge/PHPBridge.kt` (load order: compat→php→php_wrapper)             |
| DELETE | `resources/androidstudio/app/src/main/cpp/compat/php_preloader.c` (if still present from earlier attempt)            |

#### What this PR does

1. Builds `libcompat.so` from `compat/android_compat.cpp`. Its `JNI_OnLoad`
   loads `libphp.so` with `RTLD_GLOBAL` before `System.loadLibrary("php")` runs.
2. Adds a CMake compile-time ZTS check — `FATAL_ERROR` if `/* #undef ZTS */`
   is present in `php_config.h`.
3. Adds `extern "C"` guards around `glob()`/`globfree()` in both
   `php_config.h` copies (auto-patched at CMake configure time).
4. Updates `PHPBridge.kt` to load `compat` first.

#### Testing

```
adb logcat -d -s "Compat" | grep "RTLD_GLOBAL"
# Should show: Compat: libphp.so loaded with RTLD_GLOBAL — execute_ex=0x...
```

- Request timing header should now show `opcache=AVAILABLE`
- `adb logcat -s "PhpEngine"` should show `OPcache warmup completed`

#### Risk: MEDIUM

Changes `System.loadLibrary` order in `PHPBridge.kt`. If `libcompat.so` fails to
build or load, the app crashes at startup with `UnsatisfiedLinkError`.

**Rollback:** Revert `PHPBridge.kt` load order. OPcache reverts to
`NOT_AVAILABLE` but the app remains functional.

---

### PR 03 — Core C Engine & Per-Thread Context

**Goal:** Land the PHP engine singleton, per-thread TSRM management, and per-job
request isolation. No Kotlin wiring yet — this PR just compiles the C code into
`libphp_wrapper.so`.

#### Files

| Action | File                                                                                              |
| ------ | ------------------------------------------------------------------------------------------------- |
| ADD    | `resources/.../cpp/php_engine.h` (89 lines)                                                       |
| ADD    | `resources/.../cpp/php_engine.c` (1188 lines)                                                     |
| ADD    | `resources/.../cpp/php_thread_context.h`                                                          |
| ADD    | `resources/.../cpp/php_thread_context.c` (129 lines)                                              |
| ADD    | `resources/.../cpp/php_request_context.h` (182 lines)                                             |
| ADD    | `resources/.../cpp/php_request_context.c` (1038 lines)                                            |
| MODIFY | `resources/.../cpp/CMakeLists.txt` (add new sources to `libphp_wrapper.so`)                       |
| MODIFY | `resources/.../cpp/php_bridge.c` (add supervisor forward declarations, per-thread output routing) |

#### What this PR does NOT do

- Does not start any worker threads
- Does not expose JNI methods to Kotlin
- Does not change PHP execution for HTTP requests

#### Testing

- Android Studio builds without C compiler errors
- App starts and serves HTTP requests identically to before
- No `libphp_wrapper.so` linker errors in build log

#### Risk: LOW

Pure compile-only addition. No runtime paths are changed for HTTP.

---

### PR 04 — Worker Pool, Supervisor & Scheduler Gate

**Goal:** Land the full native threading runtime and expose it via JNI. Workers
are not yet started from Kotlin — this PR makes them startable.

#### Files

| Action | File                                                                                |
| ------ | ----------------------------------------------------------------------------------- |
| ADD    | `resources/.../cpp/worker_pool.h`                                                   |
| ADD    | `resources/.../cpp/worker_pool.c` (740 lines)                                       |
| ADD    | `resources/.../cpp/supervisor.h`                                                    |
| ADD    | `resources/.../cpp/supervisor.c` (733 lines)                                        |
| ADD    | `resources/.../cpp/scheduler_gate.h`                                                |
| ADD    | `resources/.../cpp/scheduler_gate.c` (67 lines)                                     |
| ADD    | `resources/.../cpp/bridge_jni.cpp` (196 lines, replaces `php_bridge.c` JNI surface) |
| ADD    | `resources/.../cpp/libphp_wrapper.cpp` (121 lines)                                  |
| ADD    | `resources/.../worker/PhpSupervisorBridge.kt`                                       |
| MODIFY | `resources/.../cpp/CMakeLists.txt`                                                  |

#### PhpSupervisorBridge JNI surface

```kotlin
// Exposed by this PR, called by PhpWorkerService in PR 05
external fun nativeEngineInit(storagePath: String, iniPath: String): Boolean
external fun nativeSupervisorStart(mode: Int, workerCount: Int, queues: String, connection: String): Boolean
external fun nativeSupervisorStop(): Boolean
external fun nativeEnqueueQueueJob(payload: String?): String?
external fun nativeEnqueueSchedulerTick(payload: String?): String?
external fun nativeAwaitJob(jobId: String, timeoutMs: Long): String?
external fun nativeWakeWorkers(): Boolean
external fun nativeSupervisorStatusJson(): String
external fun nativeSetUiRequestActive(active: Boolean)
```

#### Testing

- App starts and serves HTTP requests identically to before
- `PhpSupervisorBridge` can be instantiated without crash (even though no calls
  are made from app code yet)
- Logcat shows no linker errors

#### Risk: LOW

New code added; nothing calls it yet from the app path.

---

### PR 05 — Android Foreground Service & Manifest

**Goal:** Wire up the worker service to actually start. This is the first PR
where workers run PHP code. Depends on PRs 01–04 and 06.

**Note:** PHP bootstrap entrypoints (PR 06) must be merged before or together with
this PR, since `PhpWorkerService` calls `queue_worker.php` and `scheduler_tick.php`.

#### Files

| Action | File                                                                                                  |
| ------ | ----------------------------------------------------------------------------------------------------- |
| ADD    | `resources/.../worker/PhpWorkerService.kt`                                                            |
| ADD    | `resources/.../worker/WorkerBootReceiver.kt`                                                          |
| MODIFY | `resources/.../AndroidManifest.xml` (5 permissions + FGS + receiver)                                  |
| MODIFY | `resources/.../bridge/PHPBridge.kt` (single→2-thread executor, `nativeSetUiRequestActive`)            |
| MODIFY | `resources/.../bridge/LaravelEnvironment.kt` (deferred worker start, `setEnvironmentVariable` fix)    |
| MODIFY | `resources/.../ui/MainActivity.kt` (`onFirstPageRendered`, `autoStartWorkerService`)                  |
| MODIFY | `resources/.../network/PHPWebViewClient.kt` (HTTP response `\n\n` separator fix, first-page callback) |
| MODIFY | `resources/.../network/WebViewManager.kt` (`onFirstPageRendered` hook)                                |

#### Manifest additions

```xml
<uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
<uses-permission android:name="android.permission.FOREGROUND_SERVICE_DATA_SYNC" />
<uses-permission android:name="android.permission.WAKE_LOCK" />
<uses-permission android:name="android.permission.RECEIVE_BOOT_COMPLETED" />
<uses-permission android:name="android.permission.POST_NOTIFICATIONS" />

<service android:name=".worker.PhpWorkerService"
    android:foregroundServiceType="dataSync"
    android:exported="false" />

<receiver android:name=".worker.WorkerBootReceiver"
    android:exported="true">
    <intent-filter>
        <action android:name="android.intent.action.BOOT_COMPLETED" />
    </intent-filter>
</receiver>
```

#### Worker auto-start flow

```
Application start
  → LaravelEnvironment.initialize()
  → [UI renders first page]
  → PHPWebViewClient.onPageFinished() → onFirstPageRendered()
  → MainActivity.autoStartWorkerService()       ← new
  → PhpWorkerService starts (FGS)
  → supervisor_engine_init() + supervisor_start()
  → polling loops begin
```

#### Testing

- First WebView page loads without blank screen
- `adb logcat -d -s "Supervisor"` shows worker start after first page paint
- `adb logcat -d -s "PhpWorkerService"` shows `Worker service started`
- `GET /_native/api/worker/status` returns `{"status":"running",...}`
- Dispatch a test job → appears processed in queue status within 10s

#### Risk: HIGH

First PR to start workers. Key regression vectors:

- Blank screen on app start (fixed by deferred start, but timing is critical)
- App crash on `UnsatisfiedLinkError` if library order wrong
- `SQLITE_BUSY` errors if WAL not configured (PR 07 dependency)
- FGS permission crash on Android 14 (all 5 permissions must be declared)

**Rollback:** Set `NATIVEPHP_WORKER_AUTO_START=false` in app `.env`. Workers stop
without any app-visible regression.

---

### PR 06 — PHP Bootstrap Entrypoints

**Goal:** Land the PHP-side worker entrypoints. Can be merged without PR 05 —
the files exist in the bundle but are never called until the service starts.

#### Files

| Action | File                                                                                                                        |
| ------ | --------------------------------------------------------------------------------------------------------------------------- |
| ADD    | `bootstrap/worker/common.php`                                                                                               |
| ADD    | `bootstrap/worker/queue_worker.php`                                                                                         |
| ADD    | `bootstrap/worker/scheduler_tick.php`                                                                                       |
| MODIFY | `bootstrap/android/native.php` (logging, `max_execution_time`, `socket_default_timeout`, OPcache timing in response header) |

#### Critical patterns in `common.php`

```php
// 1. fork() deadlock prevention — MUST happen before kernel bootstrap
$termClass = new ReflectionClass(\Symfony\Component\Console\Terminal::class);
$widthProp  = $termClass->getProperty('width');  $widthProp->setAccessible(true);
$heightProp = $termClass->getProperty('height'); $heightProp->setAccessible(true);
$widthProp->setValue(null, 80);
$heightProp->setValue(null, 24);

// 2. Thread-safe env assignment (NEVER putenv())
$_ENV['NATIVEPHP_JOB_TYPE'] = $jobType;   // 'queue' or 'scheduler'
$_SERVER['LARAVEL_STORAGE_PATH'] = $storagePath;

// 3. Memory limit
ini_set('memory_limit', getenv('NATIVEPHP_WORKER_MEMORY_LIMIT') ?: '512M');
```

#### Testing

- Deploy to device with PR 05 active
- Dispatch a simple `SleepJob` → verify it runs and logs `{processed: true, ...}`
- Dispatch a failing job → verify `WorkerErrorReporter` table row created
- `adb shell run-as <appId> cat app_storage/laravel/storage/logs/worker.jsonl`

#### Risk: MEDIUM

A bug in `common.php` (e.g., wrong path resolution) will silently fail all worker
jobs. The JSON output (`{processed: false, error: "..."}`) is the primary signal.

---

### PR 07 — PHP Framework Layer (WorkerServiceProvider)

**Goal:** Auto-configure Laravel for workers — zero developer setup required.
Handles WAL mode, table creation, config cache, and queue driver promotion.

#### Files

| Action | File                                                                           |
| ------ | ------------------------------------------------------------------------------ |
| ADD    | `src/Worker/WorkerServiceProvider.php`                                         |
| ADD    | `src/Worker/WorkerConfig.php`                                                  |
| ADD    | `src/Worker/WorkerErrorReporter.php`                                           |
| ADD    | `config/nativephp-worker.php`                                                  |
| MODIFY | `composer.json` (add `WorkerServiceProvider` to `extra.laravel.providers`)     |
| MODIFY | `src/NativeServiceProvider.php` (register `WorkerCommand`)                     |
| MODIFY | `src/Facades/System.php` (add `isWorkerContext()` and 5 worker facade methods) |
| MODIFY | `src/System.php` (implement `isWorkerContext()`)                               |

#### What `WorkerServiceProvider` does on every boot

```php
// 1. WAL mode — PRAGMA order matters: busy_timeout FIRST
DB::listen(fn($q) => null); // triggers ConnectionEstablished listener
PRAGMA busy_timeout = 5000;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA wal_autocheckpoint = 100;
PRAGMA cache_size = -8000;

// 2. First-boot tasks under flock() — safe for N concurrent threads
flock($lock, LOCK_EX):
  CREATE TABLE IF NOT EXISTS jobs ...
  CREATE TABLE IF NOT EXISTS failed_jobs ...
  CREATE TABLE IF NOT EXISTS job_batches ...
  CREATE TABLE IF NOT EXISTS worker_errors ...
  ensureConfigCached()   // atomic temp+rename, no Artisan recursion
```

#### Testing

- Fresh install: `PRAGMA journal_mode` returns `wal`
- `jobs`, `failed_jobs`, `worker_errors` tables exist after first worker boot
- Config cache file exists at `bootstrap/cache/config.php` after first worker boot
- Dispatch `MyJob::dispatch()` from HTTP request → job appears in `jobs` table
  → worker processes it within poll interval → job deleted

#### Risk: MEDIUM

Changing queue config before worker launch. If `WorkerServiceProvider` throws
during `boot()`, the entire request fails. The `flock()` serialisation ensures
table creation is safe, but file permission issues in `storage/` can cause hangs.

---

### PR 08 — Metrics, Dashboard & Status API

**Goal:** Give developers visibility into worker health via an API endpoint and a
Blade dashboard component.

#### Files

| Action | File                                                         |
| ------ | ------------------------------------------------------------ |
| ADD    | `src/Worker/WorkerMetrics.php`                               |
| ADD    | `src/Worker/NativeDbPool.php`                                |
| ADD    | `src/Http/Controllers/WorkerStatusController.php`            |
| MODIFY | `routes/api.php` (register `GET /_native/api/worker/status`) |
| ADD    | `resources/views/components/worker-dashboard.blade.php`      |

#### API response shape

```json
{
  "status": "running",
  "activeJobs": 1,
  "pendingJobs": 3,
  "completedJobs": 142,
  "failedJobs": 2,
  "schedulerRunning": false,
  "uptimeSeconds": 3600,
  "mode": 0,
  "config": { "workerCount": 2, "queues": "high,default,low", ... },
  "queueStats": { "default": { "pending": 3, "reserved": 1 } }
}
```

#### Dashboard usage

```blade
{{-- In any Blade view --}}
<x-nativephp-worker-dashboard />
{{-- Auto-polls every 5s via Alpine.js --}}
```

#### Testing

- `curl http://127.0.0.1/_native/api/worker/status` from device WebView
- Dashboard visible and updating in app
- `WorkerMetrics::throughput(5)` returns sensible value after sustained load

#### Risk: LOW

Read-only additions. No behaviour change to worker execution path.

---

### PR 09 — Developer Tools (Artisan Commands)

**Goal:** Local development experience — simulate workers on dev machine and tail
structured logs from device.

#### Files

| Action | File                                                         |
| ------ | ------------------------------------------------------------ |
| ADD    | `src/Commands/WorkerCommand.php`                             |
| MODIFY | `src/Commands/TailCommand.php` (add `--type=worker` support) |
| MODIFY | `src/NativeServiceProvider.php` (register both commands)     |

#### Usage

```bash
# Simulate workers locally (no Android device needed)
php artisan native:worker
php artisan native:worker --queue-only --workers=1
php artisan native:worker --status

# Tail structured JSON log from device
php artisan native:tail --type=worker
```

#### Testing

- `php artisan native:worker --status` shows running/stopped state
- Dispatching a job while `native:worker` is running processes it within 5s
- `native:tail --type=worker` streams new log lines as jobs complete

#### Risk: LOW

---

### PR 10 — Worker Events

**Goal:** Let app developers listen for worker lifecycle events using standard
Laravel event listeners.

#### Files

| Action | File                                                                          |
| ------ | ----------------------------------------------------------------------------- |
| ADD    | `src/Events/Worker/WorkerStarted.php`                                         |
| ADD    | `src/Events/Worker/WorkerStopped.php`                                         |
| ADD    | `src/Events/Worker/JobCompleted.php`                                          |
| ADD    | `src/Events/Worker/JobFailed.php`                                             |
| ADD    | `src/Events/Worker/CircuitBreakerTripped.php`                                 |
| ADD    | `src/Events/Worker/SchedulerTickCompleted.php`                                |
| MODIFY | `bootstrap/worker/queue_worker.php` (fire `JobCompleted`/`JobFailed`)         |
| MODIFY | `bootstrap/worker/scheduler_tick.php` (fire `SchedulerTickCompleted`)         |
| MODIFY | `src/Worker/WorkerServiceProvider.php` (fire `WorkerStarted`/`WorkerStopped`) |

#### Usage in app

```php
Event::listen(JobFailed::class, function (JobFailed $event) {
    Log::error("Job failed: {$event->jobName}", ['error' => $event->error]);
    Notification::send($admin, new WorkerAlert($event));
});
```

#### Risk: LOW

Additive only. Events add overhead per-job; keep listeners fast.

---

### PR 11 — iOS Worker Support

**Goal:** Bring background job processing to iOS via BGProcessingTask.

#### Files

| Action | File                                                                               |
| ------ | ---------------------------------------------------------------------------------- |
| ADD    | `bootstrap/ios/ios_worker.php`                                                     |
| ADD    | `src/Worker/IosWorkerScheduler.php`                                                |
| ADD    | `resources/xcode/NativePHP/Worker/PhpEngine.swift`                                 |
| ADD    | `resources/xcode/NativePHP/Worker/PhpSupervisor.swift`                             |
| ADD    | `resources/xcode/NativePHP/Worker/BackgroundTaskManager.swift`                     |
| MODIFY | `resources/xcode/NativePHP/Info.plist` (add `BGTaskSchedulerPermittedIdentifiers`) |
| MODIFY | `bootstrap/ios/native.php` (hook worker entrypoint)                                |

#### iOS constraints

- BGProcessingTask windows are ~25 seconds — `IosWorkerScheduler` drains the
  queue within that window and exits early if time is running out
- Scheduler only runs during BGProcessingTask windows — not every minute
- No FGS equivalent — no persistent worker; each BGProcessingTask is a one-shot run
- `ios_worker.php` always reschedules the next BGProcessingTask on exit

#### Testing

- Enable Background Fetch in Xcode capabilities
- Force a BGProcessingTask invocation: `e -l objc -- (void)[[BGTaskScheduler sharedScheduler] _simulateLaunchForTaskWithIdentifier:@"com.nativephp.worker"]`
- Verify jobs drain from queue within 25s window

#### Risk: MEDIUM (iOS-only, Android unaffected)

---

### PR 12 — Optional C Modules (SQLite Pool + Native Queue)

**Goal:** Add two optional performance modules behind a CMake feature flag that
defaults to OFF. Zero impact on builds that don't opt in.

#### Files

| Action | File                                                                        |
| ------ | --------------------------------------------------------------------------- |
| ADD    | `resources/.../cpp/sqlite_pool.h`                                           |
| ADD    | `resources/.../cpp/sqlite_pool.c` (~280 lines)                              |
| ADD    | `resources/.../cpp/native_queue.h`                                          |
| ADD    | `resources/.../cpp/native_queue.c` (~310 lines)                             |
| MODIFY | `resources/.../cpp/CMakeLists.txt` (`NATIVEPHP_ENABLE_SQLITE_MODULES` flag) |
| MODIFY | `resources/.../cpp/supervisor.c` (integrate pool + queue peek)              |

#### Enabling

```cmake
# In CMakeLists.txt or via Gradle:
-DNATIVEPHP_ENABLE_SQLITE_MODULES=ON
```

#### What they do

- **`sqlite_pool.c`** — pre-opens N WAL-mode SQLite connections. Workers borrow
  (`sqlite_pool_acquire`) / return (`sqlite_pool_release`) via condvar. Eliminates
  per-job connection open/close overhead.
- **`native_queue.c`** — `SELECT COUNT(*) FROM jobs WHERE reserved_at IS NULL`.
  If zero, `supervisor.c`'s queue poll skips PHP bootstrap entirely. On an idle
  app, this reduces CPU wake cost from ~300ms (PHP bootstrap) to ~1ms (SQL query).

#### Testing

- Build with flag ON; verify `supervisor.c` linkage
- Run 100 jobs; confirm throughput improves
- Run with empty queue for 5 minutes; confirm no idle PHP spawns in logcat

#### Risk: VERY LOW (opt-in only)

---

### PR 13 — WorkManager Integration (Android 14+)

**Goal:** Alternative execution strategy for Android 14+ where `dataSync` FGS
has a hard 6-hour per-24h limit.

#### Files

| Action | File                                                                                             |
| ------ | ------------------------------------------------------------------------------------------------ |
| ADD    | `resources/.../worker/PhpPeriodicWorker.kt`                                                      |
| MODIFY | `resources/androidstudio/app/build.gradle.kts` (add `androidx.work:work-runtime-ktx:2.10.0`)     |
| MODIFY | `src/Worker/WorkerConfig.php` (add `androidExecutionStrategy()`)                                 |
| MODIFY | `config/nativephp-worker.php` (add `android_execution_strategy`, `workmanager_interval_minutes`) |

#### Strategy selection

| `NATIVEPHP_ANDROID_EXECUTION_STRATEGY` | Behaviour                                                              |
| -------------------------------------- | ---------------------------------------------------------------------- |
| `auto` (default)                       | FGS in foreground; falls back to WorkManager when FGS limit approached |
| `foreground`                           | Always use FGS                                                         |
| `workmanager`                          | Always use WorkManager (minimum 15-minute interval)                    |

#### Important: build.gradle.kts minify

This PR adds the WorkManager dependency. **Do NOT hardcode** `isMinifyEnabled =
false` — keep the `REPLACE_MINIFY_ENABLED` placeholder so consumer apps control
ProGuard in their own release builds.

#### Testing

- Set `NATIVEPHP_ANDROID_EXECUTION_STRATEGY=workmanager`
- Verify WorkManager job scheduled with `adb shell dumpsys jobscheduler | grep nativephp`
- Kill FGS manually; confirm WorkManager picks up within 15 minutes

#### Risk: LOW (alternative path only, FGS unchanged)

---

### PR 14 — Build Config Cleanup

**Goal:** Remove debug artifacts and fix the hardcoded `isMinifyEnabled = false`
that would affect all consumer apps' release APKs.

#### Files

| Action | File                                                                                                                        |
| ------ | --------------------------------------------------------------------------------------------------------------------------- |
| DELETE | `opcache.so` (root — debug copy, duplicates jniLibs)                                                                        |
| DELETE | `disasm.txt`, `disasm2.txt`, `dynsyms.txt`, `relocs.txt` (symbol dump artifacts)                                            |
| DELETE | `scripts/android_compat.c` (stale C draft, superseded by `compat/android_compat.cpp`)                                       |
| MODIFY | `resources/androidstudio/app/build.gradle.kts` (restore `REPLACE_MINIFY_ENABLED` + `REPLACE_SHRINK_RESOURCES` placeholders) |
| MODIFY | `.gitignore` (add `*.so` root-level, `disasm*.txt`, `dynsyms.txt`, `relocs.txt`)                                            |

#### Why the minify fix matters

`isMinifyEnabled = false` (hardcoded) disables ProGuard for **all consumer app
release builds** that use the template — roughly 20–40% APK size increase and
no code shrinking. The correct value is the template placeholder
`REPLACE_MINIFY_ENABLED` which NativePHP's `native:build` replaces per-app.

#### .gitignore additions

```
# Native build debug artifacts
/opcache.so
/disasm*.txt
/dynsyms.txt
/relocs.txt
```

#### Risk: LOW (cleanup only)

---

### PR 15 — Documentation Pass

**Goal:** Ensure all four documentation files accurately reflect the implemented
state. Update "not yet committed" notes now that `php_engine.c` and
`php_request_context.c` are committed.

#### Files

| Action | File                                            |
| ------ | ----------------------------------------------- |
| MODIFY | `docs/MULTITHREADING-IMPLEMENTATION.md`         |
| MODIFY | `docs/AGENT-GUIDE-NATIVEPHP-WORKERS.md`         |
| MODIFY | `docs/architecture-concurrent-runtime.md`       |
| MODIFY | `docs/laravel-integration.md`                   |
| ADD    | `docs/MULTITHREADING-PR-ROADMAP.md` (this file) |

#### Key fixes

- Remove all "not yet committed" notes for `php_engine.c` / `php_request_context.c`
- Update §14.2 completion notes
- Fix stale `php_preloader` reference in `php_engine.c` inline comment
- Verify §3.4 artifact sizes match actual files (libphp.so ~40MB, opcache.so ~1.8MB)

#### Risk: ZERO

---

## 4. Dependency Graph

```
PR 01 (ZTS binary)
  └─► PR 02 (RTLD_GLOBAL compat)
        └─► PR 03 (C engine + per-thread/per-job context)
              └─► PR 04 (worker pool + supervisor)
                    └─► PR 05 (Android FGS + manifest)   ←─ also needs PR 06
                    └─► PR 06 (PHP bootstrap)
                          └─► PR 07 (WorkerServiceProvider)
                                └─► PR 08 (metrics + dashboard)
                                └─► PR 09 (dev commands)
                                └─► PR 10 (events)

PR 04 ──► PR 12 (optional C modules, standalone)
PR 04 ──► PR 13 (WorkManager, standalone after PR 05)
PR 07 ──► PR 11 (iOS workers, uses WorkerServiceProvider)

PR 14 (cleanup) — no dependencies, can merge any time
PR 15 (docs)    — no dependencies, can merge any time
```

Minimum viable path for working Android workers:
**PR 01 → PR 02 → PR 03 → PR 04 → PR 06 → PR 05 → PR 07**

---

## 5. Risk Register

| PR  | Risk                                         | Likelihood | Mitigation                                                                  |
| --- | -------------------------------------------- | ---------- | --------------------------------------------------------------------------- |
| 01  | New libphp.so breaks HTTP                    | Low        | ABI-compatible; revert binary only                                          |
| 02  | `UnsatisfiedLinkError` on `libcompat.so`     | Medium     | Verify CMake includes `compat/` dir; check build output                     |
| 03  | C compiler errors (ZTS macro not defined)    | Low        | `zts_guard.h` provides early `#error`; check `php_config.h` include paths   |
| 04  | JNI signature mismatch                       | Low        | Match Kotlin `external fun` signatures to C function names exactly          |
| 05  | Blank screen on app start                    | Medium     | If seen, increase post-render delay in `autoStartWorkerService()`           |
| 05  | FGS crashes on Android 14                    | Medium     | All 5 permissions required; verify `foregroundServiceType="dataSync"`       |
| 06  | `fork()` hang in scheduler thread            | Low        | Verify `Terminal::$width` Reflection pre-set; check for `proc_open()` calls |
| 07  | Infinite recursion in `ensureConfigCached()` | Low        | Never call `Artisan::call('config:cache')`; use atomic temp+rename only     |
| 07  | `SQLITE_BUSY`                                | Medium     | `busy_timeout=5000` before `journal_mode=WAL`; verify PRAGMA order          |
| 13  | Hardcoded `isMinifyEnabled=false`            | High       | See PR 14; must be fixed before release                                     |

---

## 6. Testing Checkpoints

### After PR 02 (OPcache)

```
adb logcat -d -s "Compat" | grep RTLD_GLOBAL
# → Compat: libphp.so loaded with RTLD_GLOBAL — execute_ex=0x..., zend_execute_ex=0x...

adb logcat -d -s "PhpEngine" | grep -E "OPcache|execute_ex"
# → PhpEngine: execute_ex visible at 0x...
# → PhpEngine: OPcache warmup completed
```

### After PR 05 + 06 (First worker run)

```
# 1. Dispatch a test job from Laravel tinker
php artisan tinker --env=local
>>> App\Jobs\TestJob::dispatch()

# 2. Check worker processed it
adb shell run-as com.example.app cat \
  app_storage/laravel/storage/logs/worker.jsonl | tail -5
# → {"event":"job_completed","job_name":"TestJob","duration_ms":42,...}

# 3. Confirm queue is empty
curl http://127.0.0.1/_native/api/worker/status | jq .pendingJobs
# → 0
```

### After PR 07 (WAL + auto tables)

```
adb shell run-as com.example.app \
  sqlite3 app_storage/persisted_data/database/database.sqlite \
  "PRAGMA journal_mode; SELECT name FROM sqlite_master WHERE type='table';"
# → wal
# → jobs | failed_jobs | job_batches | worker_errors | ...
```

### Full integration smoke test

```
# 1. Uninstall app (clean state)
adb uninstall com.example.app

# 2. Install and launch
adb install app-debug.apk && \
  adb shell am start -n com.example.app/.ui.MainActivity

# 3. Wait 30s; check no crash
sleep 30
adb shell pidof com.example.app   # must return a PID

# 4. Dispatch 10 jobs, wait 60s
for i in $(seq 1 10); do
  curl -X POST http://127.0.0.1/_native/api/dispatch-test
done
sleep 60

# 5. Confirm all processed
curl http://127.0.0.1/_native/api/worker/status | jq '.completedJobs, .failedJobs'
# → 10
# → 0

# 6. Cold boot (kills process completely, waits for restart)
adb shell am force-stop com.example.app
sleep 5
adb shell am start -n com.example.app/.ui.MainActivity
sleep 30
adb shell pidof com.example.app   # must still be alive
```
