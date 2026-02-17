<?php

namespace Native\Mobile\Worker;

use Illuminate\Console\Scheduling\CallbackEvent;
use Illuminate\Console\Scheduling\Event;
use Illuminate\Console\Scheduling\Schedule;
use Illuminate\Contracts\Console\Kernel as ConsoleKernel;
use Illuminate\Contracts\Container\Container;
use Throwable;

/**
 * Fork-safe scheduler runner for NativePHP mobile.
 *
 * Laravel's default `schedule:run` command executes each scheduled command
 * event via `Symfony\Component\Process\Process::fromShellCommandline()`,
 * which calls `proc_open()` → `fork()` under the hood.
 *
 * In the NativePHP Android worker pool (ZTS PHP with pthreads), `fork()`
 * deadlocks because the forked child inherits locked mutexes from all
 * other threads (~65 threads) and never reaches `exec()`.
 *
 * This runner replaces the fork-based execution with in-process
 * `Artisan::call()`, which runs each command in the current thread.
 *
 * Safe for both:
 *  - Android ZTS worker threads (no fork)
 *  - iOS BGProcessingTask (single-threaded, still avoids unnecessary fork)
 *
 * @see \Illuminate\Console\Scheduling\ScheduleRunCommand  The standard runner we replace
 * @see \Illuminate\Console\Scheduling\Event::execute()     The fork we avoid
 */
class SafeScheduleRunner
{
    /**
     * Run all due scheduled events in-process without forking.
     *
     * @param  Container  $app  The Laravel application container
     * @return array{ran: int, skipped: int, failed: int, events: list<array>}
     */
    public static function run(Container $app): array
    {
        $schedule = $app->make(Schedule::class);
        $kernel = $app->make(ConsoleKernel::class);

        $results = [
            'ran' => 0,
            'skipped' => 0,
            'failed' => 0,
            'events' => [],
        ];

        foreach ($schedule->dueEvents($app) as $event) {
            // Check event filters (includes cron expression, environments,
            // truth tests, and the withoutOverlapping skip-if-exists check).
            if (! $event->filtersPass($app)) {
                $results['skipped']++;
                continue;
            }

            if ($event instanceof CallbackEvent) {
                // CallbackEvent::execute() runs in-process via Container::call().
                // No proc_open, no fork — safe to call run() directly.
                self::runCallbackEvent($event, $app, $results);
            } else {
                // Command events would normally fork via Process.
                // Execute in-process via Artisan::call() instead.
                self::runCommandEvent($event, $app, $kernel, $results);
            }
        }

        return $results;
    }

    /**
     * Execute a CallbackEvent safely (already in-process).
     */
    protected static function runCallbackEvent(
        CallbackEvent $event,
        Container $app,
        array &$results,
    ): void {
        $description = $event->getSummaryForDisplay();

        try {
            $event->run($app);
            $results['ran']++;
            $results['events'][] = [
                'type' => 'callback',
                'description' => $description,
                'status' => 'ok',
            ];
        } catch (Throwable $e) {
            $results['failed']++;
            $results['events'][] = [
                'type' => 'callback',
                'description' => $description,
                'status' => 'error',
                'error' => $e->getMessage(),
            ];
        }
    }

    /**
     * Execute a command Event in-process via Artisan::call().
     *
     * Replicates the lifecycle of Event::run() without the fork:
     *  1. Create mutex (if withoutOverlapping)
     *  2. Call before callbacks
     *  3. Artisan::call() the command in-process
     *  4. finish() → after callbacks + release mutex
     */
    protected static function runCommandEvent(
        Event $event,
        Container $app,
        ConsoleKernel $kernel,
        array &$results,
    ): void {
        $commandName = self::extractArtisanCommand($event->command);

        if ($commandName === null) {
            // Non-artisan shell command — cannot execute safely in-process.
            // Skip silently; these are unsupported on mobile anyway (no shell).
            $results['skipped']++;
            $results['events'][] = [
                'type' => 'shell',
                'command' => mb_substr($event->command, 0, 100),
                'status' => 'skipped',
                'reason' => 'Non-artisan shell commands cannot run on mobile',
            ];

            return;
        }

        // ── Mutex: acquire lock for withoutOverlapping events ──
        // filtersPass() only checks mutex->exists(). A concurrent tick could
        // pass the filter simultaneously, so we still need mutex->create()
        // to guarantee mutual exclusion (same logic as Event::run()).
        if ($event->withoutOverlapping && ! $event->mutex->create($event)) {
            $results['skipped']++;

            return;
        }

        $exitCode = 1;

        try {
            // Before callbacks (same as Event::start())
            $event->callBeforeCallbacks($app);

            // Execute in-process — the key difference from Event::execute()
            // which would call Process::fromShellCommandline() → fork().
            $exitCode = $kernel->call($commandName);

            $results['ran']++;
            $results['events'][] = [
                'type' => 'command',
                'command' => $commandName,
                'status' => 'ok',
                'exit_code' => $exitCode,
            ];
        } catch (Throwable $e) {
            $exitCode = 1;
            $results['failed']++;
            $results['events'][] = [
                'type' => 'command',
                'command' => $commandName,
                'status' => 'error',
                'error' => $e->getMessage(),
            ];
        }

        // After callbacks + mutex release (same as Event::finish())
        try {
            $event->finish($app, $exitCode);
        } catch (Throwable $e) {
            // finish() should not throw, but don't let it break the runner
            error_log("[SafeScheduleRunner] finish() error for '{$commandName}': {$e->getMessage()}");
        }
    }

    /**
     * Extract the artisan command + arguments from an Event's command string.
     *
     * The command string is built by Application::formatCommandString() and
     * looks like: "'/path/to/php' '/path/to/artisan' app:some-command --flag"
     *
     * We strip the PHP binary and artisan binary prefix to get just the
     * artisan command portion: "app:some-command --flag"
     *
     * @return string|null The artisan command string, or null for non-artisan commands
     */
    protected static function extractArtisanCommand(string $command): ?string
    {
        // Match "artisan" (possibly quoted) followed by the actual command.
        // The artisan path could be '/data/.../artisan' or just 'artisan'.
        if (preg_match('/\bartisan[\'"]?\s+(.+)$/s', $command, $matches)) {
            $artisanCommand = trim($matches[1]);

            // Strip any trailing output redirections that might have been
            // appended (shouldn't be on $event->command, but be defensive).
            $artisanCommand = preg_replace('/\s*[>|&].*$/', '', $artisanCommand);

            // Remove surrounding quotes from the command name if present
            $artisanCommand = trim($artisanCommand, "'\" \t\n\r");

            return $artisanCommand ?: null;
        }

        return null;
    }
}
