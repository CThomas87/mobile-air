@file:Suppress("DEPRECATION")

package com.nativephp.mobile.bridge

import android.content.Context
import android.util.Log
import android.webkit.CookieManager
import org.json.JSONObject
import java.util.concurrent.ConcurrentHashMap
import com.nativephp.mobile.network.PHPRequest
import com.nativephp.mobile.security.LaravelCookieStore

class PHPBridge(private val context: Context) {
    private var lastPostData: String? = null
    private val requestDataMap = ConcurrentHashMap<String, String>()

    /**
     * Fixed thread pool for parallel HTTP request execution.
     * PHP ZTS (Zend Thread Safety) enables safe concurrent PHP execution.
     * 2 lanes lets the browser fire parallel requests (e.g. /workers/status +
     * /workers/activities) without serialization waits.
     */
    private val uiPhpExecutor = java.util.concurrent.Executors.newFixedThreadPool(2) { runnable ->
        Thread(runnable, "NativePHP-UI-Lane").apply {
            priority = Thread.NORM_PRIORITY
            isDaemon = false
        }
    }

    private val nativePhpScript: String
        get() = "${getLaravelPath()}/vendor/nativephp/mobile/bootstrap/android/native.php"

    external fun nativeExecuteScript(filename: String): String
    external fun nativeSetEnv(name: String, value: String, overwrite: Int): Int
    external fun runArtisanCommand(command: String): String
    external fun initialize()
    external fun setRequestInfo(method: String, uri: String, postData: String?)
    external fun getLaravelPublicPath(): String
    external fun getLaravelRootPath(): String
    external fun shutdown()
    external fun nativeSetUiRequestActive(active: Boolean)
    external fun nativeHandleRequestOnce(
        method: String,
        uri: String,
        postData: String?,
        scriptPath: String,
        headers: String
    ): String


    companion object {
        private const val TAG = "PHPBridge"
        private const val MAX_REQUEST_AGE = 5 * 60 * 1000L
        private const val MAX_RAW_RESPONSE_CHARS = 2 * 1024 * 1024

        init {
            System.loadLibrary("compat") // JNI_OnLoad pre-loads libphp.so with RTLD_GLOBAL
            System.loadLibrary("php")
            System.loadLibrary("php_wrapper")
        }
    }

    fun handleLaravelRequest(request: PHPRequest): String {
        val requestStart = System.currentTimeMillis()

        /* Ensure bridge JNI reference is up to date (thread-safe via C mutex) */
        initialize()

        val future = uiPhpExecutor.submit<String> {
            val prepStart = System.currentTimeMillis()
            val laneThread = Thread.currentThread().name
            Log.d(TAG, "🧵 UI request lane thread: $laneThread uri=${request.uri}")

            /* ─── Build thread-local headers string ───
             * Format: "KEY\nVALUE\nKEY\nVALUE\n..."
             * Passed directly to the C layer via JNI parameter.
             * Avoids process-global setenv() which races between threads. */
            val headersStr = buildString {
                // Request headers (already HTTP_* formatted)
                request.headers.forEach { (key, value) ->
                    val envKey = "HTTP_${key.replace("-", "_").uppercase()}"
                    append(envKey).append('\n').append(value).append('\n')
                }
                // Cookies
                val cookieHeader = LaravelCookieStore.asCookieHeader()
                if (cookieHeader.isNotEmpty()) {
                    append("HTTP_COOKIE").append('\n').append(cookieHeader).append('\n')
                }
            }

            val prepTime = System.currentTimeMillis() - prepStart
            val jniStart = System.currentTimeMillis()

            nativeSetUiRequestActive(true)
            val output = try {
                nativeHandleRequestOnce(
                    request.method,
                    request.uri,
                    request.body,
                    nativePhpScript,
                    headersStr
                )
            } finally {
                nativeSetUiRequestActive(false)
            }

            val jniTime = System.currentTimeMillis() - jniStart
            val processStart = System.currentTimeMillis()

            val processedOutput = processRawPHPResponse(output)

            val processTime = System.currentTimeMillis() - processStart
            Log.d("PerfTiming", "⏱️ BRIDGE [${request.uri}] prep=${prepTime}ms jni=${jniTime}ms process=${processTime}ms")

            processedOutput
        }

        val result = future.get()
        val totalTime = System.currentTimeMillis() - requestStart
        Log.d("PerfTiming", "⏱️ BRIDGE_TOTAL [${request.uri}] ${totalTime}ms")
        return result
    }

    // New function to store request data with a key
    fun storeRequestData(key: String, data: String) {
        requestDataMap[key] = data
        Log.d(TAG, "🔑 Stored request data with key: $key (length=${data.length})")

        // Also update last post data for backward compatibility
        lastPostData = data

        // Clean up old requests occasionally
        if (requestDataMap.size > 10) {
            cleanupOldRequests()
        }
    }

    // Clean up old request data
    private fun cleanupOldRequests() {
        val now = System.currentTimeMillis()
        val keysToRemove = mutableListOf<String>()

        // Find keys with timestamps older than MAX_REQUEST_AGE
        requestDataMap.keys.forEach { key ->
            if (key.contains("-")) {
                val timestampStr = key.substringAfterLast("-")
                try {
                    val timestamp = timestampStr.toLong()
                    if (now - timestamp > MAX_REQUEST_AGE) {
                        keysToRemove.add(key)
                    }
                } catch (e: NumberFormatException) {
                    // Key doesn't have a valid timestamp format, ignore
                }
            }
        }

        // Remove old entries
        keysToRemove.forEach { requestDataMap.remove(it) }
        if (keysToRemove.isNotEmpty()) {
            Log.d(TAG, "🧹 Cleaned up ${keysToRemove.size} old request entries")
        }
    }

    fun getLastPostData(): String? {
        return lastPostData
    }

    fun getLaravelPath(): String {
        val storageDir = context.getDir("storage", Context.MODE_PRIVATE)
        return "${storageDir.absolutePath}/laravel"
    }

    fun processRawPHPResponse(response: String): String {
        val normalizedResponse = run {
            val statusIndex = response.indexOf("HTTP/")
            if (statusIndex > 0) {
                response.substring(statusIndex)
            } else {
                response
            }
        }

        val boundedResponse = if (normalizedResponse.length > MAX_RAW_RESPONSE_CHARS && !normalizedResponse.startsWith("HTTP/")) {
            "HTTP/1.1 500 Internal Server Error\r\n" +
                    "Content-Type: text/plain; charset=utf-8\r\n\r\n" +
                    "NativePHP response exceeded safe parser limit before HTTP headers."
        } else {
            normalizedResponse
        }

        // Log the first 200 characters to understand the response format
        Log.d(TAG, "🔍 Response first 200 chars: ${boundedResponse.take(200)}")

        // Check for Set-Cookie headers regardless of response format
        if (boundedResponse.contains("Set-Cookie:", ignoreCase = true)) {
            Log.d(TAG, "🍪 Found Set-Cookie in raw response!")

            // Extract all Set-Cookie lines
            val setCookieLines = boundedResponse.split("\r\n")
                .filter { it.startsWith("Set-Cookie:", ignoreCase = true) }

            setCookieLines.forEach { cookieLine ->
                Log.d(TAG, "🍪 Cookie line: $cookieLine")

                // Extract the cookie value (after "Set-Cookie:")
                val cookieValue = cookieLine.substringAfter(":", "").trim()
                if (cookieValue.isNotEmpty()) {
                    // Manually set this cookie
                    val cookieManager = CookieManager.getInstance()
                    cookieManager.setCookie("http://127.0.0.1", cookieValue)
                    Log.d(TAG, "🍪 Manually set cookie: $cookieValue")
                }
            }

            // Make sure to flush the cookies
            CookieManager.getInstance().flush()
            Log.d(TAG, "🍪 Flushed cookies after extraction")
        } else {
            Log.d(TAG, "⚠️ No Set-Cookie headers found in the response")
        }

        // Continue with your existing logic for different response types
        if (boundedResponse.trim().startsWith("{") && boundedResponse.trim().endsWith("}")) {
            try {
            val json = JSONObject(boundedResponse)
                if (json.has("message") && json.getString("message")
                        .contains("CSRF token mismatch")
                ) {
                    Log.e(TAG, "CSRF token mismatch detected. Adding 419 status.")
                    return "HTTP/1.1 419 Page Expired\r\n" +
                            "Content-Type: application/json\r\n" +
                            "X-CSRF-Error: true\r\n" +
                            "\r\n" +
                            boundedResponse
                }

                // Regular JSON response
                return "HTTP/1.1 200 OK\r\n" +
                        "Content-Type: application/json\r\n" +
                        "\r\n" +
                        boundedResponse
            } catch (e: Exception) {
                Log.e(TAG, "Error parsing JSON response", e)
            }
        }

        // If it already has headers (check for common header fields)
        if (boundedResponse.contains("Content-Type:", ignoreCase = true) ||
            boundedResponse.contains("Set-Cookie:", ignoreCase = true)
        ) {

            // It has some headers, but might not have the status line
            // Add a status line if it doesn't have one
            if (!boundedResponse.startsWith("HTTP/")) {
                return "HTTP/1.1 200 OK\r\n" + boundedResponse
            }
            return boundedResponse
        }

        // Default case: assume it's just content without headers
        return "HTTP/1.1 200 OK\r\n" +
                "Content-Type: text/html\r\n" +
                "\r\n" +
                boundedResponse
    }

    // All native bridge methods have been migrated to god method pattern
    // See BridgeFunctionRegistry.kt and bridge/functions/* for implementations
}