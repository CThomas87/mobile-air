package com.nativephp.mobile.worker

/**
 * PhpSupervisorBridge — JNI bridge for the native Supervisor API.
 *
 * This Kotlin object provides the interface between the Android
 * Kotlin layer and the native C supervisor. All native methods
 * are registered in php_bridge.c via JNI_OnLoad.
 */
object PhpSupervisorBridge {

    /**
     * Initialize the PHP engine (module-level, once per app lifecycle).
     *
     * @param iniPath      Path to php.ini (or empty string for defaults)
     * @param iniOverrides Additional INI entries as "key=val\n..." (or empty)
     * @param appBasePath  Base path of the Laravel app
     * @return true on success
     */
    @JvmStatic
    external fun nativeEngineInit(iniPath: String, iniOverrides: String, appBasePath: String): Boolean

    /**
     * Start the supervisor with worker pool.
     *
     * @param mode         0=all, 1=queue, 2=scheduler
     * @param workerCount  Number of worker threads
     * @param queues       Comma-separated queue names
     * @param connection   Queue connection name (e.g., "database")
     * @return true on success
     */
    @JvmStatic
    external fun nativeStartSupervisor(mode: Int, workerCount: Int, queues: String, connection: String): Boolean

    /**
     * Stop the supervisor. Cancels pending work, waits for active jobs.
     */
    @JvmStatic
    external fun nativeStopSupervisor()

    /**
     * Enqueue a queue job for execution.
     *
     * @param payloadJson JSON string with job payload (or empty for auto-pop)
     * @param priority Job priority (-10 to +10, 0 = normal, higher = more urgent)
     * @return Job ID string, or null on failure
     */
    @JvmStatic
    external fun nativeEnqueueQueueJob(payloadJson: String, priority: Int = 0): String?

    /**
     * Enqueue a scheduler tick for execution.
     *
     * @param payloadJson JSON string with tick payload (or empty)
     * @return Job ID string, or null if tick already running
     */
    @JvmStatic
    external fun nativeEnqueueSchedulerTick(payloadJson: String): String?

    /**
     * Await a job result.
     *
     * @param jobId     Job ID to wait for
     * @param timeoutMs Timeout in milliseconds (0 = indefinite)
     * @return JSON result string { ok, stdout, stderr, exitCode, error, startedAt, endedAt }
     */
    @JvmStatic
    external fun nativeAwaitJob(jobId: String, timeoutMs: Int): String?

    /**
     * Cancel a pending or running job.
     *
     * @param jobId Job ID to cancel
     * @return true on success
     */
    @JvmStatic
    external fun nativeCancelJob(jobId: String): Boolean

    /**
     * Get supervisor status as JSON.
     *
     * @return JSON string { status, activeJobs, pendingJobs, completedJobs, failedJobs,
     *         schedulerRunning, uptimeSeconds, mode }
     */
    @JvmStatic
    external fun nativeGetStatus(): String

    /**
     * Shutdown the PHP engine. Call after stopping all work.
     */
    @JvmStatic
    external fun nativeEngineShutdown()

    // ─── Extended Configuration Methods ───

    /**
     * Configure the circuit breaker for crash recovery.
     *
     * @param maxConsecutiveCrashes  Pause thread after this many crashes (default 3)
     * @param baseBackoffSeconds     Initial backoff (doubles per crash, default 5)
     */
    @JvmStatic
    external fun nativeConfigureCircuitBreaker(maxConsecutiveCrashes: Int, baseBackoffSeconds: Int)

    /**
     * Set the memory_limit INI override for worker PHP requests.
     *
     * @param memoryLimit PHP memory_limit string (e.g., "64M")
     */
    @JvmStatic
    external fun nativeSetMemoryLimit(memoryLimit: String)

    /**
     * Enable structured worker logging to a file.
     *
     * @param logPath    Path to the worker log file
     * @param maxSizeKb  Maximum log file size in KB (ring buffer)
     */
    @JvmStatic
    external fun nativeSetLogFile(logPath: String, maxSizeKb: Int)

    /**
     * Wake all idle worker threads immediately.
     * Used for foreground immediate dispatch (<500ms latency).
     */
    @JvmStatic
    external fun nativeWakeWorkers()

    /**
     * Set native SQLite database path and pool size for queue peek + pooling.
     */
    @JvmStatic
    external fun nativeSetDbPath(dbPath: String, poolSize: Int)

    /**
     * Get native queue status JSON, e.g. {"total":N,"queues":{...}}.
     */
    @JvmStatic
    external fun nativeGetQueueStatus(): String

    init {
        // Native libraries are already loaded by PHPBridge companion init.
        // If this runs before PHPBridge, ensure libs are loaded:
        try {
            System.loadLibrary("compat") // JNI_OnLoad pre-loads libphp.so with RTLD_GLOBAL
            System.loadLibrary("php")
            System.loadLibrary("php_wrapper")
        } catch (e: UnsatisfiedLinkError) {
            // Already loaded — this is expected
        }
    }
}
