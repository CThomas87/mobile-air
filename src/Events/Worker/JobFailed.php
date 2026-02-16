<?php

namespace Native\Mobile\Events\Worker;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

/**
 * Fired when a background worker job fails.
 *
 * This event is dispatched by the native supervisor via the bridge
 * when a queue job finishes with a non-zero exit code or exception.
 */
class JobFailed
{
    use Dispatchable, SerializesModels;

    public function __construct(
        public string $jobId,
        public string $jobName,
        public int $durationMs,
        public ?string $error = null,
        public ?string $stderr = null,
    ) {}
}
