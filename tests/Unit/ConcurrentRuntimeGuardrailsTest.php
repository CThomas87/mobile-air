<?php

namespace Tests\Unit;

use Tests\TestCase;

class ConcurrentRuntimeGuardrailsTest extends TestCase
{
    private function repoFile(string $path): string
    {
        return dirname(__DIR__, 2).DIRECTORY_SEPARATOR.str_replace('/', DIRECTORY_SEPARATOR, $path);
    }

    public function test_cmake_checks_zts_status(): void
    {
        $cmake = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/CMakeLists.txt'));

        // CMake must check for ZTS status (fatal error if absent, status if present)
        $this->assertStringContainsString('message(FATAL_ERROR', $cmake);
        $this->assertStringContainsString('"#define ZTS 1"', $cmake);
        $this->assertStringContainsString('"/* #undef ZTS */"', $cmake);
    }

    public function test_php_runtime_units_include_zts_guard_header(): void
    {
        $files = [
            'resources/androidstudio/app/src/main/cpp/php_engine.c',
            'resources/androidstudio/app/src/main/cpp/php_thread_context.c',
            'resources/androidstudio/app/src/main/cpp/php_request_context.c',
            'resources/androidstudio/app/src/main/cpp/worker_pool.c',
            'resources/androidstudio/app/src/main/cpp/supervisor.c',
        ];

        foreach ($files as $file) {
            $content = file_get_contents($this->repoFile($file));
            $this->assertStringContainsString('#include "zts_guard.h"', $content, "Missing zts_guard include in {$file}");
        }
    }

    public function test_per_job_env_mutation_is_not_used_in_request_context(): void
    {
        $requestCtx = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/php_request_context.c'));

        $this->assertStringNotContainsString('setenv("NATIVEPHP_JOB_', $requestCtx);
        $this->assertStringNotContainsString('unsetenv("NATIVEPHP_JOB_', $requestCtx);
    }

    public function test_scheduler_gate_is_released_on_job_completion_without_await(): void
    {
        $workerPool = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/worker_pool.c'));

        $this->assertStringContainsString('supervisor_notify_job_completed(php_request_get_job_id(ctx));', $workerPool);
    }

    public function test_android_worker_service_awaits_enqueued_jobs_and_persists_boot_config(): void
    {
        $service = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/java/com/nativephp/mobile/worker/PhpWorkerService.kt'));
        $mainActivity = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/java/com/nativephp/mobile/ui/MainActivity.kt'));
        $workerFunctions = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/java/com/nativephp/mobile/bridge/functions/WorkerFunctions.kt'));

        $this->assertStringContainsString('return START_STICKY', $service);
        $this->assertStringContainsString('WorkerBootReceiver.saveConfig(', $service);
        $this->assertStringContainsString('WorkerBootReceiver.clearConfig(this)', $service);
        $this->assertStringContainsString('nativeAwaitJob(jobId, 5 * 60 * 1000)', $service);
        $this->assertStringContainsString('nativeAwaitJob(jobId, 60 * 1000)', $service);

        // Worker appBasePath must point to extracted Laravel root.
        $this->assertStringContainsString('File(appStorageDir, "laravel").absolutePath', $mainActivity);
        $this->assertStringContainsString('java.io.File(appStorageDir, "laravel").absolutePath', $workerFunctions);
    }

    public function test_ios_background_task_manager_is_wired_in_app_delegate(): void
    {
        $appDelegate = file_get_contents($this->repoFile('resources/xcode/NativePHP/AppDelegate.swift'));

        $this->assertStringContainsString('BackgroundTaskManager.shared.registerTasks()', $appDelegate);
        $this->assertStringContainsString('func applicationDidEnterBackground(_ application: UIApplication)', $appDelegate);
        $this->assertStringContainsString('BackgroundTaskManager.shared.appDidEnterBackground()', $appDelegate);
        $this->assertStringContainsString('func applicationWillEnterForeground(_ application: UIApplication)', $appDelegate);
        $this->assertStringContainsString('BackgroundTaskManager.shared.appWillEnterForeground()', $appDelegate);
    }

    public function test_queue_worker_does_not_call_get_class_on_resolve_name_string(): void
    {
        $worker = file_get_contents($this->repoFile('bootstrap/worker/queue_worker.php'));

        $this->assertStringNotContainsString('get_class($job->resolveName())', $worker);
        $this->assertStringContainsString('$result[\'job_name\'] = $job->resolveName();', $worker);
    }

    public function test_engine_init_wires_tls_ub_write_for_output_capture(): void
    {
        $engine = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/php_engine.c'));

        // The engine MUST install php_request_ub_write after php_embed_init
        // so that worker thread output is captured per-job via TLS routing.
        $this->assertStringContainsString(
            'php_embed_module.ub_write = php_request_ub_write;',
            $engine,
            'php_engine.c must wire php_request_ub_write as the SAPI output handler for concurrent worker output capture'
        );

        // It must also include the request context header for the function declaration
        $this->assertStringContainsString(
            '#include "php_request_context.h"',
            $engine,
            'php_engine.c must include php_request_context.h for php_request_ub_write declaration'
        );
    }

    public function test_supervisor_await_does_not_double_release_scheduler_gate(): void
    {
        $supervisor = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/supervisor.c'));

        // The notify_job_completed callback releases the scheduler gate when
        // a scheduler job finishes. supervisor_await_job must NOT also release
        // it, or the gate will be double-released.
        $this->assertStringNotContainsString(
            'scheduler_gate_release(s_sched_gate);' . "\n" . '    }' . "\n" . "\n" . '    char *json = php_request_to_json(ctx);',
            $supervisor,
            'supervisor_await_job should not release the scheduler gate (notify_job_completed handles it)'
        );
    }

    public function test_output_routing_uses_tls_not_global_buffer(): void
    {
        $requestCtx = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/php_request_context.c'));

        // Output must go through TLS (thread-local storage) routing, not global state
        $this->assertStringContainsString('static __thread php_request_context_t *tls_current_ctx', $requestCtx);
        $this->assertStringContainsString('tls_current_ctx', $requestCtx);

        // The ub_write function must use TLS to find the owning context
        $this->assertStringContainsString('php_request_context_t *ctx = tls_current_ctx;', $requestCtx);
    }

    public function test_zts_runtime_degradation_to_single_worker(): void
    {
        $supervisor = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/supervisor.c'));

        // If ZTS verification fails at runtime, supervisor must degrade safely
        $this->assertStringContainsString('degrading to safe single-worker mode', $supervisor);
        $this->assertStringContainsString('worker_count = 1', $supervisor);
    }

    public function test_worker_pool_stop_cancels_and_joins_all_threads(): void
    {
        $pool = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/worker_pool.c'));

        // Must cancel pending and active jobs
        $this->assertStringContainsString('php_request_cancel(node->ctx)', $pool);
        $this->assertStringContainsString('php_request_cancel(pool->active_jobs[i])', $pool);

        // Must join all threads (no orphans)
        $this->assertStringContainsString('pthread_join(pool->threads[i], NULL)', $pool);
    }

    public function test_ios_bg_task_always_calls_set_task_completed(): void
    {
        $manager = file_get_contents($this->repoFile('resources/xcode/NativePHP/Worker/BackgroundTaskManager.swift'));

        // Both handlers must call setTaskCompleted
        $count = substr_count($manager, 'task.setTaskCompleted(success:');
        $this->assertGreaterThanOrEqual(2, $count,
            'Both queue and scheduler task handlers must call setTaskCompleted');
    }

    public function test_ios_bg_task_always_reschedules(): void
    {
        $manager = file_get_contents($this->repoFile('resources/xcode/NativePHP/Worker/BackgroundTaskManager.swift'));

        // Both handlers must reschedule after completion
        $this->assertStringContainsString('self.scheduleQueueTask()', $manager);
        $this->assertStringContainsString('self.scheduleSchedulerTask()', $manager);
    }

    public function test_ios_bg_task_expiration_handler_cancels_work(): void
    {
        $manager = file_get_contents($this->repoFile('resources/xcode/NativePHP/Worker/BackgroundTaskManager.swift'));

        // Both handlers must set cancellation flag in expirationHandler
        $this->assertStringContainsString('task.expirationHandler', $manager);
        $this->assertStringContainsString('isCancelled = true', $manager);
        $this->assertStringContainsString('PhpSupervisor.shared.stop()', $manager);
    }

    public function test_android_service_is_sticky_and_has_foreground_notification(): void
    {
        $service = file_get_contents($this->repoFile(
            'resources/androidstudio/app/src/main/java/com/nativephp/mobile/worker/PhpWorkerService.kt'));

        $this->assertStringContainsString('return START_STICKY', $service);
        $this->assertStringContainsString('startForeground(NOTIFICATION_ID', $service);
        $this->assertStringContainsString('createNotificationChannel()', $service);
        $this->assertStringContainsString('PARTIAL_WAKE_LOCK', $service);
    }

    public function test_android_wakelock_always_released_on_stop(): void
    {
        $service = file_get_contents($this->repoFile(
            'resources/androidstudio/app/src/main/java/com/nativephp/mobile/worker/PhpWorkerService.kt'));

        // handleStop must release the wakelock
        $this->assertStringContainsString('releaseWakeLock()', $service);
        $this->assertStringContainsString('it.release()', $service);
        $this->assertStringContainsString('wakeLock = null', $service);
    }

    public function test_android_manifest_declares_required_permissions(): void
    {
        $manifest = file_get_contents($this->repoFile('resources/stubs/android/worker-manifest-additions.xml'));

        $this->assertStringContainsString('android.permission.FOREGROUND_SERVICE', $manifest);
        $this->assertStringContainsString('android.permission.WAKE_LOCK', $manifest);
        $this->assertStringContainsString('foregroundServiceType="dataSync"', $manifest);
        $this->assertStringContainsString('RECEIVE_BOOT_COMPLETED', $manifest);
    }

    public function test_worker_service_provider_is_auto_discovered(): void
    {
        $composer = file_get_contents($this->repoFile('composer.json'));
        $decoded = json_decode($composer, true);

        $providers = $decoded['extra']['laravel']['providers'] ?? [];
        $this->assertContains(
            'Native\\Mobile\\Worker\\WorkerServiceProvider',
            $providers,
            'WorkerServiceProvider must be in composer.json extra.laravel.providers for auto-discovery'
        );
    }

    public function test_queue_worker_bootstraps_laravel_without_app_edits(): void
    {
        $worker = file_get_contents($this->repoFile('bootstrap/worker/queue_worker.php'));

        // Must auto-discover autoload.php (zero app edits)
        $this->assertStringContainsString('autoload.php', $worker);
        $this->assertStringContainsString('bootstrap/app.php', $worker);

        // WAL mode is handled by WorkerServiceProvider (not duplicated here).
        // Verify it defers to the service provider instead.
        $this->assertStringContainsString('WorkerServiceProvider', $worker);

        // Must output JSON result
        $this->assertStringContainsString('json_encode($result', $worker);
    }

    public function test_scheduler_tick_runs_schedule_run_artisan_command(): void
    {
        $tick = file_get_contents($this->repoFile('bootstrap/worker/scheduler_tick.php'));

        $this->assertStringContainsString("'schedule:run'", $tick);
        $this->assertStringContainsString('json_encode($result', $tick);

        // WAL mode is handled by WorkerServiceProvider (not duplicated here)
        $this->assertStringContainsString('WorkerServiceProvider', $tick);
    }

    // ─── Circuit Breaker & Cancellation Tests ───

    public function test_supervisor_has_circuit_breaker_configuration(): void
    {
        $supervisor_h = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/supervisor.h'));
        $supervisor_c = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/supervisor.c'));

        // Header declares the API
        $this->assertStringContainsString('supervisor_configure_circuit_breaker', $supervisor_h);
        $this->assertStringContainsString('supervisor_set_memory_limit', $supervisor_h);
        $this->assertStringContainsString('supervisor_set_log_file', $supervisor_h);
        $this->assertStringContainsString('supervisor_wake_workers', $supervisor_h);
        $this->assertStringContainsString('supervisor_notify_job_failed', $supervisor_h);

        // Implementation exists
        $this->assertStringContainsString('s_cb_max_crashes', $supervisor_c);
        $this->assertStringContainsString('s_cb_base_backoff_sec', $supervisor_c);
        $this->assertStringContainsString('sv_log_event', $supervisor_c);
    }

    public function test_worker_pool_has_circuit_breaker_in_thread_loop(): void
    {
        $pool = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/worker_pool.c'));

        $this->assertStringContainsString('consecutive_crashes', $pool);
        $this->assertStringContainsString('supervisor_get_circuit_breaker_max', $pool);
        $this->assertStringContainsString('supervisor_get_circuit_breaker_backoff', $pool);
        $this->assertStringContainsString('supervisor_notify_job_failed', $pool);
    }

    public function test_cooperative_cancellation_uses_zend_interrupt_function(): void
    {
        $ctx_c = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/php_request_context.c'));
        $ctx_h = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/php_request_context.h'));

        // Must install zend_interrupt_function handler
        $this->assertStringContainsString('zend_interrupt_function = php_request_interrupt_handler', $ctx_c);

        // Must restore previous handler on cleanup
        $this->assertStringContainsString('zend_interrupt_function = tls_prev_interrupt_function', $ctx_c);

        // Must use zend_bailout for clean exit on cancellation
        $this->assertStringContainsString('zend_bailout()', $ctx_c);

        // Header declares the interrupt handler
        $this->assertStringContainsString('php_request_interrupt_handler', $ctx_h);
        $this->assertStringContainsString('php_request_set_cancelled_interrupt', $ctx_h);
    }

    public function test_memory_limit_injected_per_request(): void
    {
        $ctx_c = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/php_request_context.c'));

        // Must inject memory_limit via zend_eval_string
        $this->assertStringContainsString('supervisor_get_memory_limit', $ctx_c);
        $this->assertStringContainsString("ini_set('memory_limit'", $ctx_c);
    }

    public function test_supervisor_status_includes_extended_fields(): void
    {
        $supervisor_c = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/supervisor.c'));

        $this->assertStringContainsString('completedJobs', $supervisor_c);
        $this->assertStringContainsString('failedJobs', $supervisor_c);
        $this->assertStringContainsString('uptimeSeconds', $supervisor_c);
    }

    public function test_worker_pool_wake_function_exists(): void
    {
        $pool_h = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/worker_pool.h'));
        $pool_c = file_get_contents($this->repoFile('resources/androidstudio/app/src/main/cpp/worker_pool.c'));

        $this->assertStringContainsString('worker_pool_wake', $pool_h);
        $this->assertStringContainsString('worker_pool_wake', $pool_c);
        $this->assertStringContainsString('pthread_cond_broadcast(&pool->queue_cond)', $pool_c);
    }

    // ─── Swift Layer Tests ───

    public function test_ios_supervisor_supports_foreground_mode(): void
    {
        $supervisor = file_get_contents($this->repoFile('resources/xcode/NativePHP/Worker/PhpSupervisor.swift'));

        $this->assertStringContainsString('isForeground', $supervisor);
        $this->assertStringContainsString('switchToForeground', $supervisor);
        $this->assertStringContainsString('switchToBackground', $supervisor);
        $this->assertStringContainsString('wakeForImmediateDispatch', $supervisor);
    }

    public function test_ios_engine_has_circuit_breaker_config(): void
    {
        $engine = file_get_contents($this->repoFile('resources/xcode/NativePHP/Worker/PhpEngine.swift'));

        $this->assertStringContainsString('configureCircuitBreaker', $engine);
        $this->assertStringContainsString('setMemoryLimit', $engine);
        $this->assertStringContainsString('setLogFile', $engine);
        $this->assertStringContainsString('wakeWorkers', $engine);
    }

    public function test_ios_bg_task_max_duration_is_25_seconds(): void
    {
        $manager = file_get_contents($this->repoFile('resources/xcode/NativePHP/Worker/BackgroundTaskManager.swift'));

        // Must be 25 seconds (NOT 25 * 60)
        $this->assertStringContainsString('maxBackgroundDuration: TimeInterval = 25', $manager);
        $this->assertStringNotContainsString('25 * 60', $manager);
    }

    public function test_ios_bg_task_uses_begin_background_task_for_transition(): void
    {
        $manager = file_get_contents($this->repoFile('resources/xcode/NativePHP/Worker/BackgroundTaskManager.swift'));

        $this->assertStringContainsString('beginBackgroundTask', $manager);
        $this->assertStringContainsString('endBackgroundTask', $manager);
        $this->assertStringContainsString('NativePHP.Transition', $manager);
    }

    public function test_ios_foreground_starts_supervisor_on_enter_foreground(): void
    {
        $manager = file_get_contents($this->repoFile('resources/xcode/NativePHP/Worker/BackgroundTaskManager.swift'));

        $this->assertStringContainsString('foreground: true', $manager);
        $this->assertStringContainsString('switchToForeground', $manager);
    }

    // ─── Kotlin Layer Tests ───

    public function test_android_bridge_has_extended_native_methods(): void
    {
        $bridge = file_get_contents($this->repoFile(
            'resources/androidstudio/app/src/main/java/com/nativephp/mobile/worker/PhpSupervisorBridge.kt'));

        $this->assertStringContainsString('nativeConfigureCircuitBreaker', $bridge);
        $this->assertStringContainsString('nativeSetMemoryLimit', $bridge);
        $this->assertStringContainsString('nativeSetLogFile', $bridge);
        $this->assertStringContainsString('nativeWakeWorkers', $bridge);
    }

    public function test_android_service_configures_circuit_breaker_before_start(): void
    {
        $service = file_get_contents($this->repoFile(
            'resources/androidstudio/app/src/main/java/com/nativephp/mobile/worker/PhpWorkerService.kt'));

        // Circuit breaker must be configured BEFORE nativeStartSupervisor
        $cbPos = strpos($service, 'nativeConfigureCircuitBreaker');
        $startPos = strpos($service, 'nativeStartSupervisor');

        $this->assertNotFalse($cbPos, 'Service must call nativeConfigureCircuitBreaker');
        $this->assertNotFalse($startPos, 'Service must call nativeStartSupervisor');
        $this->assertLessThan($startPos, $cbPos,
            'Circuit breaker must be configured before supervisor starts');
    }

    public function test_android_service_has_battery_exemption(): void
    {
        $service = file_get_contents($this->repoFile(
            'resources/androidstudio/app/src/main/java/com/nativephp/mobile/worker/PhpWorkerService.kt'));

        $this->assertStringContainsString('ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS', $service);
        $this->assertStringContainsString('isIgnoringBatteryOptimizations', $service);
        $this->assertStringContainsString('requestBatteryExemption', $service);
    }

    public function test_android_service_has_fgs_type_for_api_34(): void
    {
        $service = file_get_contents($this->repoFile(
            'resources/androidstudio/app/src/main/java/com/nativephp/mobile/worker/PhpWorkerService.kt'));

        $this->assertStringContainsString('FOREGROUND_SERVICE_TYPE_DATA_SYNC', $service);
        $this->assertStringContainsString('UPSIDE_DOWN_CAKE', $service);
    }

    public function test_android_service_has_immediate_wake(): void
    {
        $service = file_get_contents($this->repoFile(
            'resources/androidstudio/app/src/main/java/com/nativephp/mobile/worker/PhpWorkerService.kt'));

        $this->assertStringContainsString('ACTION_WAKE', $service);
        $this->assertStringContainsString('handleWake', $service);
        $this->assertStringContainsString('nativeWakeWorkers', $service);
    }

    // ─── PHP Layer Tests ───

    public function test_system_facade_has_worker_control_methods(): void
    {
        $system = file_get_contents($this->repoFile('src/System.php'));
        $facade = file_get_contents($this->repoFile('src/Facades/System.php'));

        $methods = [
            'startBackgroundWorker',
            'stopBackgroundWorker',
            'workerStatus',
            'cancelJob',
            'requestBatteryExemption',
            'pushToWebView',
        ];

        foreach ($methods as $method) {
            $this->assertStringContainsString($method, $system, "System.php must have {$method}");
            $this->assertStringContainsString($method, $facade, "System facade must have {$method} annotation");
        }
    }

    public function test_worker_events_exist(): void
    {
        $events = [
            'src/Events/Worker/JobCompleted.php',
            'src/Events/Worker/JobFailed.php',
            'src/Events/Worker/WorkerStarted.php',
            'src/Events/Worker/WorkerStopped.php',
            'src/Events/Worker/CircuitBreakerTripped.php',
            'src/Events/Worker/SchedulerTickCompleted.php',
        ];

        foreach ($events as $event) {
            $this->assertFileExists($this->repoFile($event), "Missing event file: {$event}");
        }
    }

    public function test_worker_command_registered(): void
    {
        $provider = file_get_contents($this->repoFile('src/NativeServiceProvider.php'));

        $this->assertStringContainsString('WorkerCommand::class', $provider);
    }

    public function test_tail_command_supports_worker_type(): void
    {
        $tail = file_get_contents($this->repoFile('src/Commands/TailCommand.php'));

        $this->assertStringContainsString("--type=", $tail);
        $this->assertStringContainsString("worker", $tail);
        $this->assertStringContainsString("worker.log", $tail);
    }

    public function test_worker_status_api_endpoint_exists(): void
    {
        $api = file_get_contents($this->repoFile('routes/api.php'));

        $this->assertStringContainsString('worker/status', $api);
        $this->assertStringContainsString('WorkerStatusController', $api);
    }

    public function test_dashboard_blade_component_exists(): void
    {
        $this->assertFileExists(
            $this->repoFile('resources/views/components/worker-dashboard.blade.php'),
            'Worker dashboard Blade component must exist'
        );
    }

    public function test_config_has_platform_specific_runtimes(): void
    {
        $config = file_get_contents($this->repoFile('config/nativephp-worker.php'));

        $this->assertStringContainsString('max_background_runtime_android', $config);
        $this->assertStringContainsString('max_background_runtime_ios', $config);
        $this->assertStringContainsString('circuit_breaker_threshold', $config);
        $this->assertStringContainsString('circuit_breaker_backoff', $config);
        $this->assertStringContainsString('memory_limit', $config);
        $this->assertStringContainsString('override_sync_driver', $config);
        $this->assertStringContainsString('immediate_dispatch', $config);
        $this->assertStringContainsString('max_worker_count', $config);
    }

    public function test_worker_service_provider_auto_creates_queue_tables(): void
    {
        $provider = file_get_contents($this->repoFile('src/Worker/WorkerServiceProvider.php'));

        $this->assertStringContainsString('ensureQueueTablesExist', $provider);
        $this->assertStringContainsString("Schema::create('jobs'", $provider);
        $this->assertStringContainsString("Schema::create('failed_jobs'", $provider);
        $this->assertStringContainsString("Schema::create('job_batches'", $provider);
    }

    public function test_worker_config_enforces_hard_cap(): void
    {
        $config = file_get_contents($this->repoFile('src/Worker/WorkerConfig.php'));

        $this->assertStringContainsString('MAX_WORKER_HARD_CAP', $config);
        $this->assertStringContainsString('Log::warning', $config);
    }
}
