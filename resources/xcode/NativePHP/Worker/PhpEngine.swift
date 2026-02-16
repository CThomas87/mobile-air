import Foundation

/**
 * PhpEngine — Swift wrapper around the native C PhpEngine/Supervisor.
 *
 * Provides a clean Swift API for initializing the PHP runtime,
 * managing the supervisor, and executing background work.
 *
 * The underlying C functions (supervisor.h) are compiled into the PHP
 * framework and accessed via the Bridge module's C headers.
 */
class PhpEngine {
    static let shared = PhpEngine()

    private(set) var isInitialized = false
    private let queue = DispatchQueue(label: "com.nativephp.phpengine", qos: .userInitiated)

    private init() {}

    // MARK: - Engine Lifecycle

    /**
     * Initialize the PHP engine (module-level, once per app lifecycle).
     *
     * - Parameter iniPath: Path to php.ini (or nil for defaults)
     * - Parameter iniOverrides: Additional INI entries as "key=val\n..." (or nil)
     * - Parameter appBasePath: Base path of the Laravel app
     * - Returns: true on success
     */
    func initialize(iniPath: String? = nil, iniOverrides: String? = nil, appBasePath: String) -> Bool {
        return queue.sync {
            if isInitialized {
                return true
            }

            let result = supervisor_engine_init(
                iniPath.flatMap { $0.isEmpty ? nil : $0 },
                iniOverrides.flatMap { $0.isEmpty ? nil : $0 },
                appBasePath
            )

            isInitialized = (result == 0)
            return isInitialized
        }
    }

    /**
     * Shutdown the PHP engine. Call after all work is done.
     */
    func shutdown() {
        queue.sync {
            guard isInitialized else { return }
            supervisor_engine_shutdown()
            isInitialized = false
        }
    }

    // MARK: - Supervisor Control

    /**
     * Start the supervisor.
     *
     * - Parameter mode: 0=all, 1=queue only, 2=scheduler only
     * - Parameter workerCount: Number of worker threads (default 2)
     * - Parameter queues: Comma-separated queue names (default "default")
     * - Parameter connection: Queue connection name (default "database")
     * - Returns: true on success
     */
    func startSupervisor(mode: Int32 = 0, workerCount: Int32 = 2,
                          queues: String = "default", connection: String = "database") -> Bool {
        return queue.sync {
            guard isInitialized else { return false }
            let result = supervisor_start(supervisor_mode_t(rawValue: UInt32(mode)), workerCount, queues, connection)
            return result == 0
        }
    }

    /**
     * Stop the supervisor gracefully.
     */
    func stopSupervisor() {
        queue.sync {
            supervisor_stop()
        }
    }

    // MARK: - Job Operations

    /**
     * Enqueue a queue job for execution.
     *
     * - Parameter payload: JSON payload (or nil for auto-pop)
     * - Returns: Job ID string, or nil on failure
     */
    func enqueueQueueJob(payload: String? = nil) -> String? {
        let cResult = supervisor_enqueue_queue_job(payload)
        guard let cResult = cResult else { return nil }
        let jobId = String(cString: cResult)
        free(cResult)
        return jobId
    }

    /**
     * Enqueue a scheduler tick.
     *
     * - Parameter payload: JSON payload (or nil)
     * - Returns: Job ID string, or nil if tick already running
     */
    func enqueueSchedulerTick(payload: String? = nil) -> String? {
        let cResult = supervisor_enqueue_scheduler_tick(payload)
        guard let cResult = cResult else { return nil }
        let jobId = String(cString: cResult)
        free(cResult)
        return jobId
    }

    /**
     * Await a job result.
     *
     * - Parameter jobId: Job ID to wait for
     * - Parameter timeoutMs: Timeout in milliseconds (0 = indefinite)
     * - Returns: JSON result string, or nil on timeout
     */
    func awaitJob(jobId: String, timeoutMs: UInt32 = 0) -> String? {
        let cResult = supervisor_await_job(jobId, timeoutMs)
        guard let cResult = cResult else { return nil }
        let result = String(cString: cResult)
        free(cResult)
        return result
    }

    /**
     * Cancel a pending or running job.
     *
     * - Parameter jobId: Job ID to cancel
     * - Returns: true on success
     */
    func cancelJob(jobId: String) -> Bool {
        return supervisor_cancel_job(jobId) == 0
    }

    /**
     * Get supervisor status as JSON.
     */
    func getStatus() -> String {
        let cResult = supervisor_status_json()
        guard let cResult = cResult else { return "{\"status\":\"error\"}" }
        let status = String(cString: cResult)
        free(cResult)
        return status
    }

    // MARK: - Configuration Pass-through

    /**
     * Configure the circuit breaker for crash recovery.
     *
     * - Parameter maxCrashes: Max consecutive crashes before backoff (default 3)
     * - Parameter baseBackoff: Base backoff in seconds, doubles per crash (default 5)
     */
    func configureCircuitBreaker(maxCrashes: Int32 = 3, baseBackoff: Int32 = 5) {
        supervisor_configure_circuit_breaker(maxCrashes, baseBackoff)
    }

    /**
     * Set the memory_limit INI override for worker PHP requests.
     *
     * - Parameter limit: PHP memory_limit string (e.g., "64M")
     */
    func setMemoryLimit(_ limit: String) {
        supervisor_set_memory_limit(limit)
    }

    /**
     * Enable structured worker logging to a file.
     *
     * - Parameter path: Path to the worker log file
     * - Parameter maxSizeKB: Maximum log file size in KB (ring buffer, default 1024)
     */
    func setLogFile(path: String, maxSizeKB: Int32 = 1024) {
        supervisor_set_log_file(path, maxSizeKB)
    }

    /**
     * Wake all idle worker threads immediately.
     * Used for foreground immediate dispatch (<500ms latency).
     */
    func wakeWorkers() {
        supervisor_wake_workers()
    }
}
