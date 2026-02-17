<?php

namespace Native\Mobile\Worker;

use Illuminate\Support\Facades\Log;

/**
 * IosWorkerScheduler — Manages background work within iOS's constrained execution model.
 *
 * iOS grants background execution via BGProcessingTask (~30 seconds) and
 * BGAppRefreshTask (~30 seconds). Unlike Android's Foreground Service which
 * can run for hours, iOS background tasks must complete within a tight window.
 *
 * Strategy:
 *  - Each BGProcessingTask invocation processes as many queue jobs as possible
 *    within the time budget (default: 25 seconds, 5s safety margin).
 *  - The scheduler tick runs once per BGProcessingTask invocation if enabled.
 *  - When the deadline approaches, the worker gracefully exits and reschedules
 *    the next BGProcessingTask.
 *
 * Usage from Swift:
 *   IosWorkerScheduler begins automatically via WorkerServiceProvider when
 *   running on iOS. The Swift bridge should:
 *   1. Register BGProcessingTask with identifier "com.nativephp.worker"
 *   2. On task execution, call the PHP worker bootstrap entrypoint
 *   3. Call $scheduler->shouldContinue() between jobs to check deadline
 */
class IosWorkerScheduler
{
    /** @var float Epoch timestamp when the current execution window expires */
    protected float $deadline;

    /** @var int Maximum number of jobs to process per window */
    protected int $maxJobsPerWindow;

    /** @var int Jobs processed in the current window */
    protected int $jobsProcessed = 0;

    /** @var bool Whether to run schedule:run in this window */
    protected bool $runScheduler;

    /** @var float Safety margin in seconds before the deadline */
    protected float $safetyMarginSeconds;

    public function __construct()
    {
        $runtimeSeconds = WorkerConfig::maxBackgroundRuntimeSeconds();
        $this->safetyMarginSeconds = 5.0;
        $this->deadline = microtime(true) + $runtimeSeconds;
        $this->maxJobsPerWindow = (int) ($runtimeSeconds / 2); // ~2s per job estimate
        $this->runScheduler = WorkerConfig::mode() !== 1; // mode 1 = queue only
    }

    /**
     * Execute the iOS background work window.
     *
     * Processes queue jobs and optionally runs the scheduler within the
     * allotted BGProcessingTask time window.
     *
     * @return array Summary of work done: processed, scheduler_ran, duration_ms
     */
    public function execute(): array
    {
        $start = hrtime(true);
        $results = [
            'jobs_processed' => 0,
            'jobs_failed' => 0,
            'scheduler_ran' => false,
            'scheduler_error' => null,
            'stopped_reason' => 'completed',
            'duration_ms' => 0,
        ];

        try {
            // Run scheduler tick first (if enabled), since it may enqueue more jobs
            if ($this->runScheduler && $this->shouldContinue()) {
                $results['scheduler_ran'] = $this->runSchedulerTick($results);
            }

            // Process queue jobs until deadline or queue empty
            $connection = WorkerConfig::connection();
            $queueNames = explode(',', WorkerConfig::queues());
            $manager = app('queue');

            while ($this->shouldContinue() && $results['jobs_processed'] < $this->maxJobsPerWindow) {
                $job = null;

                foreach ($queueNames as $queueName) {
                    $queueName = trim($queueName);
                    $job = $manager->connection($connection)->pop($queueName);
                    if ($job) break;
                }

                if (! $job) {
                    $results['stopped_reason'] = 'queue_empty';
                    break;
                }

                try {
                    $job->fire();

                    if (! $job->isDeleted() && ! $job->isReleased() && ! $job->hasFailed()) {
                        $job->delete();
                    }

                    $results['jobs_processed']++;
                } catch (\Throwable $e) {
                    $results['jobs_failed']++;

                    if (method_exists($job, 'fail')) {
                        $job->fail($e);
                    } else {
                        $job->delete();
                    }

                    WorkerErrorReporter::capture($e, [
                        'job_type' => 'queue',
                        'queue' => $queueName ?? null,
                        'job_class' => $job->resolveName(),
                    ]);
                }
            }

            if ($results['stopped_reason'] === 'completed' && ! $this->shouldContinue()) {
                $results['stopped_reason'] = 'deadline';
            }
        } catch (\Throwable $e) {
            $results['stopped_reason'] = 'error';
            Log::error('[IosWorkerScheduler] Fatal error: ' . $e->getMessage());

            try {
                WorkerErrorReporter::capture($e, ['job_type' => 'ios_worker']);
            } catch (\Throwable $_) {
                // Best-effort
            }
        }

        $results['duration_ms'] = (int) ((hrtime(true) - $start) / 1_000_000);
        return $results;
    }

    /**
     * Check if we should continue processing within the current time window.
     *
     * @return bool True if there's enough time remaining for another job.
     */
    public function shouldContinue(): bool
    {
        return (microtime(true) + $this->safetyMarginSeconds) < $this->deadline;
    }

    /**
     * Get the remaining seconds in the current execution window.
     */
    public function remainingSeconds(): float
    {
        return max(0, $this->deadline - microtime(true));
    }

    /**
     * Run a single scheduler tick.
     *
     * @param array &$results Results array to update with scheduler info
     * @return bool True if the scheduler ran successfully
     */
    protected function runSchedulerTick(array &$results): bool
    {
        try {
            $kernel = app(\Illuminate\Contracts\Console\Kernel::class);
            $output = new \Symfony\Component\Console\Output\NullOutput();
            $exitCode = $kernel->call('schedule:run', ['--no-ansi' => true, '--quiet' => true], $output);
            $kernel->terminate(
                new \Symfony\Component\Console\Input\ArrayInput(['command' => 'schedule:run']),
                $exitCode
            );
            return true;
        } catch (\Throwable $e) {
            $results['scheduler_error'] = $e->getMessage();

            try {
                WorkerErrorReporter::capture($e, ['job_type' => 'scheduler']);
            } catch (\Throwable $_) {
                // Best-effort
            }

            return false;
        }
    }
}
