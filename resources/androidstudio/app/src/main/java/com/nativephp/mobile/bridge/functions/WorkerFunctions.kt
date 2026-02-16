package com.nativephp.mobile.bridge.functions

import android.content.Context
import android.util.Log
import com.nativephp.mobile.bridge.BridgeFunction
import com.nativephp.mobile.worker.PhpWorkerService
import com.nativephp.mobile.worker.PhpSupervisorBridge

/**
 * WorkerFunctions — Bridge functions for controlling the background
 * worker service from PHP via nativephp_call().
 *
 * Registered names:
 *   Worker.Start   — Start the foreground worker service
 *   Worker.Stop    — Stop the worker service gracefully
 *   Worker.Status  — Query supervisor status (JSON)
 */
object WorkerFunctions {

    private const val TAG = "WorkerFunctions"

    /**
     * Worker.Start — Start the PhpWorkerService foreground service.
     *
     * Expects JSON parameters:
     *   workerCount (int), queues (string), connection (string),
     *   mode (int), memoryLimit (string), circuitBreakerThreshold (int),
     *   circuitBreakerBackoff (int), immediateDispatch (bool)
     */
    class Start(private val context: Context) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {

            val appStorageDir = context.getDir("storage", Context.MODE_PRIVATE)
            val appBasePath = java.io.File(appStorageDir, "laravel").absolutePath

            val workerCount = (parameters["workerCount"] as? Number)?.toInt() ?: 2
            val queues = parameters["queues"] as? String ?: "default"
            val connection = parameters["connection"] as? String ?: "database"
            val mode = (parameters["mode"] as? Number)?.toInt() ?: 0
            val memoryLimit = parameters["memoryLimit"] as? String ?: "512M"
            val cbThreshold = (parameters["circuitBreakerThreshold"] as? Number)?.toInt() ?: 3
            val cbBackoff = (parameters["circuitBreakerBackoff"] as? Number)?.toInt() ?: 5
            val immediateDispatch = parameters["immediateDispatch"] as? Boolean ?: true

            try {
                PhpWorkerService.start(
                    context = context,
                    appBasePath = appBasePath,
                    workerCount = workerCount,
                    queues = queues,
                    connection = connection,
                    mode = mode,
                    memoryLimit = memoryLimit,
                    circuitBreakerThreshold = cbThreshold,
                    circuitBreakerBackoff = cbBackoff,
                    immediateDispatch = immediateDispatch
                )
                return mapOf("started" to true)
            } catch (e: Exception) {
                Log.e(TAG, "❌ Failed to start worker service: ${e.message}", e)
                return mapOf("started" to false, "error" to (e.message ?: "Unknown error"))
            }
        }
    }

    /**
     * Worker.Stop — Stop the PhpWorkerService foreground service.
     */
    class Stop(private val context: Context) : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            try {
                PhpWorkerService.stop(context)
                return mapOf("stopped" to true)
            } catch (e: Exception) {
                Log.e(TAG, "❌ Failed to stop worker service: ${e.message}", e)
                return mapOf("stopped" to false, "error" to (e.message ?: "Unknown error"))
            }
        }
    }

    /**
     * Worker.Status — Query the native supervisor status.
     *
     * Returns JSON with: status, activeJobs, pendingJobs,
     * completedJobs, failedJobs, schedulerRunning, uptimeSeconds, mode
     */
    class Status : BridgeFunction {
        override fun execute(parameters: Map<String, Any>): Map<String, Any> {
            return try {
                val statusJson = PhpSupervisorBridge.nativeGetStatus()

                // Parse the JSON string into a map
                val parsed = org.json.JSONObject(statusJson)
                val result = mutableMapOf<String, Any>()
                for (key in parsed.keys()) {
                    result[key] = parsed.get(key)
                }
                result
            } catch (e: Exception) {
                Log.e(TAG, "❌ Failed to get worker status: ${e.message}", e)
                mapOf(
                    "status" to "stopped",
                    "activeJobs" to 0,
                    "pendingJobs" to 0,
                    "completedJobs" to 0,
                    "failedJobs" to 0,
                    "schedulerRunning" to false,
                    "uptimeSeconds" to 0,
                    "mode" to 0
                )
            }
        }
    }
}
