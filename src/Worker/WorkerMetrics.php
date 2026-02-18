<?php

namespace Native\Mobile\Worker;

use Illuminate\Support\Carbon;
use Illuminate\Support\Facades\Log;

/**
 * WorkerMetrics — PHP interface for native supervisor telemetry.
 *
 * Provides two data sources:
 *  1. **Native bridge** — Calls `supervisor_status_json()` via JNI/Swift
 *     bridge for live stats (active/pending jobs, uptime, failure rate, etc.)
 *  2. **Log file** — Reads the structured JSON-line worker log for historical
 *     event data (job completions, failures, timings).
 *
 * Usage:
 *   $metrics = WorkerMetrics::snapshot();
 *   $dashboard = WorkerMetrics::dashboard();
 */
class WorkerMetrics
{
    /**
     * Get a live status snapshot from the native supervisor.
     *
     * Returns the decoded JSON from supervisor_status_json(), or a
     * fallback array if the native bridge is unavailable.
     *
     * @return array{status: string, activeJobs: int, pendingJobs: int,
     *               completedJobs: int, failedJobs: int, failureRatePercent: float,
     *               schedulerRunning: bool, uptimeSeconds: int, mode: int,
     *               workerCount: int, circuitBreakerMax: int, circuitBreakerBackoff: int,
     *               sqlitePool: array{available: int, total: int}, memoryLimit: string}
     */
    public static function snapshot(): array
    {
        // Fall back to direct native supervisor function.
        if (function_exists('nativephp_supervisor_status')) {
            $json = call_user_func('nativephp_supervisor_status');
            $data = json_decode($json, true);
            if (is_array($data)) {
                return $data;
            }
        }

        // Fallback: construct from available PHP-side information
        return self::fallbackSnapshot();
    }

    /**
     * Get native queue status (zero-PHP-overhead).
     *
     * Uses the native queue module to query the jobs table directly via SQLite C API.
     * Returns queue-level job counts without bootstrapping PHP's database layer.
     *
     * @return array{total: int, queues: array<string, int>}|null
     */
    public static function queueStatus(): ?array
    {
        if (function_exists('nativephp_queue_status')) {
            $json = call_user_func('nativephp_queue_status');
            $data = json_decode($json, true);
            if (is_array($data)) {
                return $data;
            }
        }

        // Fallback: query via Laravel's database connection
        return self::fallbackQueueStatus();
    }

    /**
     * Get recent error summary from the worker error log.
     *
     * @param int $limit Maximum number of recent errors to return
     * @return array<int, array{timestamp: string, job_id: string, error: string}>
     */
    public static function recentErrors(int $limit = 20): array
    {
        $errors = [];

        try {
            $logPath = self::workerLogPath();
            if (! $logPath || ! file_exists($logPath)) {
                return [];
            }

            // Read log file in reverse (most recent entries last)
            $lines = file($logPath, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
            if (! $lines) {
                return [];
            }

            $lines = array_reverse($lines);

            foreach ($lines as $line) {
                $entry = json_decode($line, true);
                if (! is_array($entry)) {
                    continue;
                }

                if (($entry['event'] ?? '') !== 'job_failed') {
                    continue;
                }

                $errors[] = [
                    'timestamp' => isset($entry['ts'])
                        ? Carbon::createFromTimestampMs($entry['ts'])->toIso8601String()
                        : null,
                    'job_id' => $entry['job_id'] ?? '',
                    'error' => $entry['detail'] ?? 'unknown',
                ];

                if (count($errors) >= $limit) {
                    break;
                }
            }
        } catch (\Throwable $e) {
            Log::debug('[WorkerMetrics] Error reading log: ' . $e->getMessage());
        }

        return $errors;
    }

    /**
     * Get a comprehensive dashboard payload.
     *
     * Combines live status, queue status, and recent errors into a single
     * response suitable for rendering a worker dashboard UI.
     *
     * @return array{supervisor: array, queue: array|null, recentErrors: array,
     *               dbPool: array, timestamp: string}
     */
    public static function dashboard(): array
    {
        $supervisor = self::snapshot();
        $queue = self::queueStatus();
        $errors = self::recentErrors(10);
        $dbPool = NativeDbPool::instance()->stats();

        $sqlitePool = is_array($supervisor['sqlitePool'] ?? null) ? $supervisor['sqlitePool'] : [];
        $sqliteTotal = (int) ($sqlitePool['total'] ?? 0);
        $sqliteAvailable = (int) ($sqlitePool['available'] ?? 0);
        $dbPoolHasNative = (bool) ($dbPool['has_native'] ?? false);

        if (! $dbPoolHasNative && $sqliteTotal > 0) {
            $dbPool['has_native'] = true;
            $dbPool['total'] = $sqliteTotal;
            $dbPool['available'] = $sqliteAvailable;
            $dbPool['in_use'] = max(0, $sqliteTotal - $sqliteAvailable);
        }

        return [
            'supervisor' => $supervisor,
            'queue' => $queue,
            'recentErrors' => $errors,
            'dbPool' => $dbPool,
            'timestamp' => Carbon::now()->toIso8601String(),
        ];
    }

    /**
     * Get historical job throughput (jobs/minute) from the worker log.
     *
     * Reads the last N minutes of log entries and computes throughput.
     *
     * @param int $windowMinutes Number of minutes to look back
     * @return array{completed: int, failed: int, throughputPerMinute: float}
     */
    public static function throughput(int $windowMinutes = 5): array
    {
        $completed = 0;
        $failed = 0;
        $cutoffMs = (int) ((microtime(true) - ($windowMinutes * 60)) * 1000);

        try {
            $logPath = self::workerLogPath();
            if (! $logPath || ! file_exists($logPath)) {
                return ['completed' => 0, 'failed' => 0, 'throughputPerMinute' => 0.0];
            }

            $lines = file($logPath, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
            if (! $lines) {
                return ['completed' => 0, 'failed' => 0, 'throughputPerMinute' => 0.0];
            }

            // Read in reverse since recent entries are at the end
            foreach (array_reverse($lines) as $line) {
                $entry = json_decode($line, true);
                if (! is_array($entry)) {
                    continue;
                }

                $ts = $entry['ts'] ?? 0;
                if ($ts < $cutoffMs) {
                    break; // Past the window
                }

                $event = $entry['event'] ?? '';
                if ($event === 'job_completed') {
                    $completed++;
                } elseif ($event === 'job_failed') {
                    $failed++;
                }
            }
        } catch (\Throwable $e) {
            // Non-fatal
        }

        $total = $completed + $failed;
        $throughput = $windowMinutes > 0 ? round($total / $windowMinutes, 2) : 0.0;

        return [
            'completed' => $completed,
            'failed' => $failed,
            'throughputPerMinute' => $throughput,
        ];
    }

    /**
     * Get the worker log file path.
     */
    protected static function workerLogPath(): ?string
    {
        $storagePath = env('LARAVEL_STORAGE_PATH') ?: app()->storagePath();

        $path = rtrim($storagePath, '/\\') . '/logs/worker.jsonl';

        return file_exists($path) ? $path : null;
    }

    /**
     * Construct a fallback snapshot from PHP-side information.
     */
    protected static function fallbackSnapshot(): array
    {
        return [
            'status' => WorkerConfig::isNative() ? 'unknown' : 'not_native',
            'activeJobs' => 0,
            'pendingJobs' => 0,
            'completedJobs' => 0,
            'failedJobs' => 0,
            'failureRatePercent' => 0.0,
            'schedulerRunning' => false,
            'uptimeSeconds' => 0,
            'mode' => WorkerConfig::mode(),
            'workerCount' => WorkerConfig::workerCount(),
            'circuitBreakerMax' => WorkerConfig::circuitBreakerThreshold(),
            'circuitBreakerBackoff' => WorkerConfig::circuitBreakerBackoff(),
            'sqlitePool' => ['available' => 0, 'total' => 0],
            'memoryLimit' => WorkerConfig::memoryLimit(),
        ];
    }

    /**
     * Query queue status via Laravel's database connection (fallback).
     */
    protected static function fallbackQueueStatus(): ?array
    {
        try {
            $connection = config('queue.default', 'database');
            $driver = config("queue.connections.{$connection}.driver");

            if ($driver !== 'database') {
                return null;
            }

            $db = app('db')->connection();

            // Check if jobs table exists
            $tables = $db->select("SELECT name FROM sqlite_master WHERE type='table' AND name='jobs'");
            if (empty($tables)) {
                return ['total' => 0, 'queues' => []];
            }

            $rows = $db->select('SELECT queue, COUNT(*) as count FROM jobs WHERE reserved_at IS NULL GROUP BY queue');

            $queues = [];
            $total = 0;
            foreach ($rows as $row) {
                $queues[$row->queue] = $row->count;
                $total += $row->count;
            }

            return ['total' => $total, 'queues' => $queues];
        } catch (\Throwable $e) {
            return null;
        }
    }
}
