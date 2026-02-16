<?php

namespace Native\Mobile\Events\Worker;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

/**
 * Fired when the native supervisor starts and workers become available.
 *
 * Dispatched on both foreground start (app launch) and background start
 * (BGProcessingTask on iOS, Foreground Service on Android).
 */
class WorkerStarted
{
    use Dispatchable, SerializesModels;

    public function __construct(
        public int $workerCount,
        public string $queues,
        public int $mode,
        public string $platform,
        public bool $isForeground = true,
    ) {}
}
