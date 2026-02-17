#include <jni.h>
#include <android/log.h>
#include "php_embed.h"
#include "PHP.h"
#include <zend_exceptions.h>

/* Supervisor API headers */
#include "supervisor.h"
#include "php_engine.h"
#include "php_request_context.h"
#include "php_thread_context.h"

// Define Android logging macros first
#define LOG_TAG "PHP-Native"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

JavaVM *g_jvm = NULL;
jobject g_bridge_instance = NULL;

// Forward declaration for bridge_jni.cpp initialization
extern jint InitializeBridgeJNI(JNIEnv *env);

// Forward declarations for bridge_jni.cpp C functions (defined in bridge_jni.cpp)
extern const char *NativePHPCall(const char *functionName, const char *parametersJSON);
extern int NativePHPCan(const char *functionName);

// Global state
static int php_initialized = 0;
static jobject g_callback_obj = NULL;
static jmethodID g_callback_method = NULL;
static char *g_collected_output = NULL;
static size_t g_collected_length = 0;
static size_t g_collected_capacity = 0;
static int g_output_limit_hit = 0;

void pipe_php_output(const char *str);

#define BUFFER_CHUNK_SIZE (256 * 1024)              // 256KB increments
#define MAX_BUFFER_SIZE (16 * 1024 * 1024)          // 16MB max buffer
#define MAX_HTTP_RESPONSE_RETURN_SIZE (1024 * 1024) // 1MB max returned to WebView bridge
#define RESPONSE_PREVIEW_SIZE 512

static void (*jni_output_callback_ptr)(const char *) = NULL;

static void log_sanitized_preview(const char *label, const char *data, size_t len)
{
    size_t preview_len = len < RESPONSE_PREVIEW_SIZE ? len : RESPONSE_PREVIEW_SIZE;
    char preview[RESPONSE_PREVIEW_SIZE + 1];
    for (size_t i = 0; i < preview_len; i++)
    {
        unsigned char ch = (unsigned char)data[i];
        preview[i] = (ch >= 32 && ch <= 126) ? (char)ch : '.';
    }
    preview[preview_len] = '\0';
    LOGE("%s (%zu bytes): %s", label, preview_len, preview);
}

static void log_output_signature(const char *data, size_t len, const char *uri)
{
    if (!data || len == 0)
    {
        LOGE("LEGACY signature: empty output uri=%s", uri ? uri : "(null)");
        return;
    }

    size_t printable = 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char ch = (unsigned char)data[i];
        if ((ch >= 32 && ch <= 126) || ch == '\n' || ch == '\r' || ch == '\t')
        {
            printable++;
        }
    }

    int printable_pct = (int)((printable * 100) / len);
    LOGE("LEGACY signature: uri=%s len=%zu printable=%d%% has_http=%s has_html=%s",
         uri ? uri : "(null)",
         len,
         printable_pct,
         strstr(data, "HTTP/") ? "yes" : "no",
         (strstr(data, "<html") || strstr(data, "<!DOCTYPE")) ? "yes" : "no");

    log_sanitized_preview("LEGACY head", data, len);
    if (len > RESPONSE_PREVIEW_SIZE)
    {
        log_sanitized_preview("LEGACY tail", data + (len - RESPONSE_PREVIEW_SIZE), RESPONSE_PREVIEW_SIZE);
    }
}

static int is_worker_probe_uri(const char *uri)
{
    if (!uri)
    {
        return 0;
    }

    return strstr(uri, "/workers/status") != NULL || strstr(uri, "/workers/activities") != NULL;
}

static void log_transport_body_presence(const char *uri, const char *output)
{
    if (!is_worker_probe_uri(uri))
    {
        return;
    }

    if (!output)
    {
        LOGI("⏱️ [TRANSPORT] uri=%s raw_len=0 has_body=no body_len=0 separator=none",
             uri ? uri : "(null)");
        return;
    }

    size_t raw_len = strlen(output);
    const char *body = NULL;
    const char *separator = "none";

    const char *http_split = strstr(output, "\r\n\r\n");
    if (http_split)
    {
        body = http_split + 4;
        separator = "crlf";
    }
    else
    {
        const char *lf_split = strstr(output, "\n\n");
        if (lf_split)
        {
            body = lf_split + 2;
            separator = "lf";
        }
    }

    size_t body_len = body ? strlen(body) : 0;
    const char *has_body = body_len > 0 ? "yes" : "no";

    LOGI("⏱️ [TRANSPORT] uri=%s raw_len=%zu has_body=%s body_len=%zu separator=%s",
         uri ? uri : "(null)",
         raw_len,
         has_body,
         body_len,
         separator);
}

static void capture_legacy_output_buffer(const char *uri)
{
    zval output_buffer;
    ZVAL_UNDEF(&output_buffer);

    if (php_output_get_contents(&output_buffer) == SUCCESS &&
        Z_TYPE(output_buffer) == IS_STRING &&
        Z_STRLEN(output_buffer) > 0)
    {
        size_t buffer_len = (size_t)Z_STRLEN(output_buffer);
        LOGI("⏱️ [TIMING] LEGACY MODE: php_output_get_contents captured %zu bytes uri=%s",
             buffer_len,
             uri ? uri : "(null)");
        pipe_php_output(Z_STRVAL(output_buffer));
    }
    else
    {
        LOGI("⏱️ [TIMING] LEGACY MODE: php_output_get_contents empty uri=%s",
             uri ? uri : "(null)");
    }

    zval_ptr_dtor(&output_buffer);
}

void clear_collected_output()
{
    if (g_collected_output)
    {
        free(g_collected_output);
        g_collected_output = NULL;
    }

    g_collected_capacity = BUFFER_CHUNK_SIZE;
    g_collected_length = 0;
    g_output_limit_hit = 0;
    g_collected_output = (char *)malloc(g_collected_capacity);
    if (g_collected_output)
    {
        g_collected_output[0] = '\0';
    }
}

void pipe_php_output(const char *str)
{

    //    LOGI("PIPE: Output received: %s", str);

    // Safety check
    if (!g_collected_output)
    {
        clear_collected_output();
        return; // Failed to allocate
    }

    size_t length = strlen(str);

    // Check if we need more space
    if (g_collected_length + length + 1 > g_collected_capacity)
    {
        // Calculate new size in chunks
        size_t needed_capacity = g_collected_capacity;
        while (needed_capacity < g_collected_length + length + 1)
        {
            needed_capacity += BUFFER_CHUNK_SIZE;
        }

        // Enforce maximum size limit
        if (needed_capacity > MAX_BUFFER_SIZE)
        {
            if (!g_output_limit_hit)
            {
                LOGE("Output buffer exceeded maximum size of %d MB", MAX_BUFFER_SIZE / (1024 * 1024));
                g_output_limit_hit = 1;
            }
            // Just return and drop output beyond this point
            return;
        }

        // Reallocate with the new size
        char *new_buffer = (char *)realloc(g_collected_output, needed_capacity);
        if (new_buffer)
        {
            g_collected_output = new_buffer;
            g_collected_capacity = needed_capacity;
        }
        else
        {
            LOGE("Failed to reallocate output buffer to %zu bytes", needed_capacity);
            return; // Failed to reallocate
        }
    }

    // Append the string
    strcpy(g_collected_output + g_collected_length, str);
    g_collected_length += length;
}

void cleanup_output_buffer()
{
    if (g_collected_output)
    {
        g_collected_output[0] = '\0';
        g_collected_length = 0;
    }
}

size_t capture_php_output(const char *str, size_t str_length)
{
    if (!str)
    {
        LOGE("capture_php_output received null buffer");
        return 0;
    }

    // Log the raw output coming from PHP
    //    LOGI("PHP output captured: length=%zu", str_length);
    if (str_length > 0)
    {
        // Log a preview of the output (first 100 chars or so)
        char preview[10001] = {0};
        strncpy(preview, str, str_length > 10000 ? 10000 : str_length);
        preview[10000] = '\0'; // Ensure null termination
    }
    else
    {
        LOGI("Empty output received");
    }

    // Rest of your original code...
    char *buffer = malloc(str_length + 1);
    if (buffer)
    {
        memcpy(buffer, str, str_length);
        buffer[str_length] = '\0';

        pipe_php_output(buffer);
        free(buffer);
    }

    return str_length;
}

void override_embed_module_output(void (*callback)(const char *))
{
    jni_output_callback_ptr = callback;
    php_embed_module.ub_write = capture_php_output;
}

void jni_output_callback(const char *output)
{
    //    LOGI("PHP Output Debug - Callback called with: %s", output);

    JNIEnv *env;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
    {
        LOGE("Failed to get JNI environment");
        return;
    }

    if (g_callback_obj && g_callback_method)
    {
        LOGI("WE MADE IT HERE");
        jstring joutput = (*env)->NewStringUTF(env, output);
        (*env)->CallVoidMethod(env, g_callback_obj, g_callback_method, joutput);
        (*env)->DeleteLocalRef(env, joutput);
    }
}

int android_header_handler(sapi_header_struct *sapi_header, sapi_header_op_enum op, sapi_headers_struct *sapi_headers)
{
    if (sapi_header && sapi_header->header)
    {
        LOGI("📤 SAPI header: %s", sapi_header->header);
    }
    else
    {
        LOGI("📤 SAPI header: <null>");
    }
    // You can collect headers here if you want
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * nativephp_call / nativephp_can — PHP functions that bridge
 * into the Kotlin BridgeRouter via JNI (bridge_jni.cpp).
 *
 * These are registered as SAPI additional_functions so every
 * php_embed_init() (HTTP requests, artisan commands, workers)
 * automatically exposes them in the PHP function table.
 * ═══════════════════════════════════════════════════════════════ */

/* string|false nativephp_call(string $function, string $parameters) */
PHP_FUNCTION(nativephp_call)
{
    /* ─── Worker-thread guard: block JNI calls to prevent deadlock ───
     * The JNI bridge calls back into Kotlin which can re-enter the
     * supervisor, causing a deadlock when called from a worker thread.
     * We check the TLS request context (immune to setenv() races). */
    php_request_context_t *ctx = php_request_get_current();
    if (ctx)
    {
        job_type_t jt = php_request_get_job_type(ctx);
        if (jt == JOB_TYPE_QUEUE || jt == JOB_TYPE_SCHEDULER)
        {
            LOGE("nativephp_call BLOCKED from worker thread (job_type=%d) to prevent JNI deadlock", jt);
            RETURN_FALSE;
        }
    }

    char *function_name = NULL;
    size_t function_name_len = 0;
    char *parameters = NULL;
    size_t parameters_len = 0;

    ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_STRING(function_name, function_name_len)
    Z_PARAM_STRING(parameters, parameters_len)
    ZEND_PARSE_PARAMETERS_END();

    LOGI("nativephp_call('%s', ...) invoked from PHP", function_name);

    const char *result = NativePHPCall(function_name, parameters);
    if (result)
    {
        RETVAL_STRING(result);
        free((void *)result); /* NativePHPCall returns strdup'd memory */
    }
    else
    {
        RETURN_FALSE;
    }
}

/* bool nativephp_can(string $function) */
PHP_FUNCTION(nativephp_can)
{
    char *function_name = NULL;
    size_t function_name_len = 0;

    ZEND_PARSE_PARAMETERS_START(1, 1)
    Z_PARAM_STRING(function_name, function_name_len)
    ZEND_PARSE_PARAMETERS_END();

    RETURN_BOOL(NativePHPCan(function_name));
}

/* Function entry table — set as php_embed_module.additional_functions */
const zend_function_entry nativephp_bridge_functions[] = {
    PHP_FE(nativephp_call, NULL)
        PHP_FE(nativephp_can, NULL)
            PHP_FE_END};

char *run_php_script_once(const char *scriptPath, const char *method, const char *uri, const char *postData, const char *headers)
{
    const char *safe_script_path = (scriptPath && scriptPath[0] != '\0') ? scriptPath : NULL;
    const char *safe_method = (method && method[0] != '\0') ? method : "GET";
    const char *safe_uri = (uri && uri[0] != '\0') ? uri : "/";
    const char *safe_post_data = postData ? postData : "";

    if (!safe_script_path)
    {
        LOGE("run_php_script_once called with empty script path");
        return strdup("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nMissing script path.");
    }

    /* ═══════════════════════════════════════════════════════════════
     * ENGINE MODE: If the persistent PHP engine is already running
     * (started by nativeEngineInit), use TSRM contexts instead of
     * php_embed_init/shutdown.  This avoids the fatal conflict of
     * calling php_embed_init twice in the same process.
     * ═══════════════════════════════════════════════════════════════ */
    if (php_engine_is_initialized())
    {
        static pthread_mutex_t g_engine_http_request_mutex = PTHREAD_MUTEX_INITIALIZER;

        LOGI("⏱️ [TIMING] run_php_script_once ENGINE MODE: START uri=%s", safe_uri);
        /* ENGINE MODE: Do NOT touch g_collected_output — it's shared
         * across threads and would race.  TLS-routed output is used instead. */

        /* Attach this thread to TSRM (no-op if already attached) */
        if (php_thread_attach() != 0)
        {
            LOGE("❌ ENGINE MODE: thread attach failed uri=%s", safe_uri);
            return strdup("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nThread attach failed.");
        }

        pthread_mutex_lock(&g_engine_http_request_mutex);

        /* Create a request context for TLS-routed output capture */
        php_request_context_t *ctx = php_request_create(
            "http-request", JOB_TYPE_HTTP, safe_script_path, NULL);
        if (!ctx)
        {
            LOGE("❌ ENGINE MODE: context creation failed uri=%s", safe_uri);
            pthread_mutex_unlock(&g_engine_http_request_mutex);
            return strdup("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nAllocation failed.");
        }
        php_request_set_current(ctx);

        /* ─── Thread-safe per-request info ───
         * Instead of process-global setenv() (which races between threads),
         * store request data on the context struct.  php_request_execute()
         * reads these to populate SG(request_info) and $_SERVER. */
        const char *query_string = "";
        const char *query_start = strchr(safe_uri, '?');
        if (query_start && strlen(query_start + 1) > 0)
        {
            query_string = query_start + 1;
        }

        php_request_set_http_info(ctx, safe_method, safe_uri, query_string, headers);

        LOGI("⏱️ [TIMING] ENGINE MODE: before php_request_execute uri=%s", safe_uri);
        int exec_rc = php_request_execute(ctx);
        LOGI("⏱️ [TIMING] ENGINE MODE: after php_request_execute uri=%s rc=%d status=%d exit=%d",
             safe_uri,
             exec_rc,
             (int)php_request_get_status(ctx),
             php_request_get_exit_code(ctx));

        /* Collect output after shutdown flushes buffering layers.
         * ENGINE MODE uses only TLS-routed output — do NOT read the
         * shared g_collected_output buffer (thread race). */
        const char *output = php_request_get_stdout(ctx);
        const char *stderr_output = php_request_get_stderr(ctx);
        const char *error_output = php_request_get_error(ctx);
        size_t output_len = php_request_get_stdout_length(ctx);
        size_t stderr_len = php_request_get_stderr_length(ctx);
        size_t error_len = error_output ? strlen(error_output) : 0;
        const char *final_output = (output_len > 0 && output) ? output : "";
        size_t final_len = output_len;
        char *response = NULL;

        if (final_len == 0)
        {
            const char *diagnostic =
                "<html><body><h2>NativePHP request returned empty output</h2>"
                "<p>URI: %s</p>"
                "<p>Error: %s</p>"
                "<p>Stderr: %s</p>"
                "<p>See logcat tag PHP-Native for execution details.</p>"
                "</body></html>";
            const char *err_preview = (error_output && error_len > 0) ? error_output : "(none)";
            const char *stderr_preview = (stderr_output && stderr_len > 0) ? stderr_output : "(none)";
            size_t needed = strlen(diagnostic) + strlen(safe_uri) + strlen(err_preview) + strlen(stderr_preview) + 64;
            response = (char *)malloc(needed);
            if (response)
            {
                snprintf(response, needed, diagnostic,
                         safe_uri,
                         err_preview,
                         stderr_preview);
            }
            else
            {
                response = strdup("NativePHP request returned empty output.");
            }
        }
        else
        {
            response = (char *)malloc(final_len + 1);
            if (response)
            {
                memcpy(response, final_output, final_len);
                response[final_len] = '\0';
            }
            else
            {
                response = strdup("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nAllocation failed.");
            }
        }

        LOGI("⏱️ [TIMING] ENGINE MODE: output_len=%zu stderr_len=%zu error_len=%zu final_len=%zu uri=%s",
             output_len, stderr_len, error_len, final_len, safe_uri);
        if (final_len > 0)
        {
            char preview[201];
            size_t plen = final_len < 200 ? final_len : 200;
            memcpy(preview, final_output, plen);
            preview[plen] = '\0';
            LOGI("⏱️ [TIMING] ENGINE MODE: output_preview=%.200s", preview);
        }
        else
        {
            LOGE("⚠️ ENGINE MODE: EMPTY output for uri=%s — TLS and stdout-stream fallback both empty", safe_uri);
        }

        php_request_set_current(NULL);
        php_request_destroy(ctx);
        pthread_mutex_unlock(&g_engine_http_request_mutex);

        LOGI("⏱️ [TIMING] run_php_script_once ENGINE MODE: END uri=%s", safe_uri);
        return response;
    }

    /* ═══════════════════════════════════════════════════════════════
     * LEGACY MODE: No persistent engine. Use php_embed_init/shutdown
     * per request (the original approach).
     * ═══════════════════════════════════════════════════════════════ */
    LOGI("🔄 run_php_script_once [LEGACY MODE] uri=%s", safe_uri);

    // 🔁 Reset in case PHP was already initialized
    if (php_initialized)
    {
        php_embed_shutdown();
        php_initialized = 0;
    }

    // 🧠 Get session path from environment (set by Kotlin)
    const char *session_path = getenv("SESSION_SAVE_PATH");
    if (!session_path)
        session_path = "/tmp"; // fallback

    // ✅ Build ini entries per request
    php_embed_module.ub_write = capture_php_output;
    php_embed_module.phpinfo_as_text = 1;
    php_embed_module.php_ini_ignore = 0;
    php_embed_module.ini_entries = "output_buffering=4096\n"
                                   "implicit_flush=0\n"
                                   "display_errors=0\n"
                                   "log_errors=1\n"
                                   "error_reporting=E_ALL\n";

    php_embed_module.header_handler = android_header_handler;

    // ✅ Register nativephp_call / nativephp_can as PHP functions
    php_embed_module.additional_functions = nativephp_bridge_functions;

    LOGI("⏱️ [TIMING] LEGACY MODE: before php_embed_init uri=%s", safe_uri);
    // ✅ Start PHP
    if (php_embed_init(0, NULL) != SUCCESS)
    {
        return strdup("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nPHP init failed.");
    }
    LOGI("⏱️ [TIMING] LEGACY MODE: after php_embed_init uri=%s", safe_uri);
    sapi_module.header_handler = android_header_handler;
    php_initialized = 1;

    // ✅ Set Laravel-relevant env vars
    setenv("REQUEST_URI", safe_uri, 1);
    setenv("REQUEST_METHOD", safe_method, 1);
    setenv("SCRIPT_FILENAME", safe_script_path, 1);
    setenv("PHP_SELF", "/native.php", 1);
    setenv("HTTP_HOST", "127.0.0.1", 1);
    setenv("APP_URL", "http://127.0.0.1", 1);
    setenv("ASSET_URL", "http://127.0.0.1/_assets/", 1);
    setenv("NATIVEPHP_RUNNING", "true", 1);
    setenv("APP_RUNNING_IN_CONSOLE", "false", 1);

    // ✅ Set QUERY_STRING and defer parsing
    const char *query_string = "";
    const char *query_start = strchr(safe_uri, '?');
    if (query_start && strlen(query_start + 1) > 0)
    {
        query_string = query_start + 1;
        setenv("QUERY_STRING", query_string, 1);
        LOGI("✅ Set QUERY_STRING: %s", query_string);
    }
    else
    {
        unsetenv("QUERY_STRING");
        LOGI("⚠️ No QUERY_STRING found in URI");
    }

    // ✅ Populate request_info on the request started by php_embed_init.
    // Do NOT call initialize_php_with_request() in legacy mode because it
    // calls php_request_startup() again, which can crash/hang.
    SG(request_info).request_method = safe_method;
    SG(request_info).request_uri = (char *)safe_uri;
    SG(request_info).query_string = (char *)query_string;

    // ✅ Parse query data and execute script in the current embed request
    zend_first_try
    {
        if (strlen(query_string) > 0)
        {
            zend_string *query = zend_string_init(query_string, strlen(query_string), 0);
            sapi_module.treat_data(PARSE_GET, query->val, NULL);
            zend_string_free(query);
            LOGI("✅ Parsed query string into $_GET");
        }

        // ✅ Execute the PHP script
        LOGI("⏱️ [TIMING] LEGACY MODE: before php_execute_script uri=%s script=%s", safe_uri, safe_script_path);
        zend_file_handle fileHandle;
        zend_stream_init_filename(&fileHandle, safe_script_path);
        php_execute_script(&fileHandle);
        LOGI("⏱️ [TIMING] LEGACY MODE: after php_execute_script uri=%s", safe_uri);

        /* Fallback capture path: pull active output buffer before
         * php_embed_shutdown() to avoid losing body content when
         * ub_write callbacks are not triggered for buffered output. */
        capture_legacy_output_buffer(safe_uri);

        LOGI("✅ PHP script finished executing");
    }
    zend_end_try();

    php_embed_shutdown();
    php_initialized = 0;
    LOGI("⏱️ [TIMING] LEGACY MODE: after php_embed_shutdown uri=%s", safe_uri);

    // ✅ Copy output after shutdown so shutdown-time buffer flush is captured
    size_t response_len = (g_collected_output != NULL) ? strlen(g_collected_output) : 0;
    LOGI("⏱️ [TIMING] LEGACY MODE: response_len=%zu uri=%s", response_len, safe_uri);

    if (response_len == 0)
    {
        LOGE("⚠️ LEGACY MODE: empty output after shutdown uri=%s", safe_uri);
        return strdup("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n<html><body><h2>NativePHP produced empty output</h2><p>URI: /</p></body></html>");
    }

    char *response = NULL;

    if (response_len > MAX_HTTP_RESPONSE_RETURN_SIZE)
    {
        log_output_signature(g_collected_output, response_len, safe_uri);

        const char *http_start = strstr(g_collected_output, "HTTP/");
        if (!http_start)
        {
            const int looks_html = (strstr(g_collected_output, "<html") != NULL) ||
                                   (strstr(g_collected_output, "<!DOCTYPE") != NULL);

            if (looks_html)
            {
                const char *prefix = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nX-NativePHP-Truncated: 1\r\n\r\n";
                const char *suffix = "\n<!-- NativePHP: response truncated -->";
                size_t prefix_len = strlen(prefix);
                size_t suffix_len = strlen(suffix);
                size_t max_body = (MAX_HTTP_RESPONSE_RETURN_SIZE > (prefix_len + suffix_len + 1))
                                      ? (MAX_HTTP_RESPONSE_RETURN_SIZE - prefix_len - suffix_len)
                                      : 0;

                response = (char *)malloc(prefix_len + max_body + suffix_len + 1);
                if (!response)
                {
                    return strdup("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\nFailed to allocate response buffer.");
                }

                memcpy(response, prefix, prefix_len);
                memcpy(response + prefix_len, g_collected_output, max_body);
                memcpy(response + prefix_len + max_body, suffix, suffix_len);
                response[prefix_len + max_body + suffix_len] = '\0';

                LOGE("⚠️ LEGACY MODE: wrapped oversized headerless HTML from %zu bytes to %zu bytes uri=%s", response_len, strlen(response), safe_uri);
                return response;
            }

            LOGE("⚠️ LEGACY MODE: oversized malformed output (no HTTP status line), returning compact 500 uri=%s", safe_uri);
            return strdup("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\nNativePHP produced oversized malformed output.");
        }

        size_t remaining = strlen(http_start);
        size_t clip_len = remaining > MAX_HTTP_RESPONSE_RETURN_SIZE ? MAX_HTTP_RESPONSE_RETURN_SIZE : remaining;
        response = (char *)malloc(clip_len + 1);
        if (!response)
        {
            return strdup("HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\nFailed to allocate response buffer.");
        }

        memcpy(response, http_start, clip_len);
        response[clip_len] = '\0';
        LOGE("⚠️ LEGACY MODE: clipped oversized response from %zu bytes to %zu bytes uri=%s", response_len, clip_len, safe_uri);
    }
    else
    {
        response = strdup(g_collected_output);
    }

    return response;
}

static pthread_mutex_t g_bridge_mutex = PTHREAD_MUTEX_INITIALIZER;

JNIEXPORT void JNICALL native_initialize(JNIEnv *env, jobject thiz)
{
    /* Always keep the bridge instance up-to-date (used by nativephp_call).
     * Different PHPBridge objects may exist (one for init, one for HTTP).
     * Protected by mutex for multi-lane safety. */
    pthread_mutex_lock(&g_bridge_mutex);
    if (g_bridge_instance)
    {
        (*env)->DeleteGlobalRef(env, g_bridge_instance);
    }
    g_bridge_instance = (*env)->NewGlobalRef(env, thiz);
    pthread_mutex_unlock(&g_bridge_mutex);

    LOGI("Bridge instance updated: %p", g_bridge_instance);
    LOGI("PHP init deferred to request/artisan execution paths");
}

JNIEXPORT jint JNICALL native_set_env(JNIEnv *env, jobject thiz,
                                      jstring name, jstring value,
                                      jint overwrite)
{
    /* Block process-global env mutation once the engine is running with
     * threads — setenv() is not thread-safe per POSIX and can corrupt
     * the environment table while worker/UI threads call getenv().
     * Pre-init env vars are safe because they're set before threads start. */
    if (php_engine_is_initialized())
    {
        const char *nameStr = (*env)->GetStringUTFChars(env, name, NULL);
        LOGE("native_set_env BLOCKED (engine running): %s — use context injection", nameStr);
        (*env)->ReleaseStringUTFChars(env, name, nameStr);
        return -1;
    }

    const char *nameStr = (*env)->GetStringUTFChars(env, name, NULL);
    const char *valueStr = (*env)->GetStringUTFChars(env, value, NULL);

    int result = setenv(nameStr, valueStr, overwrite);

    (*env)->ReleaseStringUTFChars(env, name, nameStr);
    (*env)->ReleaseStringUTFChars(env, value, valueStr);

    return result;
}

JNIEXPORT void JNICALL native_set_request_info(JNIEnv *env, jobject thiz,
                                               jstring method, jstring uri,
                                               jstring post_data)
{

    const char *methodStr = (*env)->GetStringUTFChars(env, method, NULL);
    const char *uriStr = (*env)->GetStringUTFChars(env, uri, NULL);
    const char *postStr = post_data ? (*env)->GetStringUTFChars(env, post_data, NULL) : "";

    initialize_php_with_request(postStr, methodStr, uriStr);

    (*env)->ReleaseStringUTFChars(env, method, methodStr);
    (*env)->ReleaseStringUTFChars(env, uri, uriStr);
    if (post_data)
    {
        (*env)->ReleaseStringUTFChars(env, post_data, postStr);
    }
}

JNIEXPORT void JNICALL native_set_ui_request_active(JNIEnv *env, jobject thiz, jboolean active)
{
    supervisor_set_ui_request_active(active == JNI_TRUE ? 1 : 0);
}

JNIEXPORT jstring JNICALL native_run_artisan_command(JNIEnv *env, jobject thiz, jstring jcommand)
{
    const char *command = (*env)->GetStringUTFChars(env, jcommand, NULL);
    LOGI("🛠️ runArtisanCommand: %s", command);

    // Get Laravel path (needed in both modes)
    jclass cls = (*env)->GetObjectClass(env, thiz);
    jmethodID jmethod = (*env)->GetMethodID(env, cls, "getLaravelPublicPath", "()Ljava/lang/String;");
    jstring jLaravelPath = (jstring)(*env)->CallObjectMethod(env, thiz, jmethod);
    const char *cLaravelPath = (*env)->GetStringUTFChars(env, jLaravelPath, NULL);

    char artisanPath[1024];
    snprintf(artisanPath, sizeof(artisanPath), "%s/../artisan.php", cLaravelPath);
    char basePath[1024];
    snprintf(basePath, sizeof(basePath), "%s/..", cLaravelPath);
    chdir(basePath);
    LOGI("✅ Changed CWD to Laravel base: %s", basePath);

    // Tokenize command into argc/argv
    char *artisan_argv[128];
    int artisan_argc = 0;
    artisan_argv[artisan_argc++] = "php";

    char *commandCopy = strdup(command);
    char *token = strtok(commandCopy, " ");
    while (token && artisan_argc < 127)
    {
        artisan_argv[artisan_argc++] = token;
        token = strtok(NULL, " ");
    }
    artisan_argv[artisan_argc] = NULL;

    /* Only set process-global env vars in LEGACY mode (single-threaded).
     * In ENGINE mode, per-thread injection via zend_eval_string is used
     * inside the request context — setenv() would race with workers. */
    if (!php_engine_is_initialized())
    {
        setenv("APP_RUNNING_IN_CONSOLE", "true", 1);
        setenv("PHP_SELF", "artisan.php", 1);
        setenv("APP_ENV", "local", 1);
        setenv("NATIVEPHP_RUNNING", "true", 1);
    }

    jstring result;

    /* ═══════════════════════════════════════════════════════════════
     * ENGINE MODE: Use the persistent engine with TSRM contexts.
     * ═══════════════════════════════════════════════════════════════ */
    if (php_engine_is_initialized())
    {
        LOGI("🔄 artisan [ENGINE MODE] cmd=%s", command);

        if (php_thread_attach() != 0)
        {
            LOGE("❌ Thread attach failed for artisan command");
            result = (*env)->NewStringUTF(env, "Thread attach failed");
            goto cleanup;
        }

        /* Create request context for output capture */
        php_request_context_t *ctx = php_request_create(
            "artisan", JOB_TYPE_QUEUE, artisanPath, NULL);
        if (!ctx)
        {
            result = (*env)->NewStringUTF(env, "Allocation failed");
            goto cleanup;
        }
        php_request_set_current(ctx);

        /* Set argc/argv so $_SERVER['argv'] is populated by request_startup */
        SG(request_info).argc = artisan_argc;
        SG(request_info).argv = artisan_argv;

        LOGI("⏱️ [TIMING] artisan ENGINE MODE: before php_request_startup");
        if (php_request_startup() == FAILURE)
        {
            LOGE("❌ php_request_startup failed for artisan");
            php_request_set_current(NULL);
            php_request_destroy(ctx);
            result = (*env)->NewStringUTF(env, "php_request_startup failed");
            goto cleanup;
        }
        LOGI("⏱️ [TIMING] artisan ENGINE MODE: after php_request_startup");

        /* Inject console-mode env vars into per-thread $_SERVER/$_ENV
         * (replaces the process-global setenv that was removed for thread safety) */
        zend_eval_string(
            "$_SERVER['APP_RUNNING_IN_CONSOLE'] = 'true';"
            "$_ENV['APP_RUNNING_IN_CONSOLE'] = 'true';"
            "$_SERVER['PHP_SELF'] = 'artisan.php';"
            "$_SERVER['SCRIPT_NAME'] = 'artisan.php';"
            "$_SERVER['NATIVEPHP_JOB_TYPE'] = 'queue';"
            "$_ENV['NATIVEPHP_JOB_TYPE'] = 'queue';",
            NULL, "artisan_env_setup");

        zend_first_try
        {
            /* Define STDOUT/STDERR for Symfony Console */
            zend_eval_string(
                "if (!defined('STDOUT')) define('STDOUT', fopen('php://output', 'w')); "
                "if (!defined('STDERR')) define('STDERR', fopen('php://output', 'w'));",
                NULL, "patch_stdio");

            LOGI("⏱️ [TIMING] artisan ENGINE MODE: before php_execute_script cmd=%s", command);
            zend_file_handle file_handle;
            zend_stream_init_filename(&file_handle, artisanPath);
            php_execute_script(&file_handle);
            LOGI("⏱️ [TIMING] artisan ENGINE MODE: after php_execute_script cmd=%s", command);
        }
        zend_end_try();

        /* Collect output */
        const char *output = php_request_get_stdout(ctx);
        LOGI("⏱️ [TIMING] artisan ENGINE MODE: output_len=%zu cmd=%s",
             output ? strlen(output) : 0, command);
        result = (*env)->NewStringUTF(env, output ? output : "");

        LOGI("⏱️ [TIMING] artisan ENGINE MODE: before php_request_shutdown cmd=%s", command);
        php_request_shutdown(NULL);
        LOGI("⏱️ [TIMING] artisan ENGINE MODE: after php_request_shutdown cmd=%s", command);
        php_request_set_current(NULL);
        php_request_destroy(ctx);
    }
    else
    {
        /* ═══════════════════════════════════════════════════════════
         * LEGACY MODE: Use php_embed_init/shutdown per command.
         * Only ONE php_embed_init cycle per command (not two).
         * ═══════════════════════════════════════════════════════════ */
        LOGI("⏱️ [TIMING] artisan LEGACY MODE: START cmd=%s", command);

        /* Ensure any leftover PHP is cleanly shut down first */
        if (php_initialized)
        {
            LOGI("⏱️ [TIMING] artisan LEGACY MODE: shutting down stale PHP");
            php_embed_shutdown();
            php_initialized = 0;
        }

        /* Set up JNI bridge instance (needed by nativephp_call) without
         * a wasteful extra php_embed_init/shutdown cycle. */
        if (g_bridge_instance)
        {
            (*env)->DeleteGlobalRef(env, g_bridge_instance);
        }
        g_bridge_instance = (*env)->NewGlobalRef(env, thiz);

        clear_collected_output();

        /* Configure embed SAPI for this command */
        php_embed_module.ub_write = capture_php_output;
        php_embed_module.phpinfo_as_text = 1;
        php_embed_module.php_ini_ignore = 1; /* Prevent stray php.ini from overriding our settings */
        php_embed_module.ini_entries = "display_errors=1\nlog_errors=1\nerror_reporting=E_ALL\nimplicit_flush=1\noutput_buffering=0\nregister_argc_argv=1\n";
        php_embed_module.additional_functions = nativephp_bridge_functions;

        LOGI("⏱️ [TIMING] artisan LEGACY MODE: before php_embed_init cmd=%s", command);
        LOGI("🔍 [DIAG] artisan_argc=%d artisanPath=%s", artisan_argc, artisanPath);
        for (int i = 0; i < artisan_argc; i++)
        {
            LOGI("🔍 [DIAG] artisan_argv[%d]='%s'", i, artisan_argv[i]);
        }
        if (php_embed_init(artisan_argc, artisan_argv) == SUCCESS)
        {
            php_initialized = 1;
            sapi_module.header_handler = php_embed_module.header_handler;

            LOGI("⏱️ [TIMING] artisan LEGACY MODE: php_embed_init OK, executing cmd=%s", command);

            zend_eval_string(
                "if (!defined('STDOUT')) define('STDOUT', fopen('php://output', 'w')); "
                "if (!defined('STDERR')) define('STDERR', fopen('php://output', 'w'));",
                NULL, "patch_stdio");

            zend_file_handle file_handle;
            zend_stream_init_filename(&file_handle, artisanPath);
            php_execute_script(&file_handle);

            LOGI("⏱️ [TIMING] artisan LEGACY MODE: php_execute_script DONE cmd=%s", command);

            php_embed_shutdown();
            php_initialized = 0;

            LOGI("⏱️ [TIMING] artisan LEGACY MODE: php_embed_shutdown DONE cmd=%s", command);
        }
        else
        {
            LOGE("❌ artisan LEGACY MODE: php_embed_init FAILED cmd=%s", command);
        }

        LOGI("⏱️ [TIMING] artisan LEGACY MODE: END cmd=%s output_len=%zu",
             command, g_collected_length);
        result = (*env)->NewStringUTF(env, g_collected_output ? g_collected_output : "");
    }

cleanup:
    (*env)->ReleaseStringUTFChars(env, jcommand, command);
    (*env)->ReleaseStringUTFChars(env, jLaravelPath, cLaravelPath);
    (*env)->DeleteLocalRef(env, jLaravelPath);
    free(commandCopy);

    return result;
}

JNIEXPORT jstring JNICALL native_get_laravel_root_path(JNIEnv *env, jobject thiz)
{
    // Get context from the PHPBridge instance
    jclass bridgeClass = (*env)->GetObjectClass(env, thiz);
    jfieldID contextFieldId = (*env)->GetFieldID(env, bridgeClass, "context", "Landroid/content/Context;");
    jobject context = (*env)->GetObjectField(env, thiz, contextFieldId);

    // Call getDir method on the context
    jclass contextClass = (*env)->GetObjectClass(env, context);
    jmethodID getDirMethod = (*env)->GetMethodID(env, contextClass, "getDir", "(Ljava/lang/String;I)Ljava/io/File;");
    jstring dirName = (*env)->NewStringUTF(env, "storage");
    jint mode = 0; // MODE_PRIVATE
    jobject storageDir = (*env)->CallObjectMethod(env, context, getDirMethod, dirName, mode);

    // Get the absolute path from the file object
    jclass fileClass = (*env)->GetObjectClass(env, storageDir);
    jmethodID getAbsolutePathMethod = (*env)->GetMethodID(env, fileClass, "getAbsolutePath", "()Ljava/lang/String;");
    jstring storagePath = (jstring)(*env)->CallObjectMethod(env, storageDir, getAbsolutePathMethod);

    // Convert to C string for concatenation
    const char *cStoragePath = (*env)->GetStringUTFChars(env, storagePath, NULL);

    // Concatenate with "/laravel/public"
    char fullPath[1024];
    sprintf(fullPath, "%s/laravel", cStoragePath);

    // Release resources
    (*env)->ReleaseStringUTFChars(env, storagePath, cStoragePath);
    (*env)->DeleteLocalRef(env, dirName);
    (*env)->DeleteLocalRef(env, storageDir);
    (*env)->DeleteLocalRef(env, storagePath);

    // Return the final path
    return (*env)->NewStringUTF(env, fullPath);
}

JNIEXPORT jstring JNICALL native_handle_request_once(
    JNIEnv *env, jobject thiz,
    jstring jMethod, jstring jUri, jstring jPostData, jstring jScriptPath, jstring jHeaders)
{

    const char *method = (*env)->GetStringUTFChars(env, jMethod, NULL);
    const char *uri = (*env)->GetStringUTFChars(env, jUri, NULL);
    const char *post = jPostData ? (*env)->GetStringUTFChars(env, jPostData, NULL) : "";
    const char *path = (*env)->GetStringUTFChars(env, jScriptPath, NULL);
    const char *hdrs = jHeaders ? (*env)->GetStringUTFChars(env, jHeaders, NULL) : "";

    LOGI("⏱️ [TIMING] JNI native_handle_request_once START method=%s uri=%s script=%s post_len=%zu",
         method ? method : "(null)",
         uri ? uri : "(null)",
         path ? path : "(null)",
         post ? strlen(post) : 0);

    char *output = run_php_script_once(path, method, uri, post, hdrs);

    log_transport_body_presence(uri, output);

    LOGI("⏱️ [TIMING] JNI native_handle_request_once END uri=%s output_len=%zu",
         uri ? uri : "(null)",
         output ? strlen(output) : 0);

    jstring result = (*env)->NewStringUTF(env, output ? output : "");

    // Clean up
    free(output);
    (*env)->ReleaseStringUTFChars(env, jMethod, method);
    (*env)->ReleaseStringUTFChars(env, jUri, uri);
    (*env)->ReleaseStringUTFChars(env, jScriptPath, path);
    if (jPostData)
        (*env)->ReleaseStringUTFChars(env, jPostData, post);
    if (jHeaders)
        (*env)->ReleaseStringUTFChars(env, jHeaders, hdrs);

    return result;
}

JNIEXPORT jstring JNICALL native_get_laravel_public_path(JNIEnv *env, jobject thiz)
{
    // Get context from the PHPBridge instance
    jclass bridgeClass = (*env)->GetObjectClass(env, thiz);
    jfieldID contextFieldId = (*env)->GetFieldID(env, bridgeClass, "context", "Landroid/content/Context;");
    jobject context = (*env)->GetObjectField(env, thiz, contextFieldId);

    // Call getDir method on the context
    jclass contextClass = (*env)->GetObjectClass(env, context);
    jmethodID getDirMethod = (*env)->GetMethodID(env, contextClass, "getDir", "(Ljava/lang/String;I)Ljava/io/File;");
    jstring dirName = (*env)->NewStringUTF(env, "storage");
    jint mode = 0; // MODE_PRIVATE
    jobject storageDir = (*env)->CallObjectMethod(env, context, getDirMethod, dirName, mode);

    // Get the absolute path from the file object
    jclass fileClass = (*env)->GetObjectClass(env, storageDir);
    jmethodID getAbsolutePathMethod = (*env)->GetMethodID(env, fileClass, "getAbsolutePath", "()Ljava/lang/String;");
    jstring storagePath = (jstring)(*env)->CallObjectMethod(env, storageDir, getAbsolutePathMethod);

    // Convert to C string for concatenation
    const char *cStoragePath = (*env)->GetStringUTFChars(env, storagePath, NULL);
    /* Removed: setenv("APP_RUNNING_IN_CONSOLE", "false", 1);
     * — Process-global setenv() races with worker threads.
     * APP_RUNNING_IN_CONSOLE is now injected per-thread in php_request_execute(). */

    // Concatenate with "/laravel/public"
    char fullPath[1024];
    sprintf(fullPath, "%s/laravel/public", cStoragePath);

    // Release resources
    (*env)->ReleaseStringUTFChars(env, storagePath, cStoragePath);
    (*env)->DeleteLocalRef(env, dirName);
    (*env)->DeleteLocalRef(env, storageDir);
    (*env)->DeleteLocalRef(env, storagePath);

    // Return the final path
    return (*env)->NewStringUTF(env, fullPath);
}

JNIEXPORT void JNICALL native_shutdown(JNIEnv *env, jobject thiz)
{
    if (php_initialized)
    {
        php_embed_shutdown();
        php_initialized = 0;

        if (g_callback_obj)
        {
            (*env)->DeleteGlobalRef(env, g_callback_obj);
            g_callback_obj = NULL;
        }
        g_callback_method = NULL;

        if (g_bridge_instance)
        {
            (*env)->DeleteGlobalRef(env, g_bridge_instance);
            g_bridge_instance = NULL;
        }

        // Free the collected output buffer
        if (g_collected_output)
        {
            free(g_collected_output);
            g_collected_output = NULL;
            g_collected_length = 0;
            g_collected_capacity = 0;
        }
    }
}

JNIEXPORT jstring JNICALL native_execute_script(JNIEnv *env, jobject thiz, jstring filename)
{
    const char *phpFilePath = (*env)->GetStringUTFChars(env, filename, NULL);

    zend_file_handle file_handle;
    zend_stream_init_filename(&file_handle, phpFilePath);

    php_execute_script(&file_handle);

    (*env)->ReleaseStringUTFChars(env, filename, phpFilePath);

    // Return collected output
    return (*env)->NewStringUTF(env, g_collected_output ? g_collected_output : "");
}

/* ═══════════════════════════════════════════════════════════════
 * Supervisor JNI methods — PhpSupervisorBridge native implementations
 * ═══════════════════════════════════════════════════════════════ */

JNIEXPORT jboolean JNICALL native_supervisor_engine_init(JNIEnv *env, jclass clazz,
                                                         jstring jIniPath, jstring jIniOverrides, jstring jAppBasePath)
{
    const char *iniPath = (*env)->GetStringUTFChars(env, jIniPath, NULL);
    const char *iniOverrides = (*env)->GetStringUTFChars(env, jIniOverrides, NULL);
    const char *appBasePath = (*env)->GetStringUTFChars(env, jAppBasePath, NULL);

    int rc = supervisor_engine_init(
        (strlen(iniPath) > 0) ? iniPath : NULL,
        (strlen(iniOverrides) > 0) ? iniOverrides : NULL,
        appBasePath);

    (*env)->ReleaseStringUTFChars(env, jIniPath, iniPath);
    (*env)->ReleaseStringUTFChars(env, jIniOverrides, iniOverrides);
    (*env)->ReleaseStringUTFChars(env, jAppBasePath, appBasePath);

    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL native_supervisor_start(JNIEnv *env, jclass clazz,
                                                   jint mode, jint workerCount, jstring jQueues, jstring jConnection)
{
    const char *queues = (*env)->GetStringUTFChars(env, jQueues, NULL);
    const char *connection = (*env)->GetStringUTFChars(env, jConnection, NULL);

    int rc = supervisor_start((supervisor_mode_t)mode, workerCount, queues, connection);

    (*env)->ReleaseStringUTFChars(env, jQueues, queues);
    (*env)->ReleaseStringUTFChars(env, jConnection, connection);

    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL native_supervisor_stop(JNIEnv *env, jclass clazz)
{
    supervisor_stop();
}

JNIEXPORT jstring JNICALL native_supervisor_enqueue_queue_job(JNIEnv *env, jclass clazz,
                                                              jstring jPayload, jint jPriority)
{
    const char *payload = (*env)->GetStringUTFChars(env, jPayload, NULL);
    char *jobId = supervisor_enqueue_queue_job((strlen(payload) > 0) ? payload : NULL, (int)jPriority);
    (*env)->ReleaseStringUTFChars(env, jPayload, payload);

    if (jobId)
    {
        jstring result = (*env)->NewStringUTF(env, jobId);
        free(jobId);
        return result;
    }
    return NULL;
}

JNIEXPORT jstring JNICALL native_supervisor_enqueue_scheduler_tick(JNIEnv *env, jclass clazz,
                                                                   jstring jPayload)
{
    const char *payload = (*env)->GetStringUTFChars(env, jPayload, NULL);
    char *jobId = supervisor_enqueue_scheduler_tick((strlen(payload) > 0) ? payload : NULL);
    (*env)->ReleaseStringUTFChars(env, jPayload, payload);

    if (jobId)
    {
        jstring result = (*env)->NewStringUTF(env, jobId);
        free(jobId);
        return result;
    }
    return NULL;
}

JNIEXPORT jstring JNICALL native_supervisor_await_job(JNIEnv *env, jclass clazz,
                                                      jstring jJobId, jint timeoutMs)
{
    const char *jobId = (*env)->GetStringUTFChars(env, jJobId, NULL);
    char *result = supervisor_await_job(jobId, (uint32_t)timeoutMs);
    (*env)->ReleaseStringUTFChars(env, jJobId, jobId);

    if (result)
    {
        jstring jResult = (*env)->NewStringUTF(env, result);
        free(result);
        return jResult;
    }
    return NULL;
}

JNIEXPORT jboolean JNICALL native_supervisor_cancel_job(JNIEnv *env, jclass clazz,
                                                        jstring jJobId)
{
    const char *jobId = (*env)->GetStringUTFChars(env, jJobId, NULL);
    int rc = supervisor_cancel_job(jobId);
    (*env)->ReleaseStringUTFChars(env, jJobId, jobId);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL native_supervisor_get_status(JNIEnv *env, jclass clazz)
{
    char *status = supervisor_status_json();
    if (status)
    {
        jstring result = (*env)->NewStringUTF(env, status);
        free(status);
        return result;
    }
    return (*env)->NewStringUTF(env, "{\"status\":\"error\"}");
}

JNIEXPORT void JNICALL native_supervisor_engine_shutdown(JNIEnv *env, jclass clazz)
{
    supervisor_engine_shutdown();
}

JNIEXPORT void JNICALL native_supervisor_configure_circuit_breaker(
    JNIEnv *env, jclass clazz, jint maxCrashes, jint backoffSeconds)
{
    supervisor_configure_circuit_breaker((int)maxCrashes, (int)backoffSeconds);
}

JNIEXPORT void JNICALL native_supervisor_set_memory_limit(
    JNIEnv *env, jclass clazz, jstring jLimit)
{
    const char *limit = (*env)->GetStringUTFChars(env, jLimit, NULL);
    supervisor_set_memory_limit(limit);
    (*env)->ReleaseStringUTFChars(env, jLimit, limit);
}

JNIEXPORT void JNICALL native_supervisor_set_log_file(
    JNIEnv *env, jclass clazz, jstring jPath, jint maxSizeKb)
{
    const char *path = (*env)->GetStringUTFChars(env, jPath, NULL);
    supervisor_set_log_file(path, (int)maxSizeKb);
    (*env)->ReleaseStringUTFChars(env, jPath, path);
}

JNIEXPORT void JNICALL native_supervisor_wake_workers(JNIEnv *env, jclass clazz)
{
    supervisor_wake_workers();
}

/* ═══════════════════════════════════════════════════════════════ */

static JNINativeMethod gMethods[] = {
    // PHPBridge
    {"nativeExecuteScript", "(Ljava/lang/String;)Ljava/lang/String;", (void *)native_execute_script},
    {"initialize", "()V", (void *)native_initialize},
    {"shutdown", "()V", (void *)native_shutdown},
    {"setRequestInfo", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", (void *)native_set_request_info},
    {"nativeSetUiRequestActive", "(Z)V", (void *)native_set_ui_request_active},
    {"runArtisanCommand", "(Ljava/lang/String;)Ljava/lang/String;", (void *)native_run_artisan_command},
    {"getLaravelPublicPath", "()Ljava/lang/String;", (void *)native_get_laravel_public_path},
    {"getLaravelRootPath", "()Ljava/lang/String;", (void *)native_get_laravel_root_path},

    // LaravelEnvironment
    {"nativeSetEnv", "(Ljava/lang/String;Ljava/lang/String;I)I", (void *)native_set_env},
    {"nativeHandleRequestOnce", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", (void *)native_handle_request_once}};

/* Supervisor JNI methods */
static JNINativeMethod gSupervisorMethods[] = {
    {"nativeEngineInit", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z", (void *)native_supervisor_engine_init},
    {"nativeStartSupervisor", "(IILjava/lang/String;Ljava/lang/String;)Z", (void *)native_supervisor_start},
    {"nativeStopSupervisor", "()V", (void *)native_supervisor_stop},
    {"nativeEnqueueQueueJob", "(Ljava/lang/String;I)Ljava/lang/String;", (void *)native_supervisor_enqueue_queue_job},
    {"nativeEnqueueSchedulerTick", "(Ljava/lang/String;)Ljava/lang/String;", (void *)native_supervisor_enqueue_scheduler_tick},
    {"nativeAwaitJob", "(Ljava/lang/String;I)Ljava/lang/String;", (void *)native_supervisor_await_job},
    {"nativeCancelJob", "(Ljava/lang/String;)Z", (void *)native_supervisor_cancel_job},
    {"nativeGetStatus", "()Ljava/lang/String;", (void *)native_supervisor_get_status},
    {"nativeEngineShutdown", "()V", (void *)native_supervisor_engine_shutdown},
    {"nativeConfigureCircuitBreaker", "(II)V", (void *)native_supervisor_configure_circuit_breaker},
    {"nativeSetMemoryLimit", "(Ljava/lang/String;)V", (void *)native_supervisor_set_memory_limit},
    {"nativeSetLogFile", "(Ljava/lang/String;I)V", (void *)native_supervisor_set_log_file},
    {"nativeWakeWorkers", "()V", (void *)native_supervisor_wake_workers},
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    g_jvm = vm;

    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
    {
        return JNI_ERR;
    }

    // Register native methods for PHPBridge
    jclass phpBridgeClass = (*env)->FindClass(env, "com/nativephp/mobile/bridge/PHPBridge");
    if (phpBridgeClass == NULL)
    {
        return JNI_ERR;
    }

    if ((*env)->RegisterNatives(env, phpBridgeClass, gMethods, sizeof(gMethods) / sizeof(gMethods[0])) != 0)
    {
        return JNI_ERR;
    }

    // Register native methods for LaravelEnvironment
    jclass laravelEnvClass = (*env)->FindClass(env, "com/nativephp/mobile/bridge/LaravelEnvironment");
    if (laravelEnvClass == NULL)
    {
        return JNI_ERR;
    }

    static JNINativeMethod envMethods[] = {
        {"nativeSetEnv", "(Ljava/lang/String;Ljava/lang/String;I)I", (void *)native_set_env}};

    if ((*env)->RegisterNatives(env, laravelEnvClass, envMethods, sizeof(envMethods) / sizeof(envMethods[0])) != 0)
    {
        return JNI_ERR;
    }

    // Initialize the bridge JNI module
    if (InitializeBridgeJNI(env) != JNI_OK)
    {
        LOGE("Failed to initialize BridgeJNI");
        return JNI_ERR;
    }

    // Register native methods for PhpSupervisorBridge
    jclass supervisorClass = (*env)->FindClass(env, "com/nativephp/mobile/worker/PhpSupervisorBridge");
    if (supervisorClass != NULL)
    {
        if ((*env)->RegisterNatives(env, supervisorClass, gSupervisorMethods,
                                    sizeof(gSupervisorMethods) / sizeof(gSupervisorMethods[0])) != 0)
        {
            LOGE("Failed to register PhpSupervisorBridge native methods");
            return JNI_ERR;
        }
        LOGI("Registered PhpSupervisorBridge native methods");
    }
    else
    {
        // Class not found is OK — supervisor may not be used
        LOGI("PhpSupervisorBridge class not found, skipping supervisor registration");
        (*env)->ExceptionClear(env);
    }

    return JNI_VERSION_1_6;
}
