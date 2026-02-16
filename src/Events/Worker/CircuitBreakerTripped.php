<?php

namespace Native\Mobile\Events\Worker;

use Illuminate\Foundation\Events\Dispatchable;
use Illuminate\Queue\SerializesModels;

/**
 * Fired when the circuit breaker trips due to consecutive worker crashes.
 *
 * This indicates a poison-pill job or systemic issue is causing repeated
 * failures. The worker thread will back off exponentially before retrying.
 */
class CircuitBreakerTripped
{
    use Dispatchable, SerializesModels;

    public function __construct(
        public int $consecutiveCrashes,
        public int $backoffSeconds,
        public ?string $lastError = null,
    ) {}
}
