<?php

namespace Native\Mobile\Http\Controllers;

use Illuminate\Http\JsonResponse;
use Native\Mobile\Worker\WorkerConfig;

/**
 * WorkerStatusController — API endpoint for the worker dashboard.
 *
 * Returns the current worker/supervisor status as JSON, along with
 * configuration details. Used by the dashboard Blade component and
 * can be polled by the frontend.
 *
 * Route: GET /_native/api/worker/status
 */
class WorkerStatusController
{
    public function __invoke(): JsonResponse
    {
        // Get native supervisor status if available
        $nativeStatus = [];
        if (function_exists('nativephp_call')) {
            $result = nativephp_call('Worker.Status', '{}');
            $nativeStatus = json_decode($result, true) ?: [];
        }

        // Merge with config
        $response = array_merge([
            'status' => 'unavailable',
            'activeJobs' => 0,
            'pendingJobs' => 0,
            'completedJobs' => 0,
            'failedJobs' => 0,
            'schedulerRunning' => false,
            'uptimeSeconds' => 0,
            'mode' => 0,
        ], $nativeStatus, [
            'config' => [
                'connection' => WorkerConfig::connection(),
                'queues' => WorkerConfig::queues(),
                'workerCount' => WorkerConfig::workerCount(),
                'mode' => WorkerConfig::mode(),
                'schedulerInterval' => WorkerConfig::schedulerIntervalSeconds(),
                'queuePollInterval' => WorkerConfig::queuePollIntervalSeconds(),
                'memoryLimit' => WorkerConfig::memoryLimit(),
                'circuitBreakerThreshold' => WorkerConfig::circuitBreakerThreshold(),
                'circuitBreakerBackoff' => WorkerConfig::circuitBreakerBackoff(),
                'overrideSyncDriver' => WorkerConfig::overrideSyncDriver(),
                'immediateDispatch' => WorkerConfig::immediateDispatch(),
                'logEnabled' => WorkerConfig::logEnabled(),
                'autoStart' => WorkerConfig::autoStart(),
                'platform' => WorkerConfig::isIos() ? 'ios' : (WorkerConfig::isAndroid() ? 'android' : 'local'),
            ],
        ]);

        // Add queue stats from database if available
        try {
            $pendingInDb = \DB::table('jobs')->count();
            $failedInDb = \DB::table('failed_jobs')->count();
            $response['queueStats'] = [
                'pendingInDatabase' => $pendingInDb,
                'failedInDatabase' => $failedInDb,
            ];
        } catch (\Throwable) {
            $response['queueStats'] = null;
        }

        return response()->json($response);
    }
}
