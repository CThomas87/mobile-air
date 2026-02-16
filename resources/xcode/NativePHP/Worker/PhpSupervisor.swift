import Foundation

/**
 * PhpSupervisor — High-level Swift orchestrator for PHP background work.
 *
 * Manages the lifecycle of the PHP engine and supervisor:
 * - Starts/stops workers
 * - Runs periodic scheduler ticks
 * - Polls for queue jobs
 * - Integrates with BGProcessingTask via BackgroundTaskManager
 * - Supports foreground mode (unbounded, push-based wake for <500ms dispatch)
 * - Supports background mode (bounded execution within BGProcessingTask limits)
 */
class PhpSupervisor {
    static let shared = PhpSupervisor()

    private(set) var isRunning = false
    private(set) var isForeground = false
    private var schedulerTimer: DispatchSourceTimer?
    private var queuePollerTimer: DispatchSourceTimer?
    private let workQueue = DispatchQueue(label: "com.nativephp.supervisor", qos: .utility)

    // Configuration
    var workerCount: Int32 = 2
    var queues: String = "default"
    var connection: String = "database"
    var mode: Int32 = 0 // SUPERVISOR_MODE_ALL
    var schedulerIntervalSec: Double = 60.0
    var queuePollIntervalSec: Double = 5.0

    // Extended configuration
    var memoryLimit: String = "64M"
    var circuitBreakerMaxCrashes: Int32 = 3
    var circuitBreakerBaseBackoff: Int32 = 5
    var logEnabled: Bool = true
    var logMaxSizeKB: Int32 = 1024
    var immediateDispatch: Bool = true

    private init() {}

    // MARK: - Lifecycle

    /**
     * Start the supervisor with the PHP engine.
     *
     * - Parameter appBasePath: Laravel app base path
     * - Parameter iniPath: php.ini path (optional)
     * - Parameter foreground: If true, starts in unbounded foreground mode
     * - Returns: true on success
     */
    func start(appBasePath: String, iniPath: String? = nil, foreground: Bool = false) -> Bool {
        guard !isRunning else {
            DebugLogger.shared.log("⚙️ Supervisor already running")
            // If transitioning to foreground mode while already running, just switch mode
            if foreground && !isForeground {
                switchToForeground()
            }
            return true
        }

        DebugLogger.shared.log("⚙️ Starting PHP Supervisor (foreground=\(foreground))...")

        // Initialize engine
        guard PhpEngine.shared.initialize(iniPath: iniPath, appBasePath: appBasePath) else {
            DebugLogger.shared.log("❌ Failed to initialize PHP engine")
            return false
        }

        // Configure circuit breaker before starting supervisor
        PhpEngine.shared.configureCircuitBreaker(
            maxCrashes: circuitBreakerMaxCrashes,
            baseBackoff: circuitBreakerBaseBackoff
        )

        // Set memory limit
        PhpEngine.shared.setMemoryLimit(memoryLimit)

        // Configure worker log
        if logEnabled {
            let logPath = appBasePath + "/storage/logs/worker.log"
            PhpEngine.shared.setLogFile(path: logPath, maxSizeKB: logMaxSizeKB)
        }

        // Start supervisor
        guard PhpEngine.shared.startSupervisor(
            mode: mode,
            workerCount: workerCount,
            queues: queues,
            connection: connection
        ) else {
            DebugLogger.shared.log("❌ Failed to start supervisor")
            return false
        }

        isRunning = true
        isForeground = foreground
        DebugLogger.shared.log("✅ Supervisor started: workers=\(workerCount) queues=\(queues) foreground=\(foreground)")

        // Start periodic scheduler ticks
        if mode == 0 || mode == 2 {
            startSchedulerLoop()
        }

        // Start queue polling
        if mode == 0 || mode == 1 {
            startQueuePoller()
        }

        return true
    }

    /**
     * Stop the supervisor gracefully.
     */
    func stop() {
        guard isRunning else { return }

        DebugLogger.shared.log("⚙️ Stopping supervisor...")

        // Cancel timers
        schedulerTimer?.cancel()
        schedulerTimer = nil
        queuePollerTimer?.cancel()
        queuePollerTimer = nil

        // Stop native supervisor
        PhpEngine.shared.stopSupervisor()

        isRunning = false
        isForeground = false
        DebugLogger.shared.log("✅ Supervisor stopped")
    }

    // MARK: - Foreground/Background Transitions

    /**
     * Switch to foreground mode (unbounded execution, push-based wake).
     * Called when the app enters the foreground while supervisor is running.
     */
    func switchToForeground() {
        guard isRunning else { return }
        isForeground = true
        DebugLogger.shared.log("⚙️ Supervisor switched to FOREGROUND mode")

        // Restart queue poller with faster polling for foreground
        if mode == 0 || mode == 1 {
            queuePollerTimer?.cancel()
            queuePollerTimer = nil
            startQueuePoller()
        }
    }

    /**
     * Switch to background mode (bounded execution).
     * Called when the app enters the background.
     */
    func switchToBackground() {
        guard isRunning else { return }
        isForeground = false
        DebugLogger.shared.log("⚙️ Supervisor switched to BACKGROUND mode")

        // Don't stop — let timers continue briefly during background transition.
        // BackgroundTaskManager will handle BGProcessingTask scheduling.
    }

    /**
     * Wake workers immediately for foreground dispatch.
     * Call this after dispatching a job to achieve <500ms latency.
     */
    func wakeForImmediateDispatch() {
        guard isRunning && isForeground && immediateDispatch else { return }
        PhpEngine.shared.wakeWorkers()
    }

    /**
     * Run a bounded amount of work (for BGProcessingTask).
     * Returns after maxDuration seconds or when cancelled.
     *
     * - Parameter maxDuration: Maximum seconds to run
     * - Parameter cancelled: Check this closure to see if we should stop
     */
    func runBounded(maxDuration: TimeInterval, cancelled: @escaping () -> Bool) {
        guard isRunning else { return }

        let deadline = Date().addingTimeInterval(maxDuration)

        DebugLogger.shared.log("⚙️ Running bounded work for \(maxDuration)s")

        while Date() < deadline && !cancelled() && isRunning {
            // Enqueue a queue job
            if mode == 0 || mode == 1 {
                if let jobId = PhpEngine.shared.enqueueQueueJob() {
                    // Await with a reasonable timeout
                    let _ = PhpEngine.shared.awaitJob(jobId: jobId, timeoutMs: 30_000)
                }
            }

            // Enqueue a scheduler tick if due
            if mode == 0 || mode == 2 {
                if let jobId = PhpEngine.shared.enqueueSchedulerTick() {
                    let _ = PhpEngine.shared.awaitJob(jobId: jobId, timeoutMs: 60_000)
                }
            }

            // Brief sleep between iterations
            if !cancelled() {
                Thread.sleep(forTimeInterval: 2.0)
            }
        }

        DebugLogger.shared.log("⚙️ Bounded work completed")
    }

    // MARK: - Periodic Loops

    private func startSchedulerLoop() {
        let timer = DispatchSource.makeTimerSource(queue: workQueue)
        timer.schedule(deadline: .now(), repeating: schedulerIntervalSec)
        timer.setEventHandler { [weak self] in
            guard let self = self, self.isRunning else { return }

            if let jobId = PhpEngine.shared.enqueueSchedulerTick() {
                DebugLogger.shared.log("⏰ Scheduler tick enqueued: \(jobId)")
                _ = PhpEngine.shared.awaitJob(jobId: jobId, timeoutMs: 5 * 60 * 1000)
            }
        }
        timer.resume()
        schedulerTimer = timer
    }

    private func startQueuePoller() {
        // In foreground mode, poll more aggressively
        let interval = isForeground ? max(1.0, queuePollIntervalSec / 2.0) : queuePollIntervalSec

        let timer = DispatchSource.makeTimerSource(queue: workQueue)
        timer.schedule(deadline: .now(), repeating: interval)
        timer.setEventHandler { [weak self] in
            guard let self = self, self.isRunning else { return }

            if let jobId = PhpEngine.shared.enqueueQueueJob() {
                DebugLogger.shared.log("📦 Queue job enqueued: \(jobId)")
                _ = PhpEngine.shared.awaitJob(jobId: jobId, timeoutMs: 60_000)
            }
        }
        timer.resume()
        queuePollerTimer = timer
    }
}
