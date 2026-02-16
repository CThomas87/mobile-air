import Foundation
import BackgroundTasks
import UIKit

/**
 * BackgroundTaskManager — iOS BGProcessingTask integration.
 *
 * Implements the BGTaskScheduler cycle pattern:
 * 1. Register task identifier(s) at app launch
 * 2. Schedule BGProcessingTaskRequest with constraints
 * 3. On task run: start supervisor, run bounded work, setTaskCompleted
 * 4. expirationHandler: cancel work cleanly, complete task
 * 5. ALWAYS reschedule at the end (cycle pattern)
 *
 * Also manages foreground/background transitions:
 * - Foreground: supervisor runs unbounded with push-based wake
 * - Background: supervisor runs bounded in BGProcessingTask (25s max)
 * - Transition: UIApplication.beginBackgroundTask provides 30s grace period
 *
 * Task identifiers:
 * - com.nativephp.worker.queue   — Queue job processing
 * - com.nativephp.worker.scheduler — Scheduler tick running
 */
class BackgroundTaskManager {
    static let shared = BackgroundTaskManager()

    // Task identifiers — must match Info.plist BGTaskSchedulerPermittedIdentifiers
    static let queueTaskIdentifier = "com.nativephp.worker.queue"
    static let schedulerTaskIdentifier = "com.nativephp.worker.scheduler"

    // Configuration
    var appBasePath: String = ""
    var iniPath: String? = nil
    var workerCount: Int32 = 2
    var queues: String = "default"
    var connection: String = "database"
    var maxBackgroundDuration: TimeInterval = 25 // 25 seconds (iOS BGProcessingTask limit with 5s safety margin)

    private var isRegistered = false
    private var backgroundTaskID: UIBackgroundTaskIdentifier = .invalid

    private init() {}

    // MARK: - Registration

    /**
     * Register background task identifiers with BGTaskScheduler.
     * MUST be called before app finishes launching (in application(_:didFinishLaunchingWithOptions:)).
     */
    func registerTasks() {
        guard !isRegistered else { return }

        let queueRegistered = BGTaskScheduler.shared.register(
            forTaskWithIdentifier: Self.queueTaskIdentifier,
            using: nil
        ) { task in
            self.handleQueueTask(task as! BGProcessingTask)
        }

        let schedulerRegistered = BGTaskScheduler.shared.register(
            forTaskWithIdentifier: Self.schedulerTaskIdentifier,
            using: nil
        ) { task in
            self.handleSchedulerTask(task as! BGProcessingTask)
        }

        isRegistered = queueRegistered && schedulerRegistered

        if isRegistered {
            DebugLogger.shared.log("✅ Background tasks registered")
        } else {
            DebugLogger.shared.log("⚠️ Failed to register some background tasks (queue=\(queueRegistered) scheduler=\(schedulerRegistered))")
        }
    }

    // MARK: - Scheduling

    /**
     * Schedule the queue processing background task.
     * Call this after any foreground work, and at the end of each background task run.
     */
    func scheduleQueueTask() {
        let request = BGProcessingTaskRequest(identifier: Self.queueTaskIdentifier)
        request.requiresNetworkConnectivity = false  // SQLite queue works offline
        request.requiresExternalPower = false
        request.earliestBeginDate = Date(timeIntervalSinceNow: 5 * 60) // 5 minutes

        do {
            try BGTaskScheduler.shared.submit(request)
            DebugLogger.shared.log("📅 Queue background task scheduled")
        } catch {
            DebugLogger.shared.log("❌ Failed to schedule queue task: \(error)")
        }
    }

    /**
     * Schedule the scheduler tick background task.
     */
    func scheduleSchedulerTask() {
        let request = BGProcessingTaskRequest(identifier: Self.schedulerTaskIdentifier)
        request.requiresNetworkConnectivity = false
        request.requiresExternalPower = false
        request.earliestBeginDate = Date(timeIntervalSinceNow: 60) // 1 minute

        do {
            try BGTaskScheduler.shared.submit(request)
            DebugLogger.shared.log("📅 Scheduler background task scheduled")
        } catch {
            DebugLogger.shared.log("❌ Failed to schedule scheduler task: \(error)")
        }
    }

    /**
     * Schedule both tasks (convenience).
     */
    func scheduleAllTasks() {
        scheduleQueueTask()
        scheduleSchedulerTask()
    }

    // MARK: - Task Handlers

    private func handleQueueTask(_ task: BGProcessingTask) {
        DebugLogger.shared.log("🔄 Queue background task started")

        var isCancelled = false

        // Expiration handler — clean cancellation
        task.expirationHandler = {
            DebugLogger.shared.log("⏰ Queue task expiring, cancelling work...")
            isCancelled = true
            PhpSupervisor.shared.stop()
        }

        DispatchQueue.global(qos: .utility).async {
            // Start supervisor in queue-only mode
            let supervisor = PhpSupervisor.shared
            supervisor.mode = 1 // SUPERVISOR_MODE_QUEUE
            supervisor.workerCount = self.workerCount
            supervisor.queues = self.queues
            supervisor.connection = self.connection

            let started = supervisor.start(
                appBasePath: self.appBasePath,
                iniPath: self.iniPath,
                foreground: false
            )

            if started {
                // Run bounded work
                supervisor.runBounded(
                    maxDuration: self.maxBackgroundDuration,
                    cancelled: { isCancelled }
                )
                supervisor.stop()
            }

            // Mark task completed
            let success = started && !isCancelled
            task.setTaskCompleted(success: success)
            DebugLogger.shared.log("✅ Queue background task completed (success=\(success))")

            // ALWAYS reschedule (cycle pattern)
            self.scheduleQueueTask()
        }
    }

    private func handleSchedulerTask(_ task: BGProcessingTask) {
        DebugLogger.shared.log("🔄 Scheduler background task started")

        var isCancelled = false

        // Expiration handler
        task.expirationHandler = {
            DebugLogger.shared.log("⏰ Scheduler task expiring, cancelling work...")
            isCancelled = true
            PhpSupervisor.shared.stop()
        }

        DispatchQueue.global(qos: .utility).async {
            let supervisor = PhpSupervisor.shared
            supervisor.mode = 2 // SUPERVISOR_MODE_SCHEDULER
            supervisor.workerCount = 1

            let started = supervisor.start(
                appBasePath: self.appBasePath,
                iniPath: self.iniPath,
                foreground: false
            )

            if started {
                // Run one scheduler tick
                if let jobId = PhpEngine.shared.enqueueSchedulerTick() {
                    let _ = PhpEngine.shared.awaitJob(jobId: jobId, timeoutMs: 5 * 60 * 1000) // 5 min timeout
                }
                supervisor.stop()
            }

            let success = started && !isCancelled
            task.setTaskCompleted(success: success)
            DebugLogger.shared.log("✅ Scheduler background task completed (success=\(success))")

            // ALWAYS reschedule
            self.scheduleSchedulerTask()
        }
    }

    // MARK: - App Lifecycle Integration

    /**
     * Call when app enters background to schedule background tasks.
     *
     * Uses UIApplication.beginBackgroundTask to get a ~30s grace period
     * for the supervisor to finish any in-flight work before the app
     * is fully suspended. Then schedules BGProcessingTasks for future work.
     */
    func appDidEnterBackground() {
        DebugLogger.shared.log("📱 App entered background, scheduling tasks")

        // Switch supervisor to background mode
        PhpSupervisor.shared.switchToBackground()

        // Begin a short background task for graceful transition.
        // This gives us ~30s to drain in-flight jobs before suspension.
        backgroundTaskID = UIApplication.shared.beginBackgroundTask(withName: "NativePHP.Transition") {
            // Expiration: stop supervisor and end task
            DebugLogger.shared.log("⏰ Background transition task expiring")
            PhpSupervisor.shared.stop()
            if self.backgroundTaskID != .invalid {
                UIApplication.shared.endBackgroundTask(self.backgroundTaskID)
                self.backgroundTaskID = .invalid
            }
        }

        // Schedule BGProcessingTasks for future background work
        scheduleAllTasks()

        // Give in-flight jobs a few seconds to complete, then end transition
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 10.0) {
            if self.backgroundTaskID != .invalid {
                DebugLogger.shared.log("📱 Background transition complete")
                UIApplication.shared.endBackgroundTask(self.backgroundTaskID)
                self.backgroundTaskID = .invalid
            }
        }
    }

    /**
     * Call when app enters foreground to start foreground workers.
     *
     * Starts the supervisor in unbounded foreground mode with push-based
     * wake for <500ms dispatch latency. Cancels any scheduled BGProcessingTasks.
     */
    func appWillEnterForeground() {
        DebugLogger.shared.log("📱 App entering foreground")

        // End any pending background transition task
        if backgroundTaskID != .invalid {
            UIApplication.shared.endBackgroundTask(backgroundTaskID)
            backgroundTaskID = .invalid
        }

        // Cancel scheduled background tasks — foreground work takes over
        BGTaskScheduler.shared.cancel(taskRequestWithIdentifier: Self.queueTaskIdentifier)
        BGTaskScheduler.shared.cancel(taskRequestWithIdentifier: Self.schedulerTaskIdentifier)

        // Start or switch supervisor to foreground mode
        let supervisor = PhpSupervisor.shared
        if supervisor.isRunning {
            supervisor.switchToForeground()
        } else {
            supervisor.workerCount = self.workerCount
            supervisor.queues = self.queues
            supervisor.connection = self.connection
            supervisor.mode = 0 // SUPERVISOR_MODE_ALL in foreground
            let _ = supervisor.start(
                appBasePath: self.appBasePath,
                iniPath: self.iniPath,
                foreground: true
            )
        }
    }
}
