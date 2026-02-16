<?php

namespace Native\Mobile\Events\Worker;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

/**
 * Fired when the native supervisor stops and workers are no longer available.
 *
 * This can happen due to:
 * - Explicit stop via System::stopBackgroundWorker()
 * - App being backgrounded (iOS)
 * - Service being killed by the OS
 * - BGProcessingTask expiration
 */
class WorkerStopped
{
    use Dispatchable, SerializesModels;

    public function __construct(
        public int $completedJobs,
        public int $failedJobs,
        public int $uptimeSeconds,
        public string $reason = 'unknown',
    ) {}
}
