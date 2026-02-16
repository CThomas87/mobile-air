<?php

namespace Native\Mobile\Events\Worker;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

/**
 * Fired when a scheduler tick (schedule:run) completes.
 */
class SchedulerTickCompleted
{
    use Dispatchable, SerializesModels;

    public function __construct(
        public int $durationMs,
        public int $exitCode = 0,
    ) {}
}
