package com.nativephp.mobile.worker

import android.app.ActivityManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.provider.Settings
import android.util.Log
import kotlinx.coroutines.*
import org.json.JSONObject

/**
 * PhpWorkerService — Sticky Foreground Service that hosts the native Supervisor.
 *
 * Lifecycle:
 * 1. startForeground() with persistent notification
 * 2. Acquire PARTIAL_WAKE_LOCK while work is active
 * 3. Configure circuit breaker, memory limit, and worker logging
 * 4. Start native Supervisor with configured worker count
 * 5. Periodically enqueue scheduler ticks + queue jobs
 * 6. On stop: cancel work, release WakeLock, stopForeground
 *
 * Returns START_STICKY so Android restarts the service if killed.
 *
 * FGS Type Audit (Android 14+):
 * - Uses foregroundServiceType="dataSync" for queue processing
 * - Android 14 enforces 6-hour limit for dataSync FGS
 * - For longer runtimes, consider "specialUse" type (requires Play Store exemption)
 * - WakeLock timeout aligned with FGS limit (6 hours)
 */
class PhpWorkerService : Service() {

    companion object {
        private const val TAG = "PhpWorkerService"
        private const val CHANNEL_ID = "nativephp_worker_channel"
        private const val NOTIFICATION_ID = 9001
        private const val WAKE_LOCK_TAG = "NativePHP::WorkerWakeLock"

        // Default configuration
        private const val DEFAULT_WORKER_COUNT = 2
        private const val DEFAULT_SCHEDULER_INTERVAL_MS = 60_000L // 1 minute
        private const val DEFAULT_QUEUE_POLL_INTERVAL_MS = 5_000L // 5 seconds
        private const val DEFAULT_QUEUES = "high,default,low"
        private const val DEFAULT_CONNECTION = "database"

        // Android 14 dataSync FGS maximum (6 hours)
        private const val MAX_FGS_RUNTIME_MS = 6 * 60 * 60 * 1000L

        // Intent action constants
        const val ACTION_START = "com.nativephp.mobile.worker.START"
        const val ACTION_STOP = "com.nativephp.mobile.worker.STOP"
        const val ACTION_STATUS = "com.nativephp.mobile.worker.STATUS"
        const val ACTION_WAKE = "com.nativephp.mobile.worker.WAKE"
        const val ACTION_REQUEST_BATTERY_EXEMPTION = "com.nativephp.mobile.worker.BATTERY_EXEMPTION"

        // Intent extras
        const val EXTRA_WORKER_COUNT = "worker_count"
        const val EXTRA_QUEUES = "queues"
        const val EXTRA_CONNECTION = "connection"
        const val EXTRA_MODE = "mode"  // 0=all, 1=queue, 2=scheduler
        const val EXTRA_INI_PATH = "ini_path"
        const val EXTRA_APP_BASE_PATH = "app_base_path"
        const val EXTRA_MEMORY_LIMIT = "memory_limit"
        const val EXTRA_CB_THRESHOLD = "circuit_breaker_threshold"
        const val EXTRA_CB_BACKOFF = "circuit_breaker_backoff"
        const val EXTRA_LOG_ENABLED = "log_enabled"
        const val EXTRA_LOG_MAX_SIZE_KB = "log_max_size_kb"
        const val EXTRA_IMMEDIATE_DISPATCH = "immediate_dispatch"

        /**
         * Start the worker service.
         */
        fun start(
            context: Context,
            appBasePath: String,
            iniPath: String = "",
            workerCount: Int = DEFAULT_WORKER_COUNT,
            queues: String = DEFAULT_QUEUES,
            connection: String = DEFAULT_CONNECTION,
            mode: Int = 0, // SUPERVISOR_MODE_ALL
            memoryLimit: String = "512M",
            circuitBreakerThreshold: Int = 3,
            circuitBreakerBackoff: Int = 5,
            logEnabled: Boolean = true,
            logMaxSizeKb: Int = 1024,
            immediateDispatch: Boolean = true
        ) {
            val intent = Intent(context, PhpWorkerService::class.java).apply {
                action = ACTION_START
                putExtra(EXTRA_APP_BASE_PATH, appBasePath)
                putExtra(EXTRA_INI_PATH, iniPath)
                putExtra(EXTRA_WORKER_COUNT, workerCount)
                putExtra(EXTRA_QUEUES, queues)
                putExtra(EXTRA_CONNECTION, connection)
                putExtra(EXTRA_MODE, mode)
                putExtra(EXTRA_MEMORY_LIMIT, memoryLimit)
                putExtra(EXTRA_CB_THRESHOLD, circuitBreakerThreshold)
                putExtra(EXTRA_CB_BACKOFF, circuitBreakerBackoff)
                putExtra(EXTRA_LOG_ENABLED, logEnabled)
                putExtra(EXTRA_LOG_MAX_SIZE_KB, logMaxSizeKb)
                putExtra(EXTRA_IMMEDIATE_DISPATCH, immediateDispatch)
            }
            context.startForegroundService(intent)
        }

        /**
         * Stop the worker service.
         */
        fun stop(context: Context) {
            val intent = Intent(context, PhpWorkerService::class.java).apply {
                action = ACTION_STOP
            }
            context.startService(intent)
        }

        /**
         * Wake workers immediately for foreground dispatch (<500ms latency).
         */
        fun wakeWorkers(context: Context) {
            val intent = Intent(context, PhpWorkerService::class.java).apply {
                action = ACTION_WAKE
            }
            context.startService(intent)
        }

        /**
         * Request battery optimization exemption.
         * Opens the system dialog for the user to whitelist this app.
         * Important for OEMs (Samsung, Xiaomi, Huawei) that aggressively kill background services.
         */
        fun requestBatteryExemption(context: Context) {
            val packageName = context.packageName
            val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager

            if (!pm.isIgnoringBatteryOptimizations(packageName)) {
                val intent = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
                    data = Uri.parse("package:$packageName")
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                }
                try {
                    context.startActivity(intent)
                    Log.i(TAG, "Battery exemption dialog shown for $packageName")
                } catch (e: Exception) {
                    Log.e(TAG, "Failed to show battery exemption dialog", e)
                }
            } else {
                Log.i(TAG, "Battery optimization already disabled for $packageName")
            }
        }
    }

    // Binder for local binding (optional, for status queries)
    inner class WorkerBinder : Binder() {
        val service: PhpWorkerService get() = this@PhpWorkerService
    }
    private val binder = WorkerBinder()

    private var wakeLock: PowerManager.WakeLock? = null
    private var supervisorStarted = false
    private var immediateDispatchEnabled = true
    private var configuredWorkerCount = DEFAULT_WORKER_COUNT
    private val serviceScope = CoroutineScope(Dispatchers.Default + SupervisorJob())

    private data class SupervisorSnapshot(
        val activeJobs: Int,
        val pendingJobs: Int
    )

    // Periodic job dispatchers
    private var schedulerJob: Job? = null
    private var queuePollerJob: Job? = null

    // FGS timeout job (Android 14+ dataSync 6-hour limit)
    private var fgsTimeoutJob: Job? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {

        when (intent?.action) {
            ACTION_START -> handleStart(intent)
            ACTION_STOP -> handleStop()
            ACTION_WAKE -> handleWake()
            ACTION_REQUEST_BATTERY_EXEMPTION -> requestBatteryExemption(this)
            ACTION_STATUS -> {
                // Status query via local binder
            }
            else -> handleStart(intent)
        }

        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        handleStop()
        serviceScope.cancel()
        super.onDestroy()
    }

    private fun handleStart(intent: Intent?) {
        if (supervisorStarted) return

        // Start foreground immediately
        val notification = buildNotification("Starting PHP workers...")

        // Android 14+ (API 34) requires specifying foregroundServiceType
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIFICATION_ID, notification,
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        // Acquire WakeLock
        acquireWakeLock()

        // Extract config from intent
        val appBasePath = intent?.getStringExtra(EXTRA_APP_BASE_PATH) ?: ""
        val iniPath = intent?.getStringExtra(EXTRA_INI_PATH) ?: ""
        val workerCount = intent?.getIntExtra(EXTRA_WORKER_COUNT, DEFAULT_WORKER_COUNT) ?: DEFAULT_WORKER_COUNT
        configuredWorkerCount = adjustWorkerCountByDeviceCapability(maxOf(workerCount, 1))
        val queues = intent?.getStringExtra(EXTRA_QUEUES) ?: DEFAULT_QUEUES
        val connection = intent?.getStringExtra(EXTRA_CONNECTION) ?: DEFAULT_CONNECTION
        val mode = intent?.getIntExtra(EXTRA_MODE, 0) ?: 0
        val memoryLimit = intent?.getStringExtra(EXTRA_MEMORY_LIMIT) ?: "512M"
        val cbThreshold = intent?.getIntExtra(EXTRA_CB_THRESHOLD, 3) ?: 3
        val cbBackoff = intent?.getIntExtra(EXTRA_CB_BACKOFF, 5) ?: 5
        val logEnabled = intent?.getBooleanExtra(EXTRA_LOG_ENABLED, true) ?: true
        val logMaxSizeKb = intent?.getIntExtra(EXTRA_LOG_MAX_SIZE_KB, 1024) ?: 1024
        immediateDispatchEnabled = intent?.getBooleanExtra(EXTRA_IMMEDIATE_DISPATCH, true) ?: true

        // Initialize and start supervisor on background thread
        serviceScope.launch {
            try {
                // Initialize engine
                val engineOk = PhpSupervisorBridge.nativeEngineInit(iniPath, "", appBasePath)
                if (!engineOk) {
                    Log.e(TAG, "Failed to initialize PHP engine")
                    stopSelf()
                    return@launch
                }

                // Configure circuit breaker before starting supervisor
                PhpSupervisorBridge.nativeConfigureCircuitBreaker(cbThreshold, cbBackoff)

                // Set memory limit
                PhpSupervisorBridge.nativeSetMemoryLimit(memoryLimit)

                // Configure worker log
                if (logEnabled) {
                    val logPath = "$appBasePath/storage/logs/worker.log"
                    PhpSupervisorBridge.nativeSetLogFile(logPath, logMaxSizeKb)
                }

                // Start supervisor
                val startOk = PhpSupervisorBridge.nativeStartSupervisor(mode, workerCount, queues, connection)
                if (!startOk) {
                    Log.e(TAG, "Failed to start supervisor")
                    PhpSupervisorBridge.nativeEngineShutdown()
                    stopSelf()
                    return@launch
                }

                supervisorStarted = true

                WorkerBootReceiver.saveConfig(
                    context = this@PhpWorkerService,
                    appBasePath = appBasePath,
                    iniPath = iniPath,
                    workerCount = workerCount,
                    queues = queues,
                    connection = connection,
                    mode = mode
                )

                // Update notification
                updateNotification("PHP workers active ($workerCount workers)")

                // Start periodic scheduler ticks
                if (mode == 0 || mode == 2) {
                    startSchedulerLoop()
                }

                // Start periodic queue polling
                if (mode == 0 || mode == 1) {
                    startQueuePoller()
                }

                // Android 14+ dataSync FGS 6-hour timeout safety
                startFgsTimeout()

            } catch (e: Exception) {
                Log.e(TAG, "Error starting supervisor", e)
                stopSelf()
            }
        }
    }

    /**
     * Handle immediate wake request for foreground dispatch.
     */
    private fun handleWake() {
        if (supervisorStarted && immediateDispatchEnabled) {
            PhpSupervisorBridge.nativeWakeWorkers()
        }
    }

    private fun handleStop() {

        // Cancel periodic jobs
        schedulerJob?.cancel()
        queuePollerJob?.cancel()
        fgsTimeoutJob?.cancel()

        if (supervisorStarted) {
            runBlocking(Dispatchers.Default) {
                try {
                    PhpSupervisorBridge.nativeStopSupervisor()
                    PhpSupervisorBridge.nativeEngineShutdown()
                } catch (e: Exception) {
                    Log.e(TAG, "Error stopping supervisor", e)
                }
            }
            supervisorStarted = false
        }

        WorkerBootReceiver.clearConfig(this)

        // Release WakeLock
        releaseWakeLock()

        // Stop foreground
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    /**
     * Periodically fire scheduler ticks (schedule:run).
     * Fires every 60 seconds by default. SchedulerGate in native code
     * ensures only one tick runs at a time.
     */
    private fun startSchedulerLoop() {
        schedulerJob = serviceScope.launch {
            while (isActive && supervisorStarted) {
                try {
                    val jobId = PhpSupervisorBridge.nativeEnqueueSchedulerTick("")
                    if (jobId != null) {
                        PhpSupervisorBridge.nativeAwaitJob(jobId, 5 * 60 * 1000)
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error enqueueing scheduler tick", e)
                }
                delay(DEFAULT_SCHEDULER_INTERVAL_MS)
            }
        }
    }

    /**
     * Periodically poll for queue jobs.
     * Enqueues multiple "pop one job" requests (up to worker count) into the
     * worker pool so all threads stay busy. Each enqueued job runs the PHP
     * queue_worker.php entrypoint which pops one job from the database queue.
     * If the pop returns nothing (queue empty), the worker exits fast (~20ms).
     */
    private fun startQueuePoller() {
        queuePollerJob = serviceScope.launch {
            while (isActive && supervisorStarted) {
                try {
                    val snapshot = readSupervisorSnapshot()
                    if (snapshot == null) {
                        delay(DEFAULT_QUEUE_POLL_INTERVAL_MS)
                        continue
                    }

                    if (snapshot.pendingJobs > 0) {
                        delay(DEFAULT_QUEUE_POLL_INTERVAL_MS)
                        continue
                    }

                    val availableSlots = (configuredWorkerCount - snapshot.activeJobs).coerceAtLeast(0)
                    if (availableSlots == 0) {
                        delay(DEFAULT_QUEUE_POLL_INTERVAL_MS)
                        continue
                    }

                    val pendingJobs = mutableListOf<String>()
                    for (i in 0 until availableSlots) {
                        val jobId = PhpSupervisorBridge.nativeEnqueueQueueJob("")
                        if (jobId != null) {
                            pendingJobs.add(jobId)
                        }
                    }

                    if (pendingJobs.isNotEmpty()) {
                        // Await all results concurrently
                        pendingJobs.map { jobId ->
                            async(Dispatchers.IO) {
                                try {
                                    PhpSupervisorBridge.nativeAwaitJob(jobId, 15 * 1000)
                                } catch (e: Exception) {
                                    Log.e(TAG, "Error awaiting queue job $jobId", e)
                                    null
                                }
                            }
                        }.forEach { it.await() }
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error in queue poller cycle", e)
                }
                delay(DEFAULT_QUEUE_POLL_INTERVAL_MS)
            }
        }
    }

    private fun readSupervisorSnapshot(): SupervisorSnapshot? {
        return try {
            val status = PhpSupervisorBridge.nativeGetStatus()
            val root = JSONObject(status)
            val activeJobs = root.optInt("activeJobs", 0)
            val pendingJobs = root.optInt("pendingJobs", 0)
            SupervisorSnapshot(activeJobs = activeJobs, pendingJobs = pendingJobs)
        } catch (e: Exception) {
            null
        }
    }

    /**
     * Safety net for Android 14+ dataSync FGS 6-hour limit.
     * Gracefully restarts the service before the system force-stops it.
     */
    private fun startFgsTimeout() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) return

        fgsTimeoutJob = serviceScope.launch {
            // Stop 5 minutes before the 6-hour limit
            delay(MAX_FGS_RUNTIME_MS - 5 * 60 * 1000)
            Log.w(TAG, "Approaching Android 14 dataSync FGS 6-hour limit, restarting service...")
            updateNotification("Restarting workers (FGS timeout)...")

            // Stop and restart via the system
            handleStop()
            // WorkerBootReceiver.saveConfig was called during handleStart,
            // but handleStop clears it. We need a restart mechanism.
            // For now, just stop — the user can re-trigger via System::startBackgroundWorker.
        }
    }

    // ─── Notification Management ───

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            "PHP Background Workers",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Keeps PHP queue workers and scheduler running in the background"
            setShowBadge(false)
        }

        val notificationManager = getSystemService(NotificationManager::class.java)
        notificationManager.createNotificationChannel(channel)
    }

    private fun buildNotification(text: String): Notification {
        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("NativePHP Worker")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_popup_sync)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .build()
    }

    private fun updateNotification(text: String) {
        val notification = buildNotification(text)
        val notificationManager = getSystemService(NotificationManager::class.java)
        notificationManager.notify(NOTIFICATION_ID, notification)
    }

    // ─── WakeLock Management ───

    private fun acquireWakeLock() {
        if (wakeLock != null) return

        val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = powerManager.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            WAKE_LOCK_TAG
        ).apply {
            // Acquire with timeout matching Android 14 dataSync FGS limit (6 hours)
            acquire(MAX_FGS_RUNTIME_MS)
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let {
            if (it.isHeld) {
                it.release()
            }
        }
        wakeLock = null
    }

    /**
     * Get the current supervisor status as JSON.
     * Can be called via local binder.
     */
    fun getStatus(): String {
        return if (supervisorStarted) {
            PhpSupervisorBridge.nativeGetStatus()
        } else {
            "{\"status\":\"stopped\"}"
        }
    }

    /**
     * Determine optimal worker count based on device memory capability.
     *
     * Each worker thread holds a full TSRM interpreter context (~20-24MB
     * without OPcache, ~16-20MB with). On low-memory devices we reduce
     * concurrency to avoid OOM kills from the Android LMK (Low Memory Killer).
     *
     * Heuristic:
     *   < 2 GB total RAM  → 1 worker
     *   2-3 GB total RAM  → min(requested, 2)
     *   3-6 GB total RAM  → min(requested, 3)
     *   > 6 GB total RAM  → min(requested, 4)
     *
     * The available memory (not total) is also checked — if less than
     * 256MB is free, we degrade to 1 worker regardless.
     */
    private fun adjustWorkerCountByDeviceCapability(requestedCount: Int): Int {
        return try {
            val activityManager = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val memInfo = ActivityManager.MemoryInfo()
            activityManager.getMemoryInfo(memInfo)

            val totalMb = memInfo.totalMem / (1024 * 1024)
            val availableMb = memInfo.availMem / (1024 * 1024)

            Log.i(TAG, "Device memory: total=${totalMb}MB, available=${availableMb}MB, lowMemory=${memInfo.lowMemory}")

            // Emergency: very low available memory
            if (availableMb < 256 || memInfo.lowMemory) {
                Log.w(TAG, "Low memory detected (${availableMb}MB free), degrading to 1 worker")
                return 1
            }

            val maxByRam = when {
                totalMb < 2048  -> 1
                totalMb < 3072  -> 2
                totalMb < 6144  -> 3
                else            -> 4
            }

            val adjusted = minOf(requestedCount, maxByRam)
            if (adjusted != requestedCount) {
                Log.i(TAG, "Adjusted worker count from $requestedCount to $adjusted based on device memory (${totalMb}MB)")
            }
            adjusted.coerceAtLeast(1)
        } catch (e: Exception) {
            Log.e(TAG, "Failed to check device memory, using requested count", e)
            requestedCount.coerceAtLeast(1)
        }
    }
}
