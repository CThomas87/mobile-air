/**
 * php_request_context.c — Per-job isolated execution context implementation
 */
#include "php_request_context.h"
#include "php_engine.h"
#include "php_thread_context.h"
#include "zts_guard.h"

/* PHP headers */
#include "php_embed.h"
#include "zend.h"
#include "zend_exceptions.h"
#include "zend_atomic.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>

/* Forward declaration for supervisor circuit breaker getters */
extern int supervisor_get_circuit_breaker_max(void);
extern int supervisor_get_circuit_breaker_backoff(void);
extern const char *supervisor_get_memory_limit(void);

/* Previous interrupt function (saved/restored per request) */
static __thread void (*tls_prev_interrupt_function)(zend_execute_data *) = NULL;

#ifdef __ANDROID__
#include <android/log.h>
#define RC_TAG "PhpReqCtx"
#define RC_LOGI(...) __android_log_print(ANDROID_LOG_INFO, RC_TAG, __VA_ARGS__)
#define RC_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, RC_TAG, __VA_ARGS__)
#else
#define RC_LOGI(...)                     \
    do                                   \
    {                                    \
        fprintf(stdout, "[PhpReqCtx] "); \
        fprintf(stdout, __VA_ARGS__);    \
        fprintf(stdout, "\n");           \
    } while (0)
#define RC_LOGE(...)                            \
    do                                          \
    {                                           \
        fprintf(stderr, "[PhpReqCtx] ERROR: "); \
        fprintf(stderr, __VA_ARGS__);           \
        fprintf(stderr, "\n");                  \
    } while (0)
#endif

/* ─── Output buffer constants ─── */
#define OUTPUT_CHUNK_SIZE (64 * 1024)     /* 64KB increments */
#define OUTPUT_MAX_SIZE (8 * 1024 * 1024) /* 8MB max per job */

/* ─── Internal buffer ─── */
typedef struct
{
    char *data;
    size_t length;
    size_t capacity;
} output_buffer_t;

static void buf_init(output_buffer_t *buf)
{
    buf->capacity = OUTPUT_CHUNK_SIZE;
    buf->length = 0;
    buf->data = (char *)malloc(buf->capacity);
    if (buf->data)
        buf->data[0] = '\0';
}

static void buf_free(output_buffer_t *buf)
{
    if (buf->data)
    {
        free(buf->data);
        buf->data = NULL;
    }
    buf->length = 0;
    buf->capacity = 0;
}

static void buf_append(output_buffer_t *buf, const char *str, size_t len)
{
    if (!buf->data || !str || len == 0)
        return;

    const size_t max_payload = OUTPUT_MAX_SIZE - 1; /* keep room for NUL */
    if (buf->length >= max_payload)
    {
        return;
    }

    size_t available = max_payload - buf->length;
    size_t to_copy = (len > available) ? available : len;
    if (to_copy == 0)
    {
        return;
    }

    if (buf->length + to_copy + 1 > buf->capacity)
    {
        size_t needed = buf->capacity;
        while (needed < buf->length + to_copy + 1)
        {
            needed += OUTPUT_CHUNK_SIZE;
        }
        if (needed > OUTPUT_MAX_SIZE)
        {
            needed = OUTPUT_MAX_SIZE;
        }
        char *new_buf = (char *)realloc(buf->data, needed);
        if (!new_buf)
            return;
        buf->data = new_buf;
        buf->capacity = needed;
    }

    memcpy(buf->data + buf->length, str, to_copy);
    buf->length += to_copy;
    buf->data[buf->length] = '\0';
}

/* ─── Request context structure ─── */
struct php_request_context
{
    char *job_id;
    job_type_t job_type;
    char *script_path;
    char *payload_json;

    job_status_t status;
    int exit_code;
    char *error_msg;

    output_buffer_t stdout_buf;
    output_buffer_t stderr_buf;

    int64_t started_at_ms;
    int64_t ended_at_ms;

    atomic_int cancelled;
    int crash_count; /* circuit breaker: consecutive crashes */

    /* Per-request HTTP info — avoids process-global setenv() races.
     * Populated via php_request_set_http_info() before execute. */
    char *request_method; /* e.g. "GET", "POST" */
    char *request_uri;    /* e.g. "/workers/status?foo=bar" */
    char *query_string;   /* e.g. "foo=bar" (NULL if none) */
    char *headers_raw;    /* "KEY\nVALUE\nKEY\nVALUE\n..." for $_SERVER injection */
};

/* ─── Thread-local current context ─── */
static __thread php_request_context_t *tls_current_ctx = NULL;

/* ─── Helper: current time in milliseconds ─── */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

/* ─── Helper: JSON-escape a string ─── */
static char *json_escape(const char *str)
{
    if (!str)
        return strdup("null");

    size_t len = strlen(str);
    /* Worst case: every char needs escaping (\uXXXX = 6 chars) + quotes + null */
    size_t alloc_size = len * 6 + 3;
    char *out = (char *)malloc(alloc_size);
    if (!out)
        return strdup("\"\"");

    char *p = out;
    *p++ = '"';

    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)str[i];
        switch (c)
        {
        case '"':
            *p++ = '\\';
            *p++ = '"';
            break;
        case '\\':
            *p++ = '\\';
            *p++ = '\\';
            break;
        case '\b':
            *p++ = '\\';
            *p++ = 'b';
            break;
        case '\f':
            *p++ = '\\';
            *p++ = 'f';
            break;
        case '\n':
            *p++ = '\\';
            *p++ = 'n';
            break;
        case '\r':
            *p++ = '\\';
            *p++ = 'r';
            break;
        case '\t':
            *p++ = '\\';
            *p++ = 't';
            break;
        default:
            if (c < 0x20)
            {
                p += sprintf(p, "\\u%04x", c);
            }
            else
            {
                *p++ = (char)c;
            }
            break;
        }
    }

    *p++ = '"';
    *p = '\0';
    return out;
}

/* ─── Public API ─── */

php_request_context_t *php_request_create(const char *job_id,
                                          job_type_t job_type,
                                          const char *script_path,
                                          const char *payload_json)
{
    php_request_context_t *ctx = (php_request_context_t *)calloc(1, sizeof(php_request_context_t));
    if (!ctx)
        return NULL;

    ctx->job_id = strdup(job_id ? job_id : "unknown");
    ctx->job_type = job_type;
    ctx->script_path = strdup(script_path ? script_path : "");
    ctx->payload_json = payload_json ? strdup(payload_json) : NULL;

    ctx->status = JOB_STATUS_PENDING;
    ctx->exit_code = -1;
    ctx->error_msg = NULL;

    buf_init(&ctx->stdout_buf);
    buf_init(&ctx->stderr_buf);

    ctx->started_at_ms = 0;
    ctx->ended_at_ms = 0;
    atomic_init(&ctx->cancelled, 0);
    ctx->crash_count = 0;

    ctx->request_method = NULL;
    ctx->request_uri = NULL;
    ctx->query_string = NULL;
    ctx->headers_raw = NULL;

    return ctx;
}

void php_request_destroy(php_request_context_t *ctx)
{
    if (!ctx)
        return;

    free(ctx->job_id);
    free(ctx->script_path);
    free(ctx->payload_json);
    free(ctx->error_msg);
    free(ctx->request_method);
    free(ctx->request_uri);
    free(ctx->query_string);
    free(ctx->headers_raw);
    buf_free(&ctx->stdout_buf);
    buf_free(&ctx->stderr_buf);
    free(ctx);
}

void php_request_set_http_info(php_request_context_t *ctx,
                               const char *method,
                               const char *uri,
                               const char *query_string,
                               const char *headers_raw)
{
    if (!ctx)
        return;
    free(ctx->request_method);
    free(ctx->request_uri);
    free(ctx->query_string);
    free(ctx->headers_raw);
    ctx->request_method = strdup(method ? method : "GET");
    ctx->request_uri = strdup(uri ? uri : "/");
    ctx->query_string = query_string ? strdup(query_string) : NULL;
    ctx->headers_raw = headers_raw ? strdup(headers_raw) : NULL;
}

int php_request_execute(php_request_context_t *ctx)
{
    if (!ctx)
        return -1;

    if (!php_engine_is_initialized())
    {
        ctx->status = JOB_STATUS_FAILED;
        ctx->error_msg = strdup("PHP engine not initialized");
        return -1;
    }

    if (!php_thread_is_attached())
    {
        ctx->status = JOB_STATUS_FAILED;
        ctx->error_msg = strdup("Thread not attached to TSRM");
        return -1;
    }

    /* Check cancellation before starting */
    if (atomic_load(&ctx->cancelled))
    {
        ctx->status = JOB_STATUS_CANCELLED;
        ctx->error_msg = strdup("Cancelled before execution");
        return -1;
    }

    ctx->status = JOB_STATUS_RUNNING;
    ctx->started_at_ms = now_ms();

    /* Set this as the current context for ub_write routing */
    php_request_set_current(ctx);

    RC_LOGI("Executing job %s (type=%d, script=%s)",
            ctx->job_id, ctx->job_type, ctx->script_path);

    /* ─── Set SG(request_info) for HTTP BEFORE php_request_startup ───
     * php_request_startup() calls php_hash_environment() which reads
     * SG(request_info) to populate $_SERVER.  Using context fields
     * instead of process-global getenv() makes this thread-safe. */
    if (ctx->job_type == JOB_TYPE_HTTP)
    {
        const char *method = ctx->request_method ? ctx->request_method : "GET";
        const char *uri = ctx->request_uri ? ctx->request_uri : "/";
        const char *query = ctx->query_string ? ctx->query_string : "";
        SG(request_info).request_method = (char *)method;
        SG(request_info).request_uri = (char *)uri;
        SG(request_info).query_string = (char *)query;
        SG(request_info).path_translated = ctx->script_path;
    }

    /* ─── PHP request lifecycle ─── */
    int result = 0;

    RC_LOGI("[DIAG] job %s step=1 BEFORE php_request_startup (type=%d)", ctx->job_id, ctx->job_type);

    /* Request startup — creates request-scoped state for this thread */
    if (php_request_startup() == FAILURE)
    {
        RC_LOGE("php_request_startup failed for job %s", ctx->job_id);
        ctx->status = JOB_STATUS_FAILED;
        ctx->error_msg = strdup("php_request_startup failed");
        ctx->ended_at_ms = now_ms();
        php_request_set_current(NULL);
        return -1;
    }

    RC_LOGI("[DIAG] job %s step=2 AFTER php_request_startup OK", ctx->job_id);

    /* Install cooperative cancellation via zend_interrupt_function.
     * PHP periodically checks this hook between opcodes. When the
     * cancelled flag is set, we throw a bailout to cleanly exit.
     */
    tls_prev_interrupt_function = zend_interrupt_function;
    zend_interrupt_function = php_request_interrupt_handler;

    RC_LOGI("[DIAG] job %s step=3 BEFORE memory_limit eval", ctx->job_id);

    /* Apply memory limit policy by job type.
     * Worker jobs use supervisor-configured limits.
     * HTTP jobs use a dedicated limit (env override, safe default). */
    if (ctx->job_type == JOB_TYPE_QUEUE || ctx->job_type == JOB_TYPE_SCHEDULER)
    {
        const char *mem_limit = supervisor_get_memory_limit();
        RC_LOGI("[DIAG] job %s step=3a worker memory_limit=%s", ctx->job_id, mem_limit ? mem_limit : "(null)");
        if (mem_limit && mem_limit[0] != '\0')
        {
            char ini_cmd[128];
            snprintf(ini_cmd, sizeof(ini_cmd),
                     "ini_set('memory_limit', '%s');", mem_limit);
            zend_eval_string(ini_cmd, NULL, "worker_memory_limit");
            RC_LOGI("[DIAG] job %s step=3b worker memory_limit eval DONE", ctx->job_id);
        }
    }
    else if (ctx->job_type == JOB_TYPE_HTTP)
    {
        const char *http_mem_limit = getenv("NATIVEPHP_HTTP_MEMORY_LIMIT");
        if (!http_mem_limit || http_mem_limit[0] == '\0')
        {
            http_mem_limit = "512M";
        }

        char ini_cmd[128];
        snprintf(ini_cmd, sizeof(ini_cmd),
                 "ini_set('memory_limit', '%s');", http_mem_limit);
        zend_eval_string(ini_cmd, NULL, "http_memory_limit");

        RC_LOGI("HTTP job %s using memory_limit=%s", ctx->job_id, http_mem_limit);
    }

    /* ─── Inject per-thread $_SERVER/$_ENV so PHP reads thread-safe values ─── */
    {
        const char *jt_str = (ctx->job_type == JOB_TYPE_QUEUE) ? "queue" : (ctx->job_type == JOB_TYPE_SCHEDULER) ? "scheduler"
                                                                                                                 : "http";
        const char *console_str = (ctx->job_type == JOB_TYPE_HTTP) ? "false" : "true";
        const char *mem_limit = supervisor_get_memory_limit();
        const char *safe_mem = (mem_limit && mem_limit[0]) ? mem_limit : "256M";

        char server_cmd[1024];
        snprintf(server_cmd, sizeof(server_cmd),
                 "$_SERVER['NATIVEPHP_JOB_TYPE'] = '%s';"
                 "$_SERVER['APP_RUNNING_IN_CONSOLE'] = '%s';"
                 "$_ENV['NATIVEPHP_JOB_TYPE'] = '%s';"
                 "$_ENV['APP_RUNNING_IN_CONSOLE'] = '%s';"
                 "$_SERVER['NATIVEPHP_JOB_ID'] = '%s';"
                 "$_ENV['NATIVEPHP_JOB_ID'] = '%s';"
                 "$_SERVER['NATIVEPHP_WORKER_MEMORY_LIMIT'] = '%s';"
                 "$_ENV['NATIVEPHP_WORKER_MEMORY_LIMIT'] = '%s';",
                 jt_str, console_str, jt_str, console_str,
                 ctx->job_id ? ctx->job_id : "unknown",
                 ctx->job_id ? ctx->job_id : "unknown",
                 safe_mem, safe_mem);
        zend_eval_string(server_cmd, NULL, "nativephp_env_setup");
        RC_LOGI("[DIAG] job %s step=3c $_SERVER injected (type=%s, mem=%s)", ctx->job_id, jt_str, safe_mem);
    }

    /* ─── Inject CGI/request variables into $_SERVER for HTTP requests ───
     * The embed SAPI does NOT populate $_SERVER from SG(request_info) —
     * only CGI/FPM/Apache SAPIs do that mapping automatically.  We must
     * explicitly inject REQUEST_METHOD, REQUEST_URI, QUERY_STRING,
     * SCRIPT_FILENAME, SCRIPT_NAME, SERVER_NAME, and SERVER_PORT so
     * that Laravel and its bootstrap script can read them. */
    if (ctx->job_type == JOB_TYPE_HTTP)
    {
        const char *method = ctx->request_method ? ctx->request_method : "GET";
        const char *uri = ctx->request_uri ? ctx->request_uri : "/";
        const char *query = ctx->query_string ? ctx->query_string : "";

        /* Escape URI/query for safe PHP eval (single-quote context) */
        char esc_uri[2048];
        char esc_query[2048];
        {
            size_t j = 0;
            for (size_t i = 0; uri[i] && j < sizeof(esc_uri) - 2; i++)
            {
                if (uri[i] == '\'' || uri[i] == '\\')
                    esc_uri[j++] = '\\';
                esc_uri[j++] = uri[i];
            }
            esc_uri[j] = '\0';
        }
        {
            size_t j = 0;
            for (size_t i = 0; query[i] && j < sizeof(esc_query) - 2; i++)
            {
                if (query[i] == '\'' || query[i] == '\\')
                    esc_query[j++] = '\\';
                esc_query[j++] = query[i];
            }
            esc_query[j] = '\0';
        }

        char cgi_cmd[8192];
        snprintf(cgi_cmd, sizeof(cgi_cmd),
                 "$_SERVER['REQUEST_METHOD']='%s';"
                 "$_SERVER['REQUEST_URI']='%s';"
                 "$_SERVER['QUERY_STRING']='%s';"
                 "$_SERVER['SCRIPT_FILENAME']='%s';"
                 "$_SERVER['SCRIPT_NAME']='/native.php';"
                 "$_SERVER['SERVER_NAME']='127.0.0.1';"
                 "$_SERVER['SERVER_PORT']='80';"
                 "$_SERVER['SERVER_PROTOCOL']='HTTP/1.1';",
                 method, esc_uri, esc_query, ctx->script_path);
        zend_eval_string(cgi_cmd, NULL, "inject_cgi_vars");
        RC_LOGI("[DIAG] job %s step=3d CGI vars injected", ctx->job_id);
    }

    /* ─── Inject per-request HTTP headers into $_SERVER (thread-safe) ───
     * Removes stale HTTP_* entries inherited from the process environment
     * and injects the correct per-request headers from the context.
     * This is the key mechanism that makes multi-lane HTTP execution safe. */
    if (ctx->job_type == JOB_TYPE_HTTP && ctx->headers_raw && ctx->headers_raw[0])
    {
        /* Clear any stale HTTP_* entries that leaked from process env */
        zend_eval_string(
            "foreach (array_keys($_SERVER) as $__k) { "
            "  if (str_starts_with($__k, 'HTTP_')) unset($_SERVER[$__k]); "
            "} unset($__k);",
            NULL, "clear_stale_http_headers");

        /* Parse headers_raw: "KEY\nVALUE\nKEY2\nVALUE2\n..."
         * Inject each as $_SERVER[KEY] = VALUE via zend_eval_string. */
        const char *p = ctx->headers_raw;
        while (*p)
        {
            const char *key_end = strchr(p, '\n');
            if (!key_end)
                break;

            const char *val_start = key_end + 1;
            const char *val_end = strchr(val_start, '\n');
            if (!val_end)
                val_end = val_start + strlen(val_start);

            size_t key_len = key_end - p;
            size_t val_len = val_end - val_start;

            if (key_len > 0 && key_len < 256)
            {
                char key_buf[256];
                memcpy(key_buf, p, key_len);
                key_buf[key_len] = '\0';

                /* Escape single quotes for safe PHP eval */
                size_t escaped_max = val_len * 2 + 1;
                char *escaped_val = (char *)malloc(escaped_max);
                if (escaped_val)
                {
                    size_t ep = 0;
                    for (size_t i = 0; i < val_len; i++)
                    {
                        if (val_start[i] == '\'' || val_start[i] == '\\')
                            escaped_val[ep++] = '\\';
                        escaped_val[ep++] = val_start[i];
                    }
                    escaped_val[ep] = '\0';

                    size_t cmd_size = 64 + key_len + ep;
                    char *cmd = (char *)malloc(cmd_size);
                    if (cmd)
                    {
                        snprintf(cmd, cmd_size, "$_SERVER['%s']='%s';", key_buf, escaped_val);
                        zend_eval_string(cmd, NULL, "inject_header");
                        free(cmd);
                    }
                    free(escaped_val);
                }
            }

            p = (*val_end) ? val_end + 1 : val_end;
        }

        RC_LOGI("[DIAG] job %s step=3e HTTP headers injected into $_SERVER", ctx->job_id);

        /* Ensure HTTP_HOST always has a value — Laravel needs it for URL
         * generation.  If the client didn't send a Host header (unlikely
         * but possible), fall back to the process-env value set at init. */
        zend_eval_string(
            "if (!isset($_SERVER['HTTP_HOST'])) "
            "  $_SERVER['HTTP_HOST'] = '127.0.0.1';",
            NULL, "ensure_http_host");
    }

    RC_LOGI("[DIAG] job %s step=4 BEFORE zend_first_try", ctx->job_id);

    /* Execute script in protected block */
    zend_first_try
    {
        RC_LOGI("[DIAG] job %s step=5 INSIDE zend_first_try", ctx->job_id);

        /* Define STDOUT/STDERR only for console-style jobs. */
        if (ctx->job_type != JOB_TYPE_HTTP)
        {
            RC_LOGI("[DIAG] job %s step=5a BEFORE patch_stdio eval", ctx->job_id);
            zend_eval_string(
                "if (!defined('STDOUT')) define('STDOUT', fopen('php://output', 'w')); "
                "if (!defined('STDERR')) define('STDERR', fopen('php://output', 'w'));",
                NULL, "patch_stdio");
            RC_LOGI("[DIAG] job %s step=5b AFTER patch_stdio eval", ctx->job_id);
        }

        RC_LOGI("[DIAG] job %s step=6 BEFORE zend_stream_init_filename script=%s", ctx->job_id, ctx->script_path);

        zend_file_handle file_handle;
        zend_stream_init_filename(&file_handle, ctx->script_path);

        RC_LOGI("[DIAG] job %s step=7 BEFORE php_execute_script", ctx->job_id);

        /* php_execute_script() returns bool: true on success, false on failure.
         * Do NOT compare against SUCCESS (which is 0 / zend_result) — that
         * inverts the check and makes every successful run appear as FAILURE. */
        if (php_execute_script(&file_handle))
        {
            RC_LOGI("[DIAG] job %s step=8 php_execute_script returned SUCCESS", ctx->job_id);
            ctx->exit_code = EG(exit_status);
            if (ctx->exit_code == 0 && !atomic_load(&ctx->cancelled))
            {
                ctx->status = JOB_STATUS_COMPLETED;
            }
            else if (atomic_load(&ctx->cancelled))
            {
                ctx->status = JOB_STATUS_CANCELLED;
            }
            else
            {
                ctx->status = JOB_STATUS_FAILED;

                if (!ctx->error_msg)
                {
                    if (PG(last_error_message))
                    {
                        const char *last_message = ZSTR_VAL(PG(last_error_message));
                        const char *last_file = PG(last_error_file)
                                                    ? ZSTR_VAL(PG(last_error_file))
                                                    : "(unknown file)";
                        uint32_t last_line = PG(last_error_lineno);

                        size_t needed = strlen(last_message) + strlen(last_file) + 64;
                        char *diagnostic = (char *)malloc(needed);
                        if (diagnostic)
                        {
                            snprintf(
                                diagnostic,
                                needed,
                                "%s in %s:%u",
                                last_message,
                                last_file,
                                (unsigned int)last_line);
                            ctx->error_msg = diagnostic;
                        }
                    }

                    if (!ctx->error_msg && ctx->stdout_buf.data && ctx->stdout_buf.length > 0)
                    {
                        size_t preview_len = ctx->stdout_buf.length < 512 ? ctx->stdout_buf.length : 512;
                        char *diagnostic = (char *)malloc(preview_len + 32);
                        if (diagnostic)
                        {
                            snprintf(diagnostic, preview_len + 32, "Non-zero exit with stdout: %.*s", (int)preview_len, ctx->stdout_buf.data);
                            ctx->error_msg = diagnostic;
                        }
                    }

                    if (!ctx->error_msg)
                    {
                        ctx->error_msg = strdup("PHP script exited with non-zero status and no captured error message");
                    }
                }
            }
        }
        else
        {
            RC_LOGI("[DIAG] job %s step=8 php_execute_script returned FAILURE", ctx->job_id);
            const char *last_message = PG(last_error_message)
                                           ? ZSTR_VAL(PG(last_error_message))
                                           : NULL;
            zend_bool has_exception = (EG(exception) != NULL);

            if (!atomic_load(&ctx->cancelled) && !has_exception && !last_message && ctx->stdout_buf.length > 0)
            {
                ctx->exit_code = 0;
                ctx->status = JOB_STATUS_COMPLETED;
                RC_LOGI("Job %s: treating php_execute_script FAILURE as success (stdout_len=%zu, no exception, no last_error)",
                        ctx->job_id,
                        ctx->stdout_buf.length);
            }
            else
            {
                ctx->exit_code = EG(exit_status) != 0 ? EG(exit_status) : 1;
                ctx->status = JOB_STATUS_FAILED;

                if (last_message)
                {
                    ctx->error_msg = strdup(last_message);
                }
                else if (has_exception)
                {
                    ctx->error_msg = strdup("php_execute_script failed with uncaught exception");
                }
                else
                {
                    ctx->error_msg = strdup("php_execute_script failed");
                }
            }
        }
    }
    zend_catch
    {
        RC_LOGI("[DIAG] job %s step=9 zend_catch triggered (bailout/exit)", ctx->job_id);
        ctx->exit_code = EG(exit_status);
        if (atomic_load(&ctx->cancelled))
        {
            ctx->status = JOB_STATUS_CANCELLED;
        }
        else
        {
            ctx->status = JOB_STATUS_FAILED;
            if (!ctx->error_msg)
            {
                ctx->error_msg = strdup("PHP execution caught by zend_catch (exit/bailout)");
            }
        }
        result = -1;
    }
    zend_end_try();

    /* Request shutdown — cleans up request-scoped state */
    zend_interrupt_function = tls_prev_interrupt_function;
    tls_prev_interrupt_function = NULL;
    php_request_shutdown(NULL);

    ctx->ended_at_ms = now_ms();

    /* Clear current context */
    php_request_set_current(NULL);

    RC_LOGI("Job %s finished: status=%d exit_code=%d duration=%lldms",
            ctx->job_id, ctx->status, ctx->exit_code,
            (long long)(ctx->ended_at_ms - ctx->started_at_ms));

    if (ctx->status == JOB_STATUS_FAILED)
    {
        const char *error = ctx->error_msg ? ctx->error_msg : "(no error message)";
        RC_LOGE("Job %s failure details: exit_code=%d error=%s stdout_len=%zu stderr_len=%zu",
                ctx->job_id,
                ctx->exit_code,
                error,
                ctx->stdout_buf.length,
                ctx->stderr_buf.length);

        if (ctx->stdout_buf.data && ctx->stdout_buf.length > 0)
        {
            size_t preview_len = ctx->stdout_buf.length < 512 ? ctx->stdout_buf.length : 512;
            RC_LOGE("Job %s stdout preview: %.*s",
                    ctx->job_id,
                    (int)preview_len,
                    ctx->stdout_buf.data);
        }

        if (ctx->stderr_buf.data && ctx->stderr_buf.length > 0)
        {
            size_t preview_len = ctx->stderr_buf.length < 512 ? ctx->stderr_buf.length : 512;
            RC_LOGE("Job %s stderr preview: %.*s",
                    ctx->job_id,
                    (int)preview_len,
                    ctx->stderr_buf.data);
        }
    }

    return result;
}

void php_request_cancel(php_request_context_t *ctx)
{
    if (ctx)
    {
        atomic_store(&ctx->cancelled, 1);
        RC_LOGI("Cancellation requested for job %s", ctx->job_id);
    }
}

int php_request_is_cancelled(php_request_context_t *ctx)
{
    return ctx ? atomic_load(&ctx->cancelled) : 0;
}

/* ─── Accessors ─── */

const char *php_request_get_job_id(const php_request_context_t *ctx)
{
    return ctx ? ctx->job_id : NULL;
}

job_status_t php_request_get_status(const php_request_context_t *ctx)
{
    return ctx ? ctx->status : JOB_STATUS_FAILED;
}

int php_request_get_exit_code(const php_request_context_t *ctx)
{
    return ctx ? ctx->exit_code : -1;
}

const char *php_request_get_stdout(const php_request_context_t *ctx)
{
    return (ctx && ctx->stdout_buf.data) ? ctx->stdout_buf.data : "";
}

const char *php_request_get_stderr(const php_request_context_t *ctx)
{
    return (ctx && ctx->stderr_buf.data) ? ctx->stderr_buf.data : "";
}

const char *php_request_get_error(const php_request_context_t *ctx)
{
    return ctx ? ctx->error_msg : NULL;
}

int64_t php_request_get_started_at(const php_request_context_t *ctx)
{
    return ctx ? ctx->started_at_ms : 0;
}

int64_t php_request_get_ended_at(const php_request_context_t *ctx)
{
    return ctx ? ctx->ended_at_ms : 0;
}

job_type_t php_request_get_job_type(const php_request_context_t *ctx)
{
    return ctx ? ctx->job_type : JOB_TYPE_HTTP;
}

char *php_request_to_json(const php_request_context_t *ctx)
{
    if (!ctx)
        return strdup("{\"ok\":false,\"error\":\"null context\"}");

    int ok = (ctx->status == JOB_STATUS_COMPLETED) ? 1 : 0;
    char *stdout_esc = json_escape(ctx->stdout_buf.data);
    char *stderr_esc = json_escape(ctx->stderr_buf.data);
    char *error_esc = ctx->error_msg ? json_escape(ctx->error_msg) : strdup("null");

    /* Calculate needed size */
    size_t needed = 512 + strlen(stdout_esc) + strlen(stderr_esc) + strlen(error_esc);
    char *json = (char *)malloc(needed);
    if (!json)
    {
        free(stdout_esc);
        free(stderr_esc);
        free(error_esc);
        return strdup("{\"ok\":false,\"error\":\"allocation failed\"}");
    }

    snprintf(json, needed,
             "{\"ok\":%s,\"stdout\":%s,\"stderr\":%s,\"exitCode\":%d,"
             "\"error\":%s,\"startedAt\":%lld,\"endedAt\":%lld}",
             ok ? "true" : "false",
             stdout_esc,
             stderr_esc,
             ctx->exit_code,
             error_esc,
             (long long)ctx->started_at_ms,
             (long long)ctx->ended_at_ms);

    free(stdout_esc);
    free(stderr_esc);
    free(error_esc);
    return json;
}

/* ─── TLS-based output routing ─── */

void php_request_set_current(php_request_context_t *ctx)
{
    tls_current_ctx = ctx;
}

php_request_context_t *php_request_get_current(void)
{
    return tls_current_ctx;
}

size_t php_request_ub_write(const char *str, size_t str_length)
{
    php_request_context_t *ctx = tls_current_ctx;
    if (ctx)
    {
        buf_append(&ctx->stdout_buf, str, str_length);
    }
    /* If no context, output is silently dropped (engine init phase) */
    return str_length;
}

/* ─── Cooperative cancellation via zend_interrupt_function ─── */

void php_request_interrupt_handler(zend_execute_data *execute_data)
{
    php_request_context_t *ctx = tls_current_ctx;

    if (ctx && atomic_load(&ctx->cancelled))
    {
        RC_LOGI("Cooperative cancellation triggered for job %s", ctx->job_id);
        /* Use zend_bailout to cleanly exit the PHP execution.
         * This is caught by the zend_catch block in php_request_execute. */
        zend_bailout();
    }

    /* Chain to previous handler if any */
    if (tls_prev_interrupt_function)
    {
        tls_prev_interrupt_function(execute_data);
    }
}

void php_request_set_cancelled_interrupt(php_request_context_t *ctx)
{
    if (ctx)
    {
        atomic_store(&ctx->cancelled, 1);
        /* Trigger the VM interrupt flag so PHP checks zend_interrupt_function
         * at the next opcode boundary. */
#ifdef ZTS
        /* In ZTS mode, EG() is per-thread so this is safe */
        if (php_request_get_current() == ctx)
        {
            zend_atomic_bool_store(&EG(vm_interrupt), true);
        }
#endif
    }
}

int php_request_get_crash_count(const php_request_context_t *ctx)
{
    return ctx ? ctx->crash_count : 0;
}

void php_request_increment_crash_count(php_request_context_t *ctx)
{
    if (ctx)
        ctx->crash_count++;
}
