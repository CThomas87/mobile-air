<?php

namespace Native\Mobile\Commands;

use Illuminate\Console\Command;
use Native\Mobile\Worker\WorkerConfig;

/**
 * WorkerCommand — Local dev simulator for the native worker system.
 *
 * Simulates what the native supervisor does on-device: runs queue:work
 * and schedule:run in the foreground with the same configuration.
 * Useful for testing worker behavior before deploying to a device.
 *
 * Usage:
 *   php artisan native:worker                  # Run queue workers + scheduler
 *   php artisan native:worker --queue-only     # Run queue workers only
 *   php artisan native:worker --scheduler-only # Run scheduler only
 *   php artisan native:worker --status         # Show worker configuration
 */
class WorkerCommand extends Command
{
    protected $signature = 'native:worker
                            {--queue-only : Run queue workers only (no scheduler)}
                            {--scheduler-only : Run scheduler only (no queue workers)}
                            {--status : Show current worker configuration and exit}
                            {--workers= : Override worker count}
                            {--queues= : Override queue names}';

    protected $description = 'Simulate the native PHP worker system locally for development';

    public function handle(): int
    {
        if ($this->option('status')) {
            return $this->showStatus();
        }

        $this->info('🚀 NativePHP Worker Simulator');
        $this->line('');

        $this->showConfig();
        $this->line('');

        $mode = $this->resolveMode();
        $modeLabel = match ($mode) {
            0 => 'all (queue + scheduler)',
            1 => 'queue only',
            2 => 'scheduler only',
            default => 'unknown',
        };

        $this->info("Starting in mode: {$modeLabel}");
        $this->line('Press Ctrl+C to stop.');
        $this->line('');

        // Run the appropriate artisan command based on mode
        if ($mode === 0 || $mode === 1) {
            $this->runQueueWorker();
        }

        if ($mode === 0 || $mode === 2) {
            $this->runScheduler();
        }

        return self::SUCCESS;
    }

    protected function showStatus(): int
    {
        $this->info('📊 NativePHP Worker Configuration');
        $this->line('');
        $this->showConfig();

        return self::SUCCESS;
    }

    protected function showConfig(): void
    {
        $config = [
            ['Connection', WorkerConfig::connection()],
            ['Queues', $this->option('queues') ?: WorkerConfig::queues()],
            ['Worker Count', $this->option('workers') ?: WorkerConfig::workerCount()],
            ['Mode', $this->resolveMode()],
            ['Scheduler Interval', WorkerConfig::schedulerIntervalSeconds() . 's'],
            ['Queue Poll Interval', WorkerConfig::queuePollIntervalSeconds() . 's'],
            ['Memory Limit', WorkerConfig::memoryLimit()],
            ['Circuit Breaker', WorkerConfig::circuitBreakerThreshold() . ' crashes / ' . WorkerConfig::circuitBreakerBackoff() . 's backoff'],
            ['Override Sync Driver', WorkerConfig::overrideSyncDriver() ? 'yes' : 'no'],
            ['Immediate Dispatch', WorkerConfig::immediateDispatch() ? 'yes' : 'no'],
            ['Log Enabled', WorkerConfig::logEnabled() ? 'yes' : 'no'],
            ['Platform', WorkerConfig::isIos() ? 'iOS' : (WorkerConfig::isAndroid() ? 'Android' : 'local')],
            ['Max BG Runtime (Android)', WorkerConfig::isIos() ? 'N/A' : (WorkerConfig::maxBackgroundRuntimeSeconds() . 's')],
            ['Max BG Runtime (iOS)', WorkerConfig::isAndroid() ? 'N/A' : '25s'],
        ];

        $this->table(['Setting', 'Value'], $config);
    }

    protected function resolveMode(): int
    {
        if ($this->option('queue-only')) {
            return 1;
        }

        if ($this->option('scheduler-only')) {
            return 2;
        }

        return WorkerConfig::mode();
    }

    protected function runQueueWorker(): void
    {
        $workers = (int) ($this->option('workers') ?: WorkerConfig::workerCount());
        $queues = $this->option('queues') ?: WorkerConfig::queues();
        $connection = WorkerConfig::connection();

        $this->info("▶ Starting queue:work (connection={$connection}, queues={$queues}, workers=simulated as 1 in dev)");

        $this->call('queue:work', [
            'connection' => $connection,
            '--queue' => $queues,
            '--memory' => (int) WorkerConfig::memoryLimit(),
            '--tries' => 3,
            '--timeout' => 60,
        ]);
    }

    protected function runScheduler(): void
    {
        $this->info('▶ Running schedule:run');

        $this->call('schedule:run');
    }
}
