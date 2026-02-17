<?php

namespace Native\Mobile\Worker;

use Illuminate\Support\Facades\DB;

/**
 * WorkerErrorReporter — Unified error reporting for native worker threads.
 *
 * Captures worker failures (queue job exceptions, scheduler errors, bootstrap
 * crashes) into a dedicated `worker_errors` SQLite table so the UI process
 * can display diagnostic information without parsing logcat.
 *
 * Usage:
 *   WorkerErrorReporter::capture($exception, ['queue' => 'default', 'job_id' => 42]);
 *   WorkerErrorReporter::recent(10);
 *   WorkerErrorReporter::pruneOlderThan(days: 7);
 */
class WorkerErrorReporter
{
    /** Maximum trace length stored in the database */
    protected const MAX_TRACE_LENGTH = 4096;

    /** Maximum message length stored in the database */
    protected const MAX_MESSAGE_LENGTH = 1024;

    /**
     * Capture an error from a worker thread into the worker_errors table.
     *
     * @param  \Throwable  $e       The exception or error to record.
     * @param  array       $context Optional context: worker_id, job_type, queue, job_class, job_id.
     * @return int|null    The inserted row ID, or null on failure.
     */
    public static function capture(\Throwable $e, array $context = []): ?int
    {
        try {
            $db = app('db')->connection();

            $db->table('worker_errors')->insert([
                'worker_id'   => $context['worker_id'] ?? (int) ($_SERVER['WORKER_ID'] ?? 0),
                'job_type'    => $context['job_type'] ?? ($_SERVER['NATIVEPHP_JOB_TYPE'] ?? 'unknown'),
                'queue'       => $context['queue'] ?? null,
                'job_class'   => $context['job_class'] ?? null,
                'job_id'      => $context['job_id'] ?? null,
                'error_class' => get_class($e),
                'message'     => mb_substr($e->getMessage(), 0, static::MAX_MESSAGE_LENGTH),
                'file'        => $e->getFile(),
                'line'        => $e->getLine(),
                'trace'       => mb_substr($e->getTraceAsString(), 0, static::MAX_TRACE_LENGTH),
                'php_thread'  => defined('ZEND_THREAD_SAFE') ? (function_exists('zend_thread_id') ? zend_thread_id() : null) : null,
                'memory_usage' => memory_get_usage(true),
                'created_at'  => now()->toDateTimeString(),
            ]);

            return (int) $db->getPdo()->lastInsertId();
        } catch (\Throwable $captureError) {
            // Last resort: write to error_log so at least logcat shows something
            error_log('[WorkerErrorReporter] Failed to capture error: ' . $captureError->getMessage());
            error_log('[WorkerErrorReporter] Original error: ' . $e->getMessage());

            return null;
        }
    }

    /**
     * Retrieve the most recent worker errors.
     *
     * @param  int   $limit  Maximum number of errors to return.
     * @param  array $filter Optional filters: worker_id, job_type, queue, error_class.
     * @return array
     */
    public static function recent(int $limit = 20, array $filter = []): array
    {
        try {
            $query = DB::table('worker_errors')->orderByDesc('id');

            if (isset($filter['worker_id'])) {
                $query->where('worker_id', $filter['worker_id']);
            }
            if (isset($filter['job_type'])) {
                $query->where('job_type', $filter['job_type']);
            }
            if (isset($filter['queue'])) {
                $query->where('queue', $filter['queue']);
            }
            if (isset($filter['error_class'])) {
                $query->where('error_class', $filter['error_class']);
            }

            return $query->limit($limit)->get()->toArray();
        } catch (\Throwable $e) {
            return [];
        }
    }

    /**
     * Get a summary of errors grouped by error class.
     *
     * @return array  Array of ['error_class' => ..., 'count' => ..., 'last_seen' => ...]
     */
    public static function summary(): array
    {
        try {
            return DB::table('worker_errors')
                ->selectRaw('error_class, COUNT(*) as count, MAX(created_at) as last_seen')
                ->groupBy('error_class')
                ->orderByDesc('count')
                ->get()
                ->toArray();
        } catch (\Throwable $e) {
            return [];
        }
    }

    /**
     * Get the total count of errors, optionally filtered by recency.
     *
     * @param  int|null $withinMinutes  Only count errors from the last N minutes.
     * @return int
     */
    public static function count(?int $withinMinutes = null): int
    {
        try {
            $query = DB::table('worker_errors');

            if ($withinMinutes !== null) {
                $query->where('created_at', '>=', now()->subMinutes($withinMinutes)->toDateTimeString());
            }

            return $query->count();
        } catch (\Throwable $e) {
            return 0;
        }
    }

    /**
     * Prune old errors to keep the table from growing unbounded.
     *
     * @param  int  $days  Delete errors older than this many days.
     * @return int  Number of rows deleted.
     */
    public static function pruneOlderThan(int $days = 7): int
    {
        try {
            return DB::table('worker_errors')
                ->where('created_at', '<', now()->subDays($days)->toDateTimeString())
                ->delete();
        } catch (\Throwable $e) {
            return 0;
        }
    }

    /**
     * Clear all worker errors.
     *
     * @return bool
     */
    public static function flush(): bool
    {
        try {
            DB::table('worker_errors')->truncate();
            return true;
        } catch (\Throwable $e) {
            return false;
        }
    }

    /**
     * Ensure the worker_errors table exists.
     * Called from WorkerServiceProvider during first-boot initialization.
     */
    public static function ensureTableExists(): void
    {
        try {
            $db = app('db')->connection();

            $db->statement('
                CREATE TABLE IF NOT EXISTS worker_errors (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    worker_id INTEGER NOT NULL DEFAULT 0,
                    job_type TEXT NOT NULL DEFAULT \'unknown\',
                    queue TEXT,
                    job_class TEXT,
                    job_id TEXT,
                    error_class TEXT NOT NULL,
                    message TEXT NOT NULL,
                    file TEXT,
                    line INTEGER,
                    trace TEXT,
                    php_thread INTEGER,
                    memory_usage INTEGER,
                    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
                )
            ');

            $db->statement('CREATE INDEX IF NOT EXISTS worker_errors_created_idx ON worker_errors (created_at)');
            $db->statement('CREATE INDEX IF NOT EXISTS worker_errors_class_idx ON worker_errors (error_class)');
        } catch (\Throwable $e) {
            // Non-fatal during early bootstrap
        }
    }
}
