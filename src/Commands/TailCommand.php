<?php

namespace Native\Mobile\Commands;

use Illuminate\Console\Command;
use Symfony\Component\Process\Process;

class TailCommand extends Command
{
    protected $signature = 'native:tail
                            {--type=laravel : Log type to tail (laravel, worker)}';

    protected $description = 'Tail Laravel or worker logs from the Android app';

    public function handle(): void
    {
        $appId = config('nativephp.app_id');

        if (empty($appId)) {
            $this->error('🚫 NATIVEPHP_APP_ID is not set');
            $this->line('Please add a NATIVEPHP_APP_ID to your .env file (e.g. com.example.myapp).');

            return;
        }

        $type = $this->option('type');

        match ($type) {
            'worker' => $this->tailAndroid($appId, 'worker'),
            default => $this->tailAndroid($appId, 'laravel'),
        };
    }

    private function tailAndroid(string $appId, string $logType = 'laravel'): void
    {
        $logFile = match ($logType) {
            'worker' => 'app_storage/persisted_data/storage/logs/worker.log',
            default => 'app_storage/persisted_data/storage/logs/laravel.log',
        };

        $label = match ($logType) {
            'worker' => 'worker',
            default => 'Laravel',
        };

        $this->info("🤖 Tailing {$label} logs for app: {$appId}");
        $this->line("Log file: {$logFile}");
        $this->line("Press Ctrl+C to stop...\n");

        $command = [
            'adb', 'shell', 'run-as', $appId, 'tail', '-f', $logFile,
        ];

        $process = new Process($command);
        $process->setTimeout(null);

        try {
            $process->start();

            foreach ($process as $type => $data) {
                if ($process::OUT === $type) {
                    if ($logType === 'worker') {
                        // Worker logs are JSON-line format; pretty-print
                        $this->formatWorkerLogLine($data);
                    } else {
                        $this->line($data, null, null, false);
                    }
                } else {
                    $this->error($data, null, null, false);
                }
            }
        } catch (\Exception $e) {
            $this->error("❌ Error running tail command: {$e->getMessage()}");
            $this->line('Make sure:');
            $this->line('• ADB is installed and in your PATH');
            $this->line('• An Android device/emulator is connected');
            $this->line('• The app is installed and running');
        }
    }

    /**
     * Format a JSON-line worker log entry for readable output.
     */
    private function formatWorkerLogLine(string $data): void
    {
        foreach (explode("\n", trim($data)) as $line) {
            $line = trim($line);
            if (empty($line)) {
                continue;
            }

            $entry = json_decode($line, true);
            if (! $entry) {
                $this->line($line);

                continue;
            }

            $ts = isset($entry['ts']) ? date('H:i:s', (int) ($entry['ts'] / 1000)) : '??:??:??';
            $event = $entry['event'] ?? 'unknown';
            $jobId = $entry['job_id'] ?? '';
            $detail = $entry['detail'] ?? '';

            $color = match ($event) {
                'job_completed', 'supervisor_started' => 'info',
                'job_failed' => 'error',
                'supervisor_stopped' => 'comment',
                default => 'line',
            };

            $formatted = "[{$ts}] {$event}";
            if ($jobId) {
                $formatted .= " job={$jobId}";
            }
            if ($detail) {
                $formatted .= " {$detail}";
            }

            $this->$color($formatted);
        }
    }
}
