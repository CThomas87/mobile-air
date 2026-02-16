<?php

namespace Tests\Feature;

use Illuminate\Database\Schema\Blueprint;
use Illuminate\Support\Facades\DB;
use Illuminate\Support\Facades\Schema;
use Tests\TestCase;

/**
 * SQLite load test harness for concurrent queue operations.
 *
 * Validates that NativePHP's SQLite WAL-mode configuration can handle
 * realistic queue workloads without corruption or excessive latency.
 *
 * M7 requirement: 100 jobs, measure p99 < 50ms on WAL-mode SQLite.
 */
class SqliteQueueLoadTest extends TestCase
{
    private string $dbPath;

    protected function setUp(): void
    {
        parent::setUp();

        $this->dbPath = sys_get_temp_dir().DIRECTORY_SEPARATOR.'nativephp_load_test_'.uniqid().'.sqlite';

        // Create the empty SQLite file (Laravel requires it to exist)
        touch($this->dbPath);

        config([
            'database.connections.load_test' => [
                'driver' => 'sqlite',
                'database' => $this->dbPath,
                'prefix' => '',
                'foreign_key_constraints' => true,
            ],
        ]);

        // Apply WAL-mode PRAGMAs matching production config
        DB::connection('load_test')->statement('PRAGMA journal_mode=WAL');
        DB::connection('load_test')->statement('PRAGMA busy_timeout=5000');
        DB::connection('load_test')->statement('PRAGMA synchronous=NORMAL');
        DB::connection('load_test')->statement('PRAGMA wal_autocheckpoint=100');

        // Create jobs table matching Laravel's queue schema
        Schema::connection('load_test')->create('jobs', function (Blueprint $table) {
            $table->id();
            $table->string('queue')->index();
            $table->longText('payload');
            $table->unsignedTinyInteger('attempts');
            $table->unsignedInteger('reserved_at')->nullable();
            $table->unsignedInteger('available_at');
            $table->unsignedInteger('created_at');
        });

        Schema::connection('load_test')->create('failed_jobs', function (Blueprint $table) {
            $table->id();
            $table->string('uuid')->unique();
            $table->text('connection');
            $table->text('queue');
            $table->longText('payload');
            $table->longText('exception');
            $table->timestamp('failed_at')->useCurrent();
        });
    }

    protected function tearDown(): void
    {
        DB::disconnect('load_test');

        if (file_exists($this->dbPath)) {
            @unlink($this->dbPath);
            @unlink($this->dbPath.'-wal');
            @unlink($this->dbPath.'-shm');
        }

        parent::tearDown();
    }

    public function test_wal_mode_is_active(): void
    {
        $result = DB::connection('load_test')->selectOne('PRAGMA journal_mode');

        $this->assertEquals('wal', $result->journal_mode);
    }

    public function test_busy_timeout_is_configured(): void
    {
        $result = DB::connection('load_test')->select('PRAGMA busy_timeout');
        $row = (array) $result[0];
        $value = reset($row);

        $this->assertEquals(5000, $value);
    }

    public function test_insert_100_jobs_under_50ms_p99(): void
    {
        $latencies = [];

        for ($i = 0; $i < 100; $i++) {
            $start = hrtime(true);

            DB::connection('load_test')->table('jobs')->insert([
                'queue' => 'default',
                'payload' => json_encode([
                    'uuid' => fake()->uuid(),
                    'displayName' => 'App\\Jobs\\TestJob',
                    'job' => 'Illuminate\\Queue\\CallQueuedHandler@call',
                    'data' => ['command' => base64_encode(str_repeat('x', 256))],
                ]),
                'attempts' => 0,
                'reserved_at' => null,
                'available_at' => time(),
                'created_at' => time(),
            ]);

            $latencies[] = (hrtime(true) - $start) / 1_000_000; // Convert to ms
        }

        sort($latencies);
        $p99 = $latencies[(int) floor(count($latencies) * 0.99)];
        $p50 = $latencies[(int) floor(count($latencies) * 0.50)];
        $avg = array_sum($latencies) / count($latencies);

        // p99 should be under 50ms for WAL-mode SQLite
        $this->assertLessThan(50, $p99, "p99 insert latency was {$p99}ms (limit: 50ms)");

        // Sanity check: avg should be much lower
        $this->assertLessThan(20, $avg, "Average insert latency was {$avg}ms");

        // Verify all 100 jobs are present
        $count = DB::connection('load_test')->table('jobs')->count();
        $this->assertEquals(100, $count);
    }

    public function test_concurrent_read_write_pattern(): void
    {
        // Insert 50 jobs as "existing workload"
        for ($i = 0; $i < 50; $i++) {
            DB::connection('load_test')->table('jobs')->insert([
                'queue' => 'default',
                'payload' => json_encode(['job' => "existing_{$i}"]),
                'attempts' => 0,
                'reserved_at' => null,
                'available_at' => time(),
                'created_at' => time(),
            ]);
        }

        // Simulate worker claiming + new dispatch interleaved
        $claimLatencies = [];
        $insertLatencies = [];

        for ($i = 0; $i < 50; $i++) {
            // Claim oldest unclaimed job (simulates worker pop)
            $start = hrtime(true);
            DB::connection('load_test')->table('jobs')
                ->whereNull('reserved_at')
                ->where('available_at', '<=', time())
                ->orderBy('id')
                ->limit(1)
                ->update(['reserved_at' => time(), 'attempts' => 1]);
            $claimLatencies[] = (hrtime(true) - $start) / 1_000_000;

            // Insert a new job (simulates dispatch while worker is active)
            $start = hrtime(true);
            DB::connection('load_test')->table('jobs')->insert([
                'queue' => 'default',
                'payload' => json_encode(['job' => "new_{$i}"]),
                'attempts' => 0,
                'reserved_at' => null,
                'available_at' => time(),
                'created_at' => time(),
            ]);
            $insertLatencies[] = (hrtime(true) - $start) / 1_000_000;
        }

        sort($claimLatencies);
        sort($insertLatencies);

        $claimP99 = $claimLatencies[(int) floor(count($claimLatencies) * 0.99)];
        $insertP99 = $insertLatencies[(int) floor(count($insertLatencies) * 0.99)];

        $this->assertLessThan(50, $claimP99, "p99 claim latency: {$claimP99}ms");
        $this->assertLessThan(50, $insertP99, "p99 insert-during-claim latency: {$insertP99}ms");

        // All 50 original jobs should be claimed
        $claimed = DB::connection('load_test')->table('jobs')
            ->whereNotNull('reserved_at')
            ->count();
        $this->assertEquals(50, $claimed);

        // 50 new jobs should be unclaimed
        $unclaimed = DB::connection('load_test')->table('jobs')
            ->whereNull('reserved_at')
            ->count();
        $this->assertEquals(50, $unclaimed);
    }

    public function test_delete_after_process_pattern(): void
    {
        // Insert 100 jobs
        for ($i = 0; $i < 100; $i++) {
            DB::connection('load_test')->table('jobs')->insert([
                'queue' => 'default',
                'payload' => json_encode(['job' => "process_{$i}"]),
                'attempts' => 0,
                'reserved_at' => null,
                'available_at' => time(),
                'created_at' => time(),
            ]);
        }

        // Simulate claim → process → delete cycle
        $cycleLatencies = [];

        for ($i = 0; $i < 100; $i++) {
            $start = hrtime(true);

            $job = DB::connection('load_test')->table('jobs')
                ->whereNull('reserved_at')
                ->orderBy('id')
                ->first();

            if ($job) {
                DB::connection('load_test')->table('jobs')
                    ->where('id', $job->id)
                    ->update(['reserved_at' => time()]);

                // Simulate minimal processing
                usleep(100); // 0.1ms

                DB::connection('load_test')->table('jobs')
                    ->where('id', $job->id)
                    ->delete();
            }

            $cycleLatencies[] = (hrtime(true) - $start) / 1_000_000;
        }

        sort($cycleLatencies);
        $p99 = $cycleLatencies[(int) floor(count($cycleLatencies) * 0.99)];

        $this->assertLessThan(100, $p99, "p99 claim-process-delete cycle: {$p99}ms");

        // All jobs should be processed
        $remaining = DB::connection('load_test')->table('jobs')->count();
        $this->assertEquals(0, $remaining);
    }

    public function test_queue_isolation_under_load(): void
    {
        $queues = ['default', 'notifications', 'emails'];

        // Insert 30 jobs per queue (90 total)
        foreach ($queues as $queue) {
            for ($i = 0; $i < 30; $i++) {
                DB::connection('load_test')->table('jobs')->insert([
                    'queue' => $queue,
                    'payload' => json_encode(['job' => "{$queue}_{$i}"]),
                    'attempts' => 0,
                    'reserved_at' => null,
                    'available_at' => time(),
                    'created_at' => time(),
                ]);
            }
        }

        $total = DB::connection('load_test')->table('jobs')->count();
        $this->assertEquals(90, $total);

        // Each queue should have exactly 30
        foreach ($queues as $queue) {
            $count = DB::connection('load_test')->table('jobs')
                ->where('queue', $queue)
                ->count();
            $this->assertEquals(30, $count, "Queue '{$queue}' should have 30 jobs");
        }

        // Process only 'default' queue — others should be untouched
        for ($i = 0; $i < 30; $i++) {
            $job = DB::connection('load_test')->table('jobs')
                ->where('queue', 'default')
                ->whereNull('reserved_at')
                ->orderBy('id')
                ->first();

            if ($job) {
                DB::connection('load_test')->table('jobs')->where('id', $job->id)->delete();
            }
        }

        // Default empty, others untouched
        $this->assertEquals(0, DB::connection('load_test')->table('jobs')->where('queue', 'default')->count());
        $this->assertEquals(30, DB::connection('load_test')->table('jobs')->where('queue', 'notifications')->count());
        $this->assertEquals(30, DB::connection('load_test')->table('jobs')->where('queue', 'emails')->count());
    }

    public function test_failed_job_recording_under_load(): void
    {
        $latencies = [];

        for ($i = 0; $i < 50; $i++) {
            $start = hrtime(true);

            DB::connection('load_test')->table('failed_jobs')->insert([
                'uuid' => fake()->uuid(),
                'connection' => 'database',
                'queue' => 'default',
                'payload' => json_encode(['job' => "failed_{$i}"]),
                'exception' => "RuntimeException: Test failure {$i}\n#0 app/Jobs/TestJob.php(25)\n#1 ...",
                'failed_at' => now(),
            ]);

            $latencies[] = (hrtime(true) - $start) / 1_000_000;
        }

        sort($latencies);
        $p99 = $latencies[(int) floor(count($latencies) * 0.99)];

        $this->assertLessThan(50, $p99, "p99 failed_job insert: {$p99}ms");
        $this->assertEquals(50, DB::connection('load_test')->table('failed_jobs')->count());
    }
}
