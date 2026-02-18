<?php

namespace Native\Mobile;

class System
{
    /**
     * Detect whether the current PHP execution is inside a native worker thread.
     *
     * The C supervisor sets NATIVEPHP_JOB_TYPE to "queue" or "scheduler"
     * before executing PHP scripts in worker threads. Calling nativephp_call()
     * from a worker thread can deadlock because the JNI bridge calls back into
     * the supervisor, which is waiting for the worker to finish.
     *
     * @return bool True if running inside a worker/scheduler thread
     */
    public static function isWorkerContext(): bool
    {
        // Prefer $_SERVER (per-thread in ZTS, immune to setenv race)
        // and avoid getenv() for HTTP requests, since worker threads may
        // temporarily set process-wide env vars.
        $serverJobType = $_SERVER['NATIVEPHP_JOB_TYPE'] ?? null;
        if (in_array($serverJobType, ['queue', 'scheduler'], true)) {
            return true;
        }

        if ($serverJobType !== null) {
            return false;
        }

        // getenv() fallback is only safe for CLI/non-request execution.
        if (PHP_SAPI === 'cli' || PHP_SAPI === 'phpdbg') {
            $envJobType = getenv('NATIVEPHP_JOB_TYPE') ?: null;

            return in_array($envJobType, ['queue', 'scheduler'], true);
        }

        return false;
    }

    /**
     * Toggle the device flashlight on/off.
     *
     * @deprecated Use \Native\Mobile\Facades\Device::toggleFlashlight() instead
     */
    #[\Deprecated(message: 'Use \Native\Mobile\Facades\Device::flashlight() instead', since: '2.0.0')]
    public function flashlight(): void
    {
        // Use the new god method pattern via Device class
        if (function_exists('nativephp_call')) {
            nativephp_call('Device.ToggleFlashlight', '{}');
        }
    }

    public function isIos(): bool
    {
        $info = \Native\Mobile\Facades\Device::getInfo();
        if ($info) {
            return json_decode($info)->platform === 'ios';
        }

        return false;
    }

    public function isAndroid(): bool
    {
        $info = \Native\Mobile\Facades\Device::getInfo();
        if ($info) {
            return json_decode($info)->platform === 'android';
        }

        return false;
    }

    public function isMobile(): bool
    {
        $info = \Native\Mobile\Facades\Device::getInfo();
        if ($info) {
            $platform = json_decode($info)->platform ?? null;

            return in_array($platform, ['ios', 'android']);
        }

        return false;
    }

    /**
     * Open the app's settings screen in the device settings.
     *
     * This allows users to manage permissions (e.g., push notifications,
     * camera, location) that they've granted or denied for the app.
     */
    public function appSettings(): void
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('System.OpenAppSettings', '{}');
        }
    }

    // ─── Background Worker Control ───

    /**
     * Start background worker processing.
     *
     * Starts the native supervisor with queue workers and/or scheduler
     * based on the configured mode. On Android this starts the foreground
     * service; on iOS it starts the in-process supervisor and schedules
     * BGProcessingTasks for background execution.
     *
     * @param  array  $options  Optional overrides: workerCount, queues, connection, mode
     * @return bool Whether the start request was accepted
     */
    public function startBackgroundWorker(array $options = []): bool
    {
        if (! function_exists('nativephp_call')) {
            return false;
        }

        $payload = json_encode(array_merge([
            'workerCount' => Worker\WorkerConfig::workerCount(),
            'queues' => Worker\WorkerConfig::queues(),
            'connection' => Worker\WorkerConfig::connection(),
            'mode' => Worker\WorkerConfig::mode(),
            'schedulerInterval' => Worker\WorkerConfig::schedulerIntervalSeconds(),
            'queuePollInterval' => Worker\WorkerConfig::queuePollIntervalSeconds(),
            'memoryLimit' => Worker\WorkerConfig::memoryLimit(),
            'circuitBreakerThreshold' => Worker\WorkerConfig::circuitBreakerThreshold(),
            'circuitBreakerBackoff' => Worker\WorkerConfig::circuitBreakerBackoff(),
            'immediateDispatch' => Worker\WorkerConfig::immediateDispatch(),
        ], $options));

        $result = nativephp_call('Worker.Start', $payload);

        return $result && json_decode($result, true)['started'] ?? false;
    }

    /**
     * Stop background worker processing.
     *
     * Gracefully shuts down the supervisor, drains active jobs,
     * and releases platform resources (foreground service, wakelock, etc.).
     */
    public function stopBackgroundWorker(): void
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('Worker.Stop', '{}');
        }
    }

    /**
     * Get the current worker/supervisor status.
     *
     * @return array{status: string, activeJobs: int, pendingJobs: int, completedJobs: int, failedJobs: int, schedulerRunning: bool, uptimeSeconds: int, mode: int}
     */
    public function workerStatus(): array
    {
        // Prevent JNI deadlock: calling Worker.Status from a worker thread
        // would call back into the supervisor that is waiting for this worker
        // to finish, creating a circular dependency.
        if (! function_exists('nativephp_call') || static::isWorkerContext()) {
            return [
                'status' => 'unavailable',
                'activeJobs' => 0,
                'pendingJobs' => 0,
                'completedJobs' => 0,
                'failedJobs' => 0,
                'schedulerRunning' => false,
                'uptimeSeconds' => 0,
                'mode' => 0,
            ];
        }

        $result = nativephp_call('Worker.Status', '{}');

        return json_decode($result, true) ?: [
            'status' => 'error',
            'activeJobs' => 0,
            'pendingJobs' => 0,
        ];
    }

    /**
     * Cancel a specific background job by ID.
     *
     * @param  string  $jobId  The native job ID (from supervisor)
     * @return bool Whether the cancellation was accepted
     */
    public function cancelJob(string $jobId): bool
    {
        if (! function_exists('nativephp_call') || static::isWorkerContext()) {
            return false;
        }

        $result = nativephp_call('Worker.CancelJob', json_encode(['jobId' => $jobId]));

        return $result && json_decode($result, true)['cancelled'] ?? false;
    }

    /**
     * Request battery optimization exemption from the OS.
     *
     * On Android, this opens the "Ignore Battery Optimizations" prompt.
     * On iOS, this is a no-op (iOS doesn't support this).
     *
     * Important for OEMs (Samsung, Xiaomi, Huawei) that aggressively
     * kill background services.
     */
    public function requestBatteryExemption(): void
    {
        if (function_exists('nativephp_call')) {
            nativephp_call('System.RequestBatteryExemption', '{}');
        }
    }

    /**
     * Send an event from a worker thread to the WebView.
     *
     * This uses the native-side event bus to push data from background
     * worker execution into the foreground UI. The event is delivered
     * as a JavaScript CustomEvent on `window`.
     *
     * @param  string  $event  Event name (e.g., 'job.completed')
     * @param  array   $data   Payload data
     */
    public function pushToWebView(string $event, array $data = []): void
    {
        // Skip JNI bridge calls in worker context to avoid potential deadlock.
        // The Worker.PushEvent call goes C→JNI→Kotlin→WebView, which may
        // contend with supervisor mutexes when called from a worker thread.
        if (! function_exists('nativephp_call') || static::isWorkerContext()) {
            return;
        }

        nativephp_call('Worker.PushEvent', json_encode([
            'event' => $event,
            'data' => $data,
        ]));
    }
}
