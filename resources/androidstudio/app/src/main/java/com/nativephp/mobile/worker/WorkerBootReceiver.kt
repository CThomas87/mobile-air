package com.nativephp.mobile.worker

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * WorkerBootReceiver — Restarts the PhpWorkerService after device reboot.
 *
 * Requires RECEIVE_BOOT_COMPLETED permission in manifest.
 * Reads saved configuration from SharedPreferences to restart the service
 * with the same parameters it had before the reboot.
 */
class WorkerBootReceiver : BroadcastReceiver() {

    companion object {
        private const val TAG = "WorkerBootReceiver"
        private const val PREFS_NAME = "nativephp_worker_prefs"
        private const val KEY_ENABLED = "worker_enabled"
        private const val KEY_APP_BASE_PATH = "app_base_path"
        private const val KEY_INI_PATH = "ini_path"
        private const val KEY_WORKER_COUNT = "worker_count"
        private const val KEY_QUEUES = "queues"
        private const val KEY_CONNECTION = "connection"
        private const val KEY_MODE = "mode"

        /**
         * Save the worker configuration so it can be restored after reboot.
         */
        fun saveConfig(
            context: Context,
            appBasePath: String,
            iniPath: String = "",
            workerCount: Int = 2,
            queues: String = "default",
            connection: String = "database",
            mode: Int = 0
        ) {
            context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit().apply {
                putBoolean(KEY_ENABLED, true)
                putString(KEY_APP_BASE_PATH, appBasePath)
                putString(KEY_INI_PATH, iniPath)
                putInt(KEY_WORKER_COUNT, workerCount)
                putString(KEY_QUEUES, queues)
                putString(KEY_CONNECTION, connection)
                putInt(KEY_MODE, mode)
                apply()
            }
        }

        /**
         * Clear saved configuration (disables auto-restart on boot).
         */
        fun clearConfig(context: Context) {
            context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit().apply {
                putBoolean(KEY_ENABLED, false)
                apply()
            }
        }
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return

        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        if (!prefs.getBoolean(KEY_ENABLED, false)) {
            return
        }

        val appBasePath = prefs.getString(KEY_APP_BASE_PATH, "") ?: ""
        if (appBasePath.isEmpty()) {
            Log.e(TAG, "No app base path saved, cannot restart worker after boot")
            return
        }

        PhpWorkerService.start(
            context = context,
            appBasePath = appBasePath,
            iniPath = prefs.getString(KEY_INI_PATH, "") ?: "",
            workerCount = prefs.getInt(KEY_WORKER_COUNT, 2),
            queues = prefs.getString(KEY_QUEUES, "default") ?: "default",
            connection = prefs.getString(KEY_CONNECTION, "database") ?: "database",
            mode = prefs.getInt(KEY_MODE, 0)
        )
    }
}
