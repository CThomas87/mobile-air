package com.nativephp.mobile.worker

import android.content.Context
import android.os.Build
import android.util.Log
import androidx.work.*
import kotlinx.coroutines.*
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/**
 * PhpPeriodicWorker — WorkManager-based alternative to PhpWorkerService FGS.
 *
 * Android 14+ enforces a 6-hour limit on `dataSync` foreground services.
 * This WorkManager implementation provides an alternative execution model
 * that chains periodic work requests to keep workers alive indefinitely
 * without the FGS time limit.
 *
 * Trade-offs vs FGS:
 * - WorkManager respects Doze mode; work may be deferred when screen is off
 * - Minimum periodic interval is 15 minutes (PeriodicWorkRequest constraint)
 * - Better for apps that don't need sub-second dispatch latency
 * - No persistent notification required (though one can be added via setForeground)
 *
 * Usage:
 *   PhpPeriodicWorker.enqueue(context, appBasePath, workerCount = 2)
 *   PhpPeriodicWorker.cancel(context)
 *
 * Each execution cycle:
 *   1. Init PHP engine (if not already running)
 *   2. Start supervisor
 *   3. Run queue jobs + scheduler tick for the work duration (~10 minutes)
 *   4. Gracefully stop supervisor
 *   5. WorkManager re-schedules automatically
 */
class PhpPeriodicWorker(
    appContext: Context,
    params: WorkerParameters
) : CoroutineWorker(appContext, params) {

    companion object {
        private const val TAG = "PhpPeriodicWorker"
        private const val UNIQUE_WORK_NAME = "nativephp_periodic_worker"

        // Keys for input data
        private const val KEY_APP_BASE_PATH = "app_base_path"
        private const val KEY_INI_PATH = "ini_path"
        private const val KEY_WORKER_COUNT = "worker_count"
        private const val KEY_QUEUES = "queues"
        private const val KEY_CONNECTION = "connection"
        private const val KEY_MODE = "mode"
        private const val KEY_MEMORY_LIMIT = "memory_limit"
        private const val KEY_WORK_DURATION_MS = "work_duration_ms"

        // Default work duration: 14 minutes (leaves 1 minute buffer in 15-min cycle)
        private const val DEFAULT_WORK_DURATION_MS = 14L * 60 * 1000

        // Minimum periodic interval (WorkManager constraint)
        private const val PERIODIC_INTERVAL_MINUTES = 15L

        /**
         * Enqueue periodic worker execution.
         * Replaces any existing enqueued work with updated configuration.
         */
        fun enqueue(
            context: Context,
            appBasePath: String,
            iniPath: String = "",
            workerCount: Int = 2,
            queues: String = "high,default,low",
            connection: String = "database",
            mode: Int = 0,
            memoryLimit: String = "512M",
            workDurationMs: Long = DEFAULT_WORK_DURATION_MS
        ) {
            val inputData = Data.Builder()
                .putString(KEY_APP_BASE_PATH, appBasePath)
                .putString(KEY_INI_PATH, iniPath)
                .putInt(KEY_WORKER_COUNT, workerCount)
                .putString(KEY_QUEUES, queues)
                .putString(KEY_CONNECTION, connection)
                .putInt(KEY_MODE, mode)
                .putString(KEY_MEMORY_LIMIT, memoryLimit)
                .putLong(KEY_WORK_DURATION_MS, workDurationMs)
                .build()

            val constraints = Constraints.Builder()
                .setRequiresBatteryNotLow(false) // Allow on low battery
                .build()

            val workRequest = PeriodicWorkRequestBuilder<PhpPeriodicWorker>(
                PERIODIC_INTERVAL_MINUTES, TimeUnit.MINUTES
            )
                .setInputData(inputData)
                .setConstraints(constraints)
                .addTag(UNIQUE_WORK_NAME)
                .build()

            WorkManager.getInstance(context).enqueueUniquePeriodicWork(
                UNIQUE_WORK_NAME,
                ExistingPeriodicWorkPolicy.UPDATE,
                workRequest
            )

            Log.i(TAG, "Periodic worker enqueued: interval=${PERIODIC_INTERVAL_MINUTES}min, " +
                    "duration=${workDurationMs}ms, workers=$workerCount")
        }

        /**
         * Cancel all periodic worker execution.
         */
        fun cancel(context: Context) {
            WorkManager.getInstance(context).cancelUniqueWork(UNIQUE_WORK_NAME)
            Log.i(TAG, "Periodic worker cancelled")
        }

        /**
         * Check if periodic worker is currently enqueued or running.
         */
        suspend fun isRunning(context: Context): Boolean {
            val workInfos = withContext(Dispatchers.IO) {
                WorkManager.getInstance(context)
                    .getWorkInfosForUniqueWork(UNIQUE_WORK_NAME)
                    .get()
            }
            return workInfos.any { info -> info.state == WorkInfo.State.RUNNING || info.state == WorkInfo.State.ENQUEUED }
        }
    }

    override suspend fun doWork(): Result {
        val appBasePath = inputData.getString(KEY_APP_BASE_PATH) ?: return Result.failure()
        val iniPath = inputData.getString(KEY_INI_PATH) ?: ""
        val workerCount = inputData.getInt(KEY_WORKER_COUNT, 2)
        val queues = inputData.getString(KEY_QUEUES) ?: "high,default,low"
        val connection = inputData.getString(KEY_CONNECTION) ?: "database"
        val mode = inputData.getInt(KEY_MODE, 0)
        val memoryLimit = inputData.getString(KEY_MEMORY_LIMIT) ?: "512M"
        val workDurationMs = inputData.getLong(KEY_WORK_DURATION_MS, DEFAULT_WORK_DURATION_MS)

        Log.i(TAG, "Work cycle starting: workers=$workerCount, duration=${workDurationMs}ms")

        // Promote to foreground with notification (Android 12+ requirement for long work)
        try {
            setForeground(createForegroundInfo())
        } catch (e: Exception) {
            Log.w(TAG, "Could not promote to foreground (may be fine on older Android)", e)
        }

        return try {
            // Initialize engine
            val engineOk = PhpSupervisorBridge.nativeEngineInit(iniPath, "", appBasePath)
            if (!engineOk) {
                Log.e(TAG, "Failed to initialize PHP engine")
                return Result.retry()
            }

            // Configure
            PhpSupervisorBridge.nativeConfigureCircuitBreaker(3, 5)
            PhpSupervisorBridge.nativeSetMemoryLimit(memoryLimit)

            val logPath = "$appBasePath/storage/logs/worker.log"
            PhpSupervisorBridge.nativeSetLogFile(logPath, 1024)

            // Start supervisor
            val startOk = PhpSupervisorBridge.nativeStartSupervisor(mode, workerCount, queues, connection)
            if (!startOk) {
                Log.e(TAG, "Failed to start supervisor")
                PhpSupervisorBridge.nativeEngineShutdown()
                return Result.retry()
            }

            // Run work cycle — process jobs for the configured duration
            val deadline = System.currentTimeMillis() + workDurationMs
            val pollIntervalMs = 5_000L
            val schedulerIntervalMs = 60_000L
            var lastSchedulerTick = 0L

            while (System.currentTimeMillis() < deadline && !isStopped) {
                // Enqueue scheduler tick if due
                val now = System.currentTimeMillis()
                if ((mode == 0 || mode == 2) && now - lastSchedulerTick >= schedulerIntervalMs) {
                    val schedJobId = PhpSupervisorBridge.nativeEnqueueSchedulerTick("")
                    if (schedJobId != null) {
                        PhpSupervisorBridge.nativeAwaitJob(schedJobId, 5 * 60 * 1000)
                    }
                    lastSchedulerTick = System.currentTimeMillis()
                }

                // Enqueue queue jobs
                if (mode == 0 || mode == 1) {
                    val statusJson = PhpSupervisorBridge.nativeGetStatus()
                    val status = JSONObject(statusJson)
                    val pending = status.optInt("pendingJobs", 0)
                    val active = status.optInt("activeJobs", 0)

                    if (pending == 0) {
                        val slots = (workerCount - active).coerceAtLeast(0)
                        val jobIds = mutableListOf<String>()
                        for (i in 0 until slots) {
                            val jobId = PhpSupervisorBridge.nativeEnqueueQueueJob("")
                            if (jobId != null) jobIds.add(jobId)
                        }
                        // Await all
                        for (jobId in jobIds) {
                            PhpSupervisorBridge.nativeAwaitJob(jobId, 15_000)
                        }
                    }
                }

                delay(pollIntervalMs)
            }

            // Graceful shutdown
            PhpSupervisorBridge.nativeStopSupervisor()
            PhpSupervisorBridge.nativeEngineShutdown()

            Log.i(TAG, "Work cycle completed successfully")
            Result.success()

        } catch (e: Exception) {
            Log.e(TAG, "Work cycle failed", e)
            try {
                PhpSupervisorBridge.nativeStopSupervisor()
                PhpSupervisorBridge.nativeEngineShutdown()
            } catch (cleanup: Exception) {
                Log.e(TAG, "Cleanup error", cleanup)
            }
            Result.retry()
        }
    }

    private fun createForegroundInfo(): ForegroundInfo {
        val channelId = "nativephp_worker_channel"

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = android.app.NotificationChannel(
                channelId,
                "PHP Background Workers",
                android.app.NotificationManager.IMPORTANCE_LOW
            )
            val nm = applicationContext.getSystemService(Context.NOTIFICATION_SERVICE) as android.app.NotificationManager
            nm.createNotificationChannel(channel)
        }

        val notification = android.app.Notification.Builder(applicationContext, channelId)
            .setContentTitle("NativePHP Worker")
            .setContentText("Processing background jobs...")
            .setSmallIcon(android.R.drawable.ic_popup_sync)
            .setOngoing(true)
            .build()

        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            ForegroundInfo(
                9002,
                notification,
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
            )
        } else {
            ForegroundInfo(9002, notification)
        }
    }
}
