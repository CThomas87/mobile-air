<?php

namespace Native\Mobile\Events\Worker;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

/**
 * Fired when a background worker job completes successfully.
 *
 * This event is dispatched by the native supervisor via the bridge
 * when a queue job finishes execution with exit code 0.
 */
class JobCompleted
{
    use Dispatchable, SerializesModels;

    public function __construct(
        public string $jobId,
        public string $jobName,
        public int $durationMs,
        public ?string $stdout = null,
    ) {}
}
