package com.nativephp.mobile.bridge

import android.annotation.SuppressLint
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.os.Build
import android.util.Base64
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.FileInputStream
import java.io.BufferedInputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.net.HttpURLConnection
import java.net.URL
import org.json.JSONObject
import java.security.MessageDigest
import java.security.SecureRandom
import kotlinx.coroutines.*
import com.nativephp.mobile.worker.PhpSupervisorBridge

class LaravelEnvironment(private val context: Context) {
    private val appStorageDir = context.getDir("storage", Context.MODE_PRIVATE)
    private val phpBridge = PHPBridge(context)

    // Cached bundle metadata to avoid reading ZIP multiple times
    private var bundleMetadataCache: BundleMetadata? = null

    private external fun nativeSetEnv(name: String, value: String, overwrite: Int): Int

    // Data class to hold bundle metadata read from ZIP
    private data class BundleMetadata(
        val version: String?,
        val bifrostAppId: String?
    )

    // Data class for version information with utility methods
    private data class VersionInfo(val raw: String, val clean: String) {
        val isDebug: Boolean get() = clean.equals(VERSION_DEBUG, ignoreCase = true)

        companion object {
            fun from(version: String?): VersionInfo? {
                if (version == null) return null
                val clean = version.trim().trim('"').trim('\'')
                return VersionInfo(version, clean)
            }
        }
    }

    companion object {
        private const val TAG = "LaravelEnvironment"

        // File and directory names
        private const val BUNDLE_ZIP = "laravel_bundle.zip"
        private const val OTA_MARKER = ".ota_applied"
        private const val VERSION_FILE = ".version"
        private const val BUNDLE_HASH_FILE = ".bundle_hash"
        private const val ENV_FILE = ".env"
        private const val CACERT_FILE = "cacert.pem"
        private const val PHP_INI_FILE = "php.ini"
        private const val APP_KEY_FILE = "persisted_data/appkey.txt"

        // Directory paths
        private const val DIR_LARAVEL = "laravel"
        private const val DIR_PERSISTED = "persisted_data"
        private const val DIR_STORAGE = "persisted_data/storage"
        private const val DIR_FRAMEWORK = "persisted_data/storage/framework"
        private const val DIR_VIEWS = "persisted_data/storage/framework/views"
        private const val DIR_SESSIONS = "persisted_data/storage/framework/sessions"
        private const val DIR_CACHE = "persisted_data/storage/framework/cache"
        private const val DIR_LOGS = "persisted_data/storage/logs"
        private const val DIR_APP = "persisted_data/storage/app"
        private const val DIR_PUBLIC = "persisted_data/storage/app/public"
        private const val DIR_DATABASE = "persisted_data/database/"
        private const val DIR_PHP_SESSIONS = "php_sessions"

        // API URLs
        private const val BIFROST_API_BASE = "https://bifrost.nativephp.com/api/apps"

        // Version constants
        private const val VERSION_DEBUG = "DEBUG"
        private const val VERSION_DEFAULT = "0.0.0"

        // Environment variable regex patterns
        private const val REGEX_APP_VERSION = "NATIVEPHP_APP_VERSION=(.+)"
        private const val REGEX_BIFROST_ID = "BIFROST_APP_ID=(.+)"

        init {
            System.loadLibrary("php_wrapper")
        }

        /**
         * Read NATIVEPHP_START_URL from the extracted .env file
         */
        fun getStartURL(context: Context): String {
            val appStorageDir = context.getDir("storage", Context.MODE_PRIVATE)
            val laravelDir = File(appStorageDir, "laravel")
            val envFile = File(laravelDir, ".env")

            if (!envFile.exists()) {
                Log.d(TAG, "⚙️ No .env file found, using default start URL")
                return "/"
            }

            try {
                val envContent = envFile.readText()
                val pattern = Regex("""NATIVEPHP_START_URL\s*=\s*([^\r\n]+)""")
                val match = pattern.find(envContent)

                if (match != null) {
                    var value = match.groupValues[1]
                        .trim()
                        .trim('"', '\'')

                    if (value.isNotEmpty()) {
                        // Ensure path starts with /
                        if (!value.startsWith("/")) {
                            value = "/$value"
                        }
                        Log.d(TAG, "⚙️ Found start URL in .env: $value")
                        return value
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "⚠️ Error reading .env file", e)
            }

            Log.d(TAG, "⚙️ No NATIVEPHP_START_URL found, using default: /")
            return "/"
        }
    }

    fun initialize() {
        try {
            val initStart = System.currentTimeMillis()
            val persistedPublic = File(appStorageDir, "persisted_data/storage/app/public")

            Log.d(TAG, "🔍 CHECKPOINT 1 - BEFORE setupDirectories: exists=${persistedPublic.exists()}, files=${persistedPublic.listFiles()?.joinToString { it.name } ?: "none"}")
            var stepStart = System.currentTimeMillis()
            setupDirectories()
            Log.i(TAG, "⏱️ [TIMING] setupDirectories: ${System.currentTimeMillis() - stepStart}ms")

            // Check for OTA updates first (reads BIFROST_APP_ID from bundled .env)
            stepStart = System.currentTimeMillis()
            val didUpdateOrExtract = if (checkAndApplyOTAUpdate()) {
                Log.d(TAG, "✅ OTA update applied successfully")
                Log.i(TAG, "⏱️ [TIMING] OTA update: ${System.currentTimeMillis() - stepStart}ms")
                true
            } else {
                // No OTA update - extract bundled version if needed
                stepStart = System.currentTimeMillis()
                val extracted = extractLaravelBundle()
                Log.i(TAG, "⏱️ [TIMING] extractLaravelBundle: ${System.currentTimeMillis() - stepStart}ms")
                extracted
            }

            stepStart = System.currentTimeMillis()
            setupEnvironment()
            Log.i(TAG, "⏱️ [TIMING] setupEnvironment: ${System.currentTimeMillis() - stepStart}ms")

            // Run essential artisan commands in LEGACY MODE (per-request php_embed_init/shutdown).
            // Migrations are timeout-guarded so cold boot cannot block the UI for 60s+.
            stepStart = System.currentTimeMillis()
            runBaseArtisanCommands(didUpdateOrExtract)
            Log.i(TAG, "⏱️ [TIMING] runBaseArtisanCommands: ${System.currentTimeMillis() - stepStart}ms")

            // Post-migration diagnostic: list every table in the SQLite DB.
            // Writes to both logcat AND persisted_data/storage/logs/laravel.log
            // so we can confirm migrations actually created tables.
            val dbFile = File(appStorageDir, "persisted_data/database/database.sqlite")
            auditDatabaseTables(dbFile)

            // Initialize persistent engine before first render so HTTP requests
            // run on the ZTS engine path (same model used by workers).
            stepStart = System.currentTimeMillis()
            initPhpEngine()
            Log.i(TAG, "⏱️ [TIMING] initPhpEngine: ${System.currentTimeMillis() - stepStart}ms")

            val totalTime = System.currentTimeMillis() - initStart
            Log.i(TAG, "⏱️ [TIMING] initialize() TOTAL: ${totalTime}ms")
        } catch (e: Exception) {
            Log.e(TAG, "Error initializing Laravel environment", e)
            throw RuntimeException("Failed to initialize Laravel environment", e)
        }
    }

    private fun extractLaravelBundle(): Boolean {
        val laravelDir = File(appStorageDir, DIR_LARAVEL)
        val otaMarkerFile = File(laravelDir, OTA_MARKER)

        // Check if OTA is configured in both bundled and extracted versions
        val bundledBifrostId = getBifrostAppId()
        val extractedBifrostId = getBifrostAppIdFromExtracted()
        val isBundledOtaConfigured = !bundledBifrostId.isNullOrEmpty()
        val isExtractedOtaConfigured = !extractedBifrostId.isNullOrEmpty()

        // If OTA marker exists but bundled version no longer has OTA configured, remove marker and force extraction
        if (otaMarkerFile.exists() && !isBundledOtaConfigured) {
            val otaVersion = otaMarkerFile.readText().trim()
            Log.d(TAG, "🔄 OTA removed from bundled version, rolling back from OTA version $otaVersion to bundled version")
            Log.d(TAG, "🔍 Bundled BIFROST_APP_ID: '$bundledBifrostId', Extracted BIFROST_APP_ID: '$extractedBifrostId'")
            otaMarkerFile.delete()
            // Continue with extraction to rollback to bundled version
        }
        // If OTA marker exists and bundled version still has OTA configured, skip extraction
        else if (otaMarkerFile.exists() && isBundledOtaConfigured) {
            val otaVersion = otaMarkerFile.readText().trim()
            Log.d(TAG, "✅ OTA update version $otaVersion is active, skipping bundle extraction")
            return false
        }

        // Get embedded version using VersionInfo wrapper
        val embeddedVersionRaw = getVersionFromBundledEnv() ?: readVersionFromZip(BUNDLE_ZIP)
        val embeddedVersion = VersionInfo.from(embeddedVersionRaw)

        if (embeddedVersion == null) {
            Log.e(TAG, "❌ Couldn't read version from laravel_bundle.zip")
            return false
        }

        Log.d(TAG, "🔍 DEBUG: embeddedVersion from bundle = '${embeddedVersion.raw}'")

        // Check current version from .env if exists
        val currentVersionRaw = if (laravelDir.exists()) {
            val envFile = File(laravelDir, ENV_FILE)
            if (envFile.exists()) {
                getVersionFromEnvFile(envFile)
            } else {
                null
            }
        } else {
            null
        }
        val currentVersion = VersionInfo.from(currentVersionRaw)

        Log.d(TAG, "🔍 DEBUG: currentVersion = '${currentVersion?.clean ?: "none"}'")
        Log.d(TAG, "🔍 DEBUG: embeddedVersion.clean = '${embeddedVersion.clean}'")
        Log.d(TAG, "🔍 DEBUG: isDebugOverride = ${embeddedVersion.isDebug}")

        // If DEBUG mode, ALWAYS extract.
        // Otherwise extract when app version differs OR bundled zip hash differs.
        val isUpToDate = currentVersion?.clean == embeddedVersion.clean
        val bundledZipHash = readBundledZipHash()
        val extractedBundleHash = File(laravelDir, BUNDLE_HASH_FILE)
            .takeIf { it.exists() }
            ?.readText()
            ?.trim()
        val bundleHashChanged = bundledZipHash != null && bundledZipHash != extractedBundleHash
        val shouldExtract = embeddedVersion.isDebug || !isUpToDate || bundleHashChanged

        Log.d(TAG, "🔍 DEBUG: isUpToDate = $isUpToDate")
        Log.d(TAG, "🔍 DEBUG: bundleHashChanged = $bundleHashChanged")
        Log.d(TAG, "🔍 DEBUG: shouldExtract = $shouldExtract")

        if (!shouldExtract) {
            Log.d(TAG, "✅ Laravel already up to date (version ${embeddedVersion.clean})")
            return false
        }

        Log.d(TAG, "📦 Extracting Laravel bundle (new version: ${embeddedVersion.raw})")
        Log.d(TAG, "📦 Current: ${currentVersion?.raw ?: "none"}, Embedded: ${embeddedVersion.raw}")

        // Delete entire laravel directory - persisted_data is separate and safe
        if (laravelDir.exists()) {
            // Check for symlinks before deletion
            val laravelStorage = File(laravelDir, "storage")

            Log.d(TAG, "🗑️ CALLING BASH RM NOW...")
            // WORKAROUND: Kotlin's deleteRecursively() has a bug that deletes persisted_data!
            // Use system rm command instead
            try {
                val process = Runtime.getRuntime().exec(arrayOf("rm", "-rf", laravelDir.absolutePath))
                process.waitFor()
                Log.d(TAG, "✅ BASH RM COMPLETED (exit code: ${process.exitValue()})")
            } catch (e: Exception) {
                Log.e(TAG, "❌ BASH RM FAILED: ${e.message}")
                // Fallback - try to delete what we can
                laravelDir.listFiles()?.forEach { it.delete() }
            }
        }

        laravelDir.mkdirs()

        try {
            val zipStream = context.assets.open(BUNDLE_ZIP)
            unzip(zipStream, laravelDir)

            if (!bundledZipHash.isNullOrEmpty()) {
                File(laravelDir, BUNDLE_HASH_FILE).writeText(bundledZipHash)
            }

            // Remove OTA marker if it exists (we're back to bundled version)
            if (otaMarkerFile.exists()) {
                otaMarkerFile.delete()
            }

            // Update .version file to match the environment value
            val versionFromEnv = getVersionFromEnvFile(File(laravelDir, ENV_FILE))
            if (versionFromEnv != null) {
                val versionFile = File(laravelDir, VERSION_FILE)
                val cleanVersion = versionFromEnv.trim('"').trim('\'')
                versionFile.writeText(cleanVersion)
                Log.d(TAG, "✅ Updated .version file to: $cleanVersion")
            }

            Log.d(TAG, "✅ Extraction complete to ${laravelDir.absolutePath}")

            // Create storage structure for hot reload compatibility
            // Even though we use persisted_data/storage, hot reload needs laravel/storage/framework to exist
            val laravelStorageFramework = File(laravelDir, "storage/framework")
            laravelStorageFramework.mkdirs()
            File(laravelStorageFramework, "views").mkdirs()
            File(laravelStorageFramework, "cache").mkdirs()
            File(laravelStorageFramework, "sessions").mkdirs()
            Log.d(TAG, "✅ Created laravel/storage/framework for hot reload")

            // Create bootstrap/cache directory (required for Laravel's cache operations)
            val bootstrapCache = File(laravelDir, "bootstrap/cache")
            bootstrapCache.mkdirs()
            Log.d(TAG, "✅ Created laravel/bootstrap/cache for Laravel cache operations")
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to extract Laravel zip", e)
            return false
        }

        return true
    }

    private fun readBundledZipHash(): String? {
        return try {
            context.assets.open(BUNDLE_ZIP).use { input ->
                calculateMD5(input)
            }
        } catch (e: Exception) {
            Log.w(TAG, "⚠️ Failed to compute bundled zip hash", e)
            null
        }
    }

    private fun isDebugVersion(version: String?): Boolean {
        // Use VersionInfo for consistent version handling
        return VersionInfo.from(version)?.isDebug ?: false
    }

    /**
     * Read bundle metadata (version and bifrost ID) from ZIP in a single pass
     * Results are cached to avoid redundant ZIP reads
     */
    private fun readBundleMetadata(): BundleMetadata {
        // Return cached value if available
        bundleMetadataCache?.let { return it }

        var version: String? = null
        var bifrostAppId: String? = null

        try {
            val zis = ZipInputStream(context.assets.open(BUNDLE_ZIP) as java.io.InputStream)
            var entry: ZipEntry?

            // Single pass through ZIP - read both .env and .version
            while (zis.nextEntry.also { entry = it } != null) {
                when (entry?.name) {
                    ENV_FILE -> {
                        val envContent = zis.bufferedReader().readText()

                        // Extract version
                        val versionMatch = Regex(REGEX_APP_VERSION).find(envContent)
                        version = versionMatch?.groupValues?.get(1)?.trim()

                        // Extract bifrost ID
                        val bifrostIdMatch = Regex(REGEX_BIFROST_ID).find(envContent)
                        bifrostAppId = bifrostIdMatch?.groupValues?.get(1)?.trim()
                    }
                    VERSION_FILE -> {
                        // Fallback: use .version file if NATIVEPHP_APP_VERSION not in .env
                        if (version == null) {
                            version = zis.bufferedReader().readText().trim()
                        }
                    }
                }

                // Early exit if we have both values
                if (version != null && bifrostAppId != null) {
                    break
                }
            }
            zis.close()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to read bundle metadata", e)
        }

        val metadata = BundleMetadata(version, bifrostAppId)
        bundleMetadataCache = metadata
        return metadata
    }

    private fun readVersionFromZip(zipFileName: String): String? {
        // Use cached metadata instead of reading ZIP again
        return readBundleMetadata().version
    }

    private fun checkAndApplyOTAUpdate(): Boolean {
        // Check if BIFROST_APP_ID exists in environment or app metadata
        val bifrostAppId = getBifrostAppId()
        if (bifrostAppId.isNullOrEmpty()) {
            Log.d(TAG, "ℹ️ No BIFROST_APP_ID found, skipping OTA check")
            return false
        }

        val laravelDir = File(appStorageDir, DIR_LARAVEL)

        // Get current version from existing .env if available, otherwise from bundled .env
        val currentVersion = if (laravelDir.exists()) {
            val envFile = File(laravelDir, ENV_FILE)
            if (envFile.exists()) {
                getVersionFromEnvFile(envFile)
            } else {
                getVersionFromBundledEnv()
            }
        } else {
            getVersionFromBundledEnv()
        } ?: VERSION_DEFAULT

        // Special case: DEBUG version means skip OTA
        if (currentVersion == VERSION_DEBUG) {
            Log.d(TAG, "ℹ️ DEBUG version detected, skipping OTA update")
            return false
        }

        Log.d(TAG, "🔄 Checking for OTA updates...")
        Log.d(TAG, "📱 Current version: $currentVersion")
        Log.d(TAG, "🆔 Bifrost App ID: $bifrostAppId")

        return try {
            val updateInfo = checkForUpdate(bifrostAppId, currentVersion)
            if (updateInfo != null && !updateInfo.optBoolean("upToDate", true)) {
                val newVersion = updateInfo.optString("current_version", "")
                val downloadUrl = updateInfo.optString("download_url", "")

                Log.d(TAG, "📥 Update available: $currentVersion → $newVersion")

                if (downloadUrl.isNotEmpty() && newVersion != currentVersion) {
                    return downloadAndApplyUpdate(downloadUrl, newVersion)
                }
            } else {
                Log.d(TAG, "✅ App is up to date")
            }
            false
        } catch (e: Exception) {
            Log.e(TAG, "❌ OTA update check failed", e)
            false
        }
    }

    private fun getVersionFromEnvFile(envFile: File): String? {
        return try {
            val envContent = envFile.readText()
            val versionMatch = Regex(REGEX_APP_VERSION).find(envContent)
            versionMatch?.groupValues?.get(1)?.trim()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to read version from .env file", e)
            null
        }
    }

    private fun getVersionFromBundledEnv(): String? {
        // Use cached metadata instead of reading ZIP again
        return readBundleMetadata().version
    }

    private fun getBifrostAppId(): String? {
        // Use cached metadata instead of reading ZIP again
        val bifrostId = readBundleMetadata().bifrostAppId

        if (!bifrostId.isNullOrEmpty()) {
            Log.d(TAG, "Found BIFROST_APP_ID in bundled .env: $bifrostId")
        } else {
            Log.d(TAG, "No BIFROST_APP_ID found in bundled .env")
        }

        return bifrostId
    }

    private fun getBifrostAppIdFromExtracted(): String? {
        // Read from extracted .env file
        val laravelDir = File(appStorageDir, DIR_LARAVEL)
        val envFile = File(laravelDir, ENV_FILE)

        if (!envFile.exists()) {
            return null
        }

        try {
            val envContent = envFile.readText()
            val bifrostIdMatch = Regex(REGEX_BIFROST_ID).find(envContent)
            val bifrostId = bifrostIdMatch?.groupValues?.get(1)?.trim()

            if (!bifrostId.isNullOrEmpty()) {
                Log.d(TAG, "Found BIFROST_APP_ID in extracted .env: $bifrostId")
                return bifrostId
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to read BIFROST_APP_ID from extracted .env", e)
        }

        Log.d(TAG, "No BIFROST_APP_ID found in extracted .env")
        return null
    }

    private fun checkForUpdate(appId: String, currentVersion: String): JSONObject? {
        return try {
            val url = URL("$BIFROST_API_BASE/$appId/ota?installed=$currentVersion")
            val connection = url.openConnection() as HttpURLConnection

            connection.requestMethod = "GET"
            connection.connectTimeout = 10000
            connection.readTimeout = 10000
            connection.setRequestProperty("Accept", "application/json")
            connection.setRequestProperty("User-Agent", "NativePHP-Android/${android.os.Build.VERSION.RELEASE}")

            val responseCode = connection.responseCode
            if (responseCode == HttpURLConnection.HTTP_OK) {
                val response = connection.inputStream.bufferedReader().use { it.readText() }
                JSONObject(response)
            } else {
                Log.e(TAG, "OTA check failed with status: $responseCode")
                null
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to check for updates", e)
            null
        }
    }

    private fun downloadAndApplyUpdate(downloadUrl: String, newVersion: String): Boolean {
        val tempFile = File(context.cacheDir, "ota_update_$newVersion.zip")

        return try {
            // Download the update
            Log.d(TAG, "📥 Downloading update from: $downloadUrl")
            val url = URL(downloadUrl)
            val connection = url.openConnection() as HttpURLConnection
            connection.connectTimeout = 30000
            connection.readTimeout = 30000

            connection.inputStream.use { input ->
                FileOutputStream(tempFile).use { output ->
                    val buffer = ByteArray(8192)
                    var bytesRead: Int
                    var totalBytes = 0L

                    while (input.read(buffer).also { bytesRead = it } != -1) {
                        output.write(buffer, 0, bytesRead)
                        totalBytes += bytesRead

                        // Log progress every 1MB
                        if (totalBytes % (1024 * 1024) == 0L) {
                            Log.d(TAG, "📥 Downloaded ${totalBytes / (1024 * 1024)}MB...")
                        }
                    }

                    Log.d(TAG, "✅ Download complete: ${totalBytes / 1024}KB")
                }
            }

            // Apply the update
            val laravelDir = File(appStorageDir, DIR_LARAVEL)

            // Delete entire laravel directory - persisted_data is separate and safe
            if (laravelDir.exists()) {
                Log.d(TAG, "🗑️ Removing old Laravel directory for OTA update (persisted_data is safe)")
                laravelDir.deleteRecursively()
            }

            laravelDir.mkdirs()

            // Extract the update
            Log.d(TAG, "📦 Extracting OTA update...")
            FileInputStream(tempFile).use { fileInput ->
                unzip(fileInput, laravelDir)
            }

            // Update the NATIVEPHP_APP_VERSION in .env file
            val envFile = File(laravelDir, ENV_FILE)
            if (envFile.exists()) {
                var envContent = envFile.readText()

                // Update or add NATIVEPHP_APP_VERSION
                if (envContent.contains(Regex("NATIVEPHP_APP_VERSION=.*"))) {
                    envContent = envContent.replace(
                        Regex("NATIVEPHP_APP_VERSION=.*"),
                        "NATIVEPHP_APP_VERSION=$newVersion"
                    )
                } else {
                    // Add it if not present
                    envContent += "\nNATIVEPHP_APP_VERSION=$newVersion"
                }

                envFile.writeText(envContent)
                Log.d(TAG, "✅ Updated NATIVEPHP_APP_VERSION to $newVersion in .env")
            }

            // Write version marker file to prevent re-extraction of old bundle
            val otaMarkerFile = File(laravelDir, OTA_MARKER)
            otaMarkerFile.writeText(newVersion)

            // Clean up
            tempFile.delete()

            Log.d(TAG, "✅ OTA update applied successfully to version $newVersion")
            true

        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to download or apply OTA update", e)

            // Clean up on failure
            if (tempFile.exists()) {
                tempFile.delete()
            }

            false
        }
    }

    private fun unzip(inputStream: java.io.InputStream, destinationDir: File) {
        val buffer = ByteArray(65536)  // 64KB buffer - optimized for modern flash storage
        val zis = ZipInputStream(BufferedInputStream(inputStream))

        // Stream each entry directly to disk to avoid OOM on large bundles
        var ze: ZipEntry? = zis.nextEntry
        while (ze != null) {
            // Skip storage directory - we use persisted_data/storage instead
            if (ze.name.startsWith("storage/") || ze.name == "storage") {
                Log.d(TAG, "⏭️ Skipping storage directory from bundle: ${ze.name}")
                zis.closeEntry()
                ze = zis.nextEntry
                continue
            }

            val file = File(destinationDir, ze.name)

            if (ze.isDirectory) {
                file.mkdirs()
            } else {
                // Ensure parent directories exist
                file.parentFile?.mkdirs()
                // Stream directly to disk - no memory buffering
                FileOutputStream(file).use { fos ->
                    var count: Int
                    while (zis.read(buffer).also { count = it } != -1) {
                        fos.write(buffer, 0, count)
                    }
                }
            }
            zis.closeEntry()
            ze = zis.nextEntry
        }
        zis.close()
    }

    /**
     * Calculate MD5 checksum of an input stream
     */
    private fun calculateMD5(input: java.io.InputStream): String {
        val md = MessageDigest.getInstance("MD5")
        val buffer = ByteArray(8192)
        var bytesRead: Int

        while (input.read(buffer).also { bytesRead = it } != -1) {
            md.update(buffer, 0, bytesRead)
        }

        val digest = md.digest()
        return digest.joinToString("") { "%02x".format(it) }
    }

    private fun copyAssetToInternalStorage(assetName: String, targetFileName: String, forceUpdate: Boolean = false): File {
        val outFile = File(context.filesDir, targetFileName)

        if (!outFile.exists()) {
            // File doesn't exist, copy it
            Log.d(TAG, "📋 Copying asset $assetName to ${outFile.absolutePath} (new file)")
            copyAssetFile(assetName, outFile)
        } else if (forceUpdate) {
            // Force update requested, copy without checksum verification
            Log.d(TAG, "📋 Force updating asset $assetName")
            copyAssetFile(assetName, outFile)
        } else {
            // File exists and no force update - verify checksum
            try {
                val existingHash = FileInputStream(outFile).use { calculateMD5(it) }
                val bundledHash = context.assets.open(assetName).use { calculateMD5(it) }

                if (existingHash != bundledHash) {
                    Log.d(TAG, "📋 Asset $assetName has changed (checksum mismatch), updating")
                    copyAssetFile(assetName, outFile)
                } else {
                    Log.d(TAG, "📋 Asset $assetName already up to date (checksum match)")
                }
            } catch (e: Exception) {
                Log.w(TAG, "⚠️ Failed to verify checksum for $assetName, re-copying to be safe", e)
                copyAssetFile(assetName, outFile)
            }
        }

        return outFile
    }

    private fun copyAssetFile(assetName: String, outFile: File) {
        try {
            context.assets.open(assetName).use { input ->
                FileOutputStream(outFile).use { output ->
                    input.copyTo(output)
                }
            }
            Log.d(TAG, "✅ Successfully copied $assetName")
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to copy asset $assetName", e)
            throw e
        }
    }

    /**
     * Initialize the persistent PHP engine (ZTS mode) once per process.
     *
     * Called AFTER artisan init commands complete (which run in LEGACY MODE).
     * The engine stays alive for the entire app lifecycle. HTTP requests
     * and worker threads share it via TSRM per-thread contexts.
     */
    private fun initPhpEngine() {
        try {
            val appBasePath = File(appStorageDir, DIR_LARAVEL).absolutePath
            Log.i(TAG, "🚀 Initializing persistent PHP engine (appBasePath=$appBasePath)")
            Log.i(TAG, "📱 Android SUPPORTED_ABIS=${Build.SUPPORTED_ABIS.joinToString(",")}")
            Log.i(TAG, "📱 Android SUPPORTED_64_BIT_ABIS=${Build.SUPPORTED_64_BIT_ABIS.joinToString(",")}")
            Log.i(TAG, "📦 nativeLibraryDir=${context.applicationInfo.nativeLibraryDir}")

            val ok = PhpSupervisorBridge.nativeEngineInit("", "", appBasePath)
            if (ok) {
                Log.i(TAG, "✅ PHP engine initialized successfully")
            } else {
                // Returns false if already initialized — that's fine
                Log.w(TAG, "⚠️ PHP engine init returned false (may already be initialized)")
            }
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to initialize PHP engine: ${e.message}", e)
            // Don't throw — fall back to legacy per-request init
        }
    }

    private fun runBaseArtisanCommands(shouldRunMigrations: Boolean) {
        val totalStart = System.currentTimeMillis()
        val dbFile = File(appStorageDir, "persisted_data/database/database.sqlite")
        if (!dbFile.exists()) {
            Log.d(TAG, "📄 Creating empty SQLite file: ${dbFile.absolutePath}")
            dbFile.createNewFile()
        } else {
            Log.d(TAG, "✅ SQLite file already exists: ${dbFile.absolutePath}")
        }

        File(appStorageDir, "persisted_data/storage/app/public").mkdirs()

        // ── Direct file ops (replace artisan commands that boot Laravel) ──

        // 1. optimize:clear — direct file deletions (~5ms vs 60s+)
        var stepStart = System.currentTimeMillis()
        clearOptimizeCache()
        Log.i(TAG, "⏱️ [TIMING] clearOptimizeCache: ${System.currentTimeMillis() - stepStart}ms")

        // 2. storage:unlink + storage:link — symlink ops (~1ms vs 4s+)
        stepStart = System.currentTimeMillis()
        relinkStorage()
        Log.i(TAG, "⏱️ [TIMING] relinkStorage: ${System.currentTimeMillis() - stepStart}ms")

        val migrationStateStamp = File(appStorageDir, "persisted_data/.nativephp_migration_state")
        val legacyMigrationStamp = File(appStorageDir, "persisted_data/.nativephp_migrations_bootstrapped")
        val expectedMigrationState = buildMigrationStateKey()
        val currentMigrationState = migrationStateStamp.takeIf { it.exists() }?.readText()?.trim().orEmpty()
        val migrationHistoryAvailable = hasMigrationHistory(dbFile)
        val migrationsNeeded = shouldRunMigrations || currentMigrationState != expectedMigrationState || (legacyMigrationStamp.exists() && !migrationStateStamp.exists()) || !migrationHistoryAvailable
        val allowBootMigrations = (System.getenv("NATIVEPHP_RUN_MIGRATIONS_ON_BOOT") ?: "true")
            .equals("true", ignoreCase = true)

        if (migrationsNeeded) {
            if (allowBootMigrations) {
                stepStart = System.currentTimeMillis()
                Log.i(TAG, "⏱️ [TIMING] artisan 'migrate --force' starting (required for initial/load consistency)...")
                val migrateOutput = phpBridge.runArtisanCommand("migrate --force")
                Log.i(TAG, "📋 artisan 'migrate --force' output:\n$migrateOutput")
                val migrationSucceeded = hasMigrationHistory(dbFile)
                if (migrationSucceeded) {
                    migrationStateStamp.parentFile?.mkdirs()
                    migrationStateStamp.writeText(expectedMigrationState)
                    legacyMigrationStamp.parentFile?.mkdirs()
                    legacyMigrationStamp.writeText(System.currentTimeMillis().toString())
                } else {
                    Log.w(TAG, "⚠️ Migration command finished but migration history still missing; state stamp not updated")
                }
                Log.i(TAG, "⏱️ [TIMING] artisan 'migrate --force' completed in ${System.currentTimeMillis() - stepStart}ms")
            } else {
                Log.w(TAG, "⏱️ [TIMING] artisan 'migrate --force' deferred by env override (set NATIVEPHP_RUN_MIGRATIONS_ON_BOOT=true to enable)")
            }
        } else {
            Log.i(TAG, "⏱️ [TIMING] artisan 'migrate --force' skipped (migration state unchanged and migrations table present)")
        }

        val totalTime = System.currentTimeMillis() - totalStart
        Log.i(TAG, "⏱️ [TIMING] runBaseArtisanCommands TOTAL: ${totalTime}ms")
    }

    /**
     * Direct file-system equivalent of `storage:unlink` + `storage:link`.
     * Removes the old symlink (if any) and creates:
     *   laravel/public/storage → ../../persisted_data/storage/app/public
     */
    private fun relinkStorage() {
        val publicStorage = File(appStorageDir, "$DIR_LARAVEL/public/storage")
        val target = File(appStorageDir, DIR_PUBLIC)

        // storage:unlink — remove existing link or directory
        val linkPath = publicStorage.toPath()
        try {
            if (java.nio.file.Files.isSymbolicLink(linkPath)) {
                java.nio.file.Files.delete(linkPath)
                Log.d(TAG, "🔗 Removed existing symlink: ${publicStorage.absolutePath}")
            } else if (publicStorage.exists()) {
                deleteRecursive(publicStorage)
                Log.d(TAG, "🔗 Removed existing directory: ${publicStorage.absolutePath}")
            }
        } catch (e: Exception) {
            Log.w(TAG, "⚠️ Could not remove old storage link: ${e.message}")
        }

        // storage:link — create symlink
        try {
            target.mkdirs() // Ensure target exists
            java.nio.file.Files.createSymbolicLink(linkPath, target.toPath())
            Log.d(TAG, "🔗 Created symlink: ${publicStorage.absolutePath} → ${target.absolutePath}")
        } catch (e: Exception) {
            Log.w(TAG, "⚠️ Symlink creation failed, falling back to copy: ${e.message}")
            // Some Android devices/filesystems don't support symlinks — copy instead
            try {
                target.copyRecursively(publicStorage, overwrite = true)
                Log.d(TAG, "🔗 Copied storage directory as fallback")
            } catch (e2: Exception) {
                Log.e(TAG, "❌ Failed to link or copy storage: ${e2.message}")
            }
        }
    }

    /**
     * Direct file-system equivalent of `php artisan optimize:clear`.
     * Deletes all cached config, routes, events, views, and general cache.
     * This avoids booting Laravel 6+ times just to delete files.
     */
    private fun clearOptimizeCache() {
        val laravelBase = File(appStorageDir, DIR_LARAVEL)
        val bootstrapCache = File(laravelBase, "bootstrap/cache")
        val frameworkBase = File(appStorageDir, DIR_FRAMEWORK)

        // config:clear — delete cached config
        deleteIfExists(File(bootstrapCache, "config.php"))

        // route:clear — delete cached routes (Laravel uses routes-v7.php)
        deleteIfExists(File(bootstrapCache, "routes-v7.php"))
        deleteIfExists(File(bootstrapCache, "routes.php"))

        // event:clear — delete cached events
        deleteIfExists(File(bootstrapCache, "events.php"))

        // compiled:clear — delete compiled class file and services cache
        deleteIfExists(File(bootstrapCache, "compiled.php"))
        deleteIfExists(File(bootstrapCache, "services.php"))
        deleteIfExists(File(bootstrapCache, "packages.php"))

        // view:clear — delete all compiled Blade views
        val viewsDir = File(frameworkBase, "views")
        if (viewsDir.exists()) {
            val count = viewsDir.listFiles()?.count { it.delete() } ?: 0
            Log.d(TAG, "🧹 Cleared $count compiled views from ${viewsDir.absolutePath}")
        }

        // cache:clear — delete file-based cache data
        val cacheDataDir = File(frameworkBase, "cache/data")
        if (cacheDataDir.exists()) {
            deleteRecursive(cacheDataDir)
            cacheDataDir.mkdirs() // Recreate empty directory
            Log.d(TAG, "🧹 Cleared cache data from ${cacheDataDir.absolutePath}")
        }

        Log.d(TAG, "✅ optimize:clear equivalent completed via direct file ops")
    }

    private fun deleteIfExists(file: File) {
        if (file.exists()) {
            file.delete()
            Log.d(TAG, "🧹 Deleted: ${file.name}")
        }
    }

    private fun deleteRecursive(file: File) {
        if (file.isDirectory) {
            file.listFiles()?.forEach { deleteRecursive(it) }
        }
        file.delete()
    }

    private fun buildMigrationStateKey(): String {
        val laravelDir = File(appStorageDir, DIR_LARAVEL)
        val envVersion = getVersionFromEnvFile(File(laravelDir, ENV_FILE))
            ?.trim()
            ?.trim('"', '\'')
            .orEmpty()

        val migrationsDir = File(laravelDir, "database/migrations")
        val migrationFingerprint = if (!migrationsDir.exists() || !migrationsDir.isDirectory) {
            "no-migrations-dir"
        } else {
            migrationsDir.walkTopDown()
                .filter { it.isFile && it.extension.equals("php", ignoreCase = true) }
                .sortedBy { it.relativeTo(migrationsDir).path }
                .joinToString("|") {
                    val relativePath = it.relativeTo(migrationsDir).path
                    "$relativePath:${it.length()}:${it.lastModified()}"
                }
        }

        val digest = MessageDigest.getInstance("SHA-256")
            .digest(("$envVersion|$migrationFingerprint").toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }

        return "v2:$digest"
    }

    private fun hasMigrationHistory(databaseFile: File): Boolean {
        if (!databaseFile.exists()) {
            return false
        }

        return try {
            SQLiteDatabase.openDatabase(databaseFile.absolutePath, null, SQLiteDatabase.OPEN_READONLY).use { db ->
                val hasMigrationsTable = db.rawQuery(
                    "SELECT 1 FROM sqlite_master WHERE type='table' AND name='migrations' LIMIT 1",
                    null
                ).use { cursor ->
                    cursor.moveToFirst()
                }

                if (!hasMigrationsTable) {
                    return false
                }

                db.rawQuery("SELECT COUNT(*) FROM migrations", null).use { cursor ->
                    if (!cursor.moveToFirst()) {
                        return false
                    }

                    cursor.getInt(0) > 0
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "⚠️ Could not verify migration history; forcing migrate for safety", e)
            false
        }
    }

    /**
     * Post-migration diagnostic: queries sqlite_master for ALL tables and writes
     * the result to both Android logcat and the Laravel log file.
     * This runs once per cold boot to help diagnose "no such table" errors.
     */
    private fun auditDatabaseTables(databaseFile: File) {
        if (!databaseFile.exists()) {
            Log.w(TAG, "\uD83D\uDD0D TABLE AUDIT: database file does not exist at ${databaseFile.absolutePath}")
            return
        }

        try {
            val tables = mutableListOf<String>()
            val dbSizeBytes = databaseFile.length()

            SQLiteDatabase.openDatabase(databaseFile.absolutePath, null, SQLiteDatabase.OPEN_READONLY).use { db ->
                // Query all user tables (exclude sqlite_* internal tables)
                db.rawQuery(
                    "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name",
                    null
                ).use { cursor ->
                    while (cursor.moveToNext()) {
                        tables.add(cursor.getString(0))
                    }
                }
            }

            val auditMsg = buildString {
                append("\n")
                append("=" .repeat(60)).append("\n")
                append("  SQLITE TABLE AUDIT — Cold Boot Diagnostic\n")
                append("=" .repeat(60)).append("\n")
                append("  DB path: ${databaseFile.absolutePath}\n")
                append("  DB size: ${dbSizeBytes} bytes\n")
                append("  Tables found: ${tables.size}\n")
                append("-" .repeat(60)).append("\n")
                if (tables.isEmpty()) {
                    append("  ⚠️  NO TABLES FOUND — migrations may have failed silently\n")
                } else {
                    tables.forEachIndexed { idx, name ->
                        append("  ${idx + 1}. $name\n")
                    }
                }
                append("=" .repeat(60))
            }

            // Write to logcat
            Log.i(TAG, auditMsg)

            // Also write to Laravel log file so it's visible from `adb pull`
            val logDir = File(appStorageDir, DIR_LOGS)
            logDir.mkdirs()
            val logFile = File(logDir, "laravel.log")
            val timestamp = java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.US).format(java.util.Date())
            logFile.appendText("[$timestamp] local.INFO: [NativePHP] TABLE AUDIT: ${tables.size} tables found: ${tables.joinToString(", ")} | DB: ${databaseFile.absolutePath} (${dbSizeBytes} bytes)\n")

        } catch (e: Exception) {
            Log.e(TAG, "\uD83D\uDD0D TABLE AUDIT FAILED: ${e.message}", e)
        }
    }

    private fun setupDirectories() {
        try {
            // Create directories with permissions as needed
            createDirectory(DIR_FRAMEWORK, withPermissions = true)
            createDirectory(DIR_VIEWS)
            createDirectory(DIR_SESSIONS, withPermissions = true)
            createDirectory(DIR_CACHE)
            createDirectory(DIR_LOGS)
            createDirectory(DIR_APP)
            createDirectory(DIR_PUBLIC)
            createDirectory(DIR_DATABASE)

            // Set permissions on parent storage directory (owner-only)
            File(appStorageDir, DIR_STORAGE).setWritable(true, true)

            // Ensure conventional Laravel paths also exist as fallback for config defaults
            val laravelFramework = File(appStorageDir, "$DIR_LARAVEL/storage/framework")
            File(laravelFramework, "views").mkdirs()
            File(laravelFramework, "cache").mkdirs()
            File(laravelFramework, "sessions").mkdirs()
            File(appStorageDir, "$DIR_LARAVEL/bootstrap/cache").mkdirs()

        } catch (e: Exception) {
            Log.e(TAG, "Failed to create directories", e)
            throw e
        }
    }

    private fun setupEnvironment() {
        try {
            val appKeyFile = File(appStorageDir, APP_KEY_FILE)
            val appKey: String = if (appKeyFile.exists()) {
                val contents = appKeyFile.readText().trim()
                if (contents.startsWith("base64:")) {
                    Log.d(TAG, "✅ Found valid APP_KEY in file")
                    contents
                } else {
                    Log.w(TAG, "⚠️ Found invalid APP_KEY in file, regenerating...")
                    appKeyFile.delete()
                    generateAndSaveAppKey(appKeyFile)
                }
            } else {
                generateAndSaveAppKey(appKeyFile)
            }

            // Set all environment variables in batches for better performance
            setEnvironmentVariables(
                "APP_KEY" to appKey,
                // Core Laravel paths
                "DOCUMENT_ROOT" to "${appStorageDir.absolutePath}/laravel",
                "LARAVEL_BASE_PATH" to "${appStorageDir.absolutePath}/laravel",
                "COMPOSER_VENDOR_DIR" to "${appStorageDir.absolutePath}/laravel/vendor",
                "COMPOSER_AUTOLOADER_PATH" to "${appStorageDir.absolutePath}/laravel/vendor/autoload.php",
                // Laravel storage paths
                "LARAVEL_STORAGE_PATH" to "${appStorageDir.absolutePath}/persisted_data/storage",
                "LARAVEL_BOOTSTRAP_PATH" to "${appStorageDir.absolutePath}/laravel/bootstrap",
                "VIEW_COMPILED_PATH" to "${appStorageDir.absolutePath}/persisted_data/storage/framework/views",
                "CACHE_PATH" to "${appStorageDir.absolutePath}/persisted_data/storage/framework/cache"
            )

            setEnvironmentVariables(
                // Laravel environment settings
                "APP_ENV" to "local",
                "APP_URL" to "http://127.0.0.1",
                "ASSET_URL" to "http://127.0.0.1/_assets",
                "DB_CONNECTION" to "sqlite",
                "DB_DATABASE" to "${appStorageDir.absolutePath}/persisted_data/database/database.sqlite",
                "CACHE_DRIVER" to "file",
                "CACHE_STORE" to "file",
                "QUEUE_CONNECTION" to "sync",
                "NATIVEPHP_PLATFORM" to "android",
                "NATIVEPHP_TEMPDIR" to context.cacheDir.absolutePath,
                "NATIVEPHP_WORKER_MEMORY_LIMIT" to "512M"
            )

            setEnvironmentVariables(
                // Cookie settings
                "COOKIE_PATH" to "/",
                "COOKIE_DOMAIN" to "127.0.0.1",
                "COOKIE_SECURE" to "false",
                "COOKIE_HTTP_ONLY" to "true",
                // Session settings
                "SESSION_DRIVER" to "file",
                "SESSION_DOMAIN" to "127.0.0.1",
                "SESSION_SECURE_COOKIE" to "false",
                "SESSION_HTTP_ONLY" to "true",
                "SESSION_SAME_SITE" to "lax"
            )

            setEnvironmentVariables(
                // PHP paths and settings
                "PHP_INI_SCAN_DIR" to appStorageDir.absolutePath,
                "CA_CERT_DIR" to context.filesDir.absolutePath,
                "PHPRC" to context.filesDir.absolutePath,
                // PHP/Server environment
                "REMOTE_ADDR" to "127.0.0.1",
                "SERVER_NAME" to "127.0.0.1",
                "SERVER_PORT" to "80",
                "SERVER_PROTOCOL" to "HTTP/1.1",
                "REQUEST_SCHEME" to "http"
            )

            Log.d(TAG, "✅ Environment variables configured")

            val phpSessionDir = File(appStorageDir, DIR_PHP_SESSIONS).apply {
                mkdirs()
                setReadable(true, true)
                setWritable(true, true)
                setExecutable(true, true)
            }
            setEnvironmentVariable("SESSION_SAVE_PATH", phpSessionDir.absolutePath)
            Log.d(TAG, "PHP session path set to: ${phpSessionDir.absolutePath}")

            try {
                // Check if we're in DEBUG mode to force certificate refresh
                val isDebugMode = try {
                    val versionFile = File(appStorageDir, "$DIR_LARAVEL/$VERSION_FILE")
                    versionFile.exists() && versionFile.readText().trim() == VERSION_DEBUG
                } catch (e: Exception) {
                    false
                }

                Log.d(TAG, "🔍 Certificate copy - DEBUG mode: $isDebugMode")
                copyAssetToInternalStorage(CACERT_FILE, CACERT_FILE, forceUpdate = isDebugMode)

                val phpIni = """
curl.cainfo="${context.filesDir.absolutePath}/$CACERT_FILE"
openssl.cafile="${context.filesDir.absolutePath}/$CACERT_FILE"
"""
                File(context.filesDir, PHP_INI_FILE).writeText(phpIni)
                Log.d(TAG, "✅ PHP ini configured with certificate path")
            } catch (e: Exception) {
                Log.e(TAG, "❌ Failed to copy or set CURL_CA_BUNDLE", e)
            }

        } catch (e: Exception) {
            Log.e(TAG, "Failed to setup environment", e)
            throw e
        }
    }

    private fun generateAndSaveAppKey(file: File): String {
        val keyBytes = ByteArray(32)
        SecureRandom().nextBytes(keyBytes)
        val generatedKey = "base64:${Base64.encodeToString(keyBytes, Base64.NO_WRAP)}"

        file.parentFile?.mkdirs()
        file.writeText(generatedKey)

        Log.d(TAG, "🔐 Generated and stored new APP_KEY: $generatedKey")
        return generatedKey
    }

    private fun setEnvironmentVariable(name: String, value: String) {
        try {
            val result = nativeSetEnv(name, value, 1)
            if (result != 0) {
                // Engine is already running with worker threads — setenv() is
                // blocked because it is not thread-safe.  This is expected on
                // process restart when PhpWorkerService initialises the engine
                // before setupEnvironment() completes.  Workers will read env
                // vars from the .env file via Laravel's Dotenv loader, so this
                // is non-fatal.
                Log.w(TAG, "⚠️ Could not set env var (engine running): $name — workers will use .env fallback")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to set environment variable: $name", e)
            throw e
        }
    }

    /**
     * Set multiple environment variables at once
     * More efficient than individual calls due to reduced JNI overhead
     */
    private fun setEnvironmentVariables(vararg pairs: Pair<String, String>) {
        for ((name, value) in pairs) {
            setEnvironmentVariable(name, value)
        }
    }

    private fun createDirectory(path: String, withPermissions: Boolean = false) {
        val dir = File(appStorageDir, path)

        // Skip if already exists
        if (dir.exists()) return

        dir.mkdirs()

        // Set owner-only permissions if requested
        if (withPermissions) {
            dir.setReadable(true, true)
            dir.setWritable(true, true)
            dir.setExecutable(true, true)
        }
    }

    fun cleanup() {
        try {
            phpBridge.shutdown()
        } catch (e: Exception) {
            Log.e(TAG, "Error during cleanup", e)
        }
    }
}
