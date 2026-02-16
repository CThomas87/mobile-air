<?php

namespace Tests\Unit;

use Native\Mobile\Worker\WorkerConfig;
use Tests\TestCase;

class WorkerConfigTest extends TestCase
{
    protected function tearDown(): void
    {
        putenv('NATIVEPHP_RUNNING');
        putenv('NATIVEPHP_PLATFORM');
        putenv('NATIVEPHP_WORKER_COUNT');
        putenv('NATIVEPHP_CIRCUIT_BREAKER_THRESHOLD');
        putenv('NATIVEPHP_CIRCUIT_BREAKER_BACKOFF');
        putenv('NATIVEPHP_WORKER_MEMORY_LIMIT');
        putenv('NATIVEPHP_OVERRIDE_SYNC_DRIVER');
        putenv('NATIVEPHP_WORKER_IMMEDIATE_DISPATCH');
        putenv('NATIVEPHP_MAX_BG_RUNTIME_IOS');
        putenv('NATIVEPHP_MAX_BG_RUNTIME_ANDROID');

        parent::tearDown();
    }

    public function test_is_native_accepts_boolean_like_values(): void
    {
        putenv('NATIVEPHP_RUNNING=true');
        $this->assertTrue(WorkerConfig::isNative());

        putenv('NATIVEPHP_RUNNING=1');
        $this->assertTrue(WorkerConfig::isNative());

        putenv('NATIVEPHP_RUNNING=on');
        $this->assertTrue(WorkerConfig::isNative());

        putenv('NATIVEPHP_RUNNING=false');
        $this->assertFalse(WorkerConfig::isNative());

        putenv('NATIVEPHP_RUNNING=0');
        $this->assertFalse(WorkerConfig::isNative());
    }

    public function test_platform_detection_is_case_insensitive(): void
    {
        putenv('NATIVEPHP_PLATFORM=IOS');
        $this->assertTrue(WorkerConfig::isIos());
        $this->assertFalse(WorkerConfig::isAndroid());

        putenv('NATIVEPHP_PLATFORM=Android');
        $this->assertTrue(WorkerConfig::isAndroid());
        $this->assertFalse(WorkerConfig::isIos());
    }

    public function test_worker_count_enforces_hard_cap(): void
    {
        $this->app['config']->set('nativephp-worker.worker_count', 10);
        $this->app['config']->set('nativephp-worker.max_worker_count', 4);

        $count = WorkerConfig::workerCount();

        $this->assertLessThanOrEqual(WorkerConfig::MAX_WORKER_HARD_CAP, $count);
        $this->assertEquals(4, $count);
    }

    public function test_worker_count_minimum_is_one(): void
    {
        $this->app['config']->set('nativephp-worker.worker_count', 0);

        $this->assertEquals(1, WorkerConfig::workerCount());
    }

    public function test_ios_max_background_runtime_is_25_seconds(): void
    {
        putenv('NATIVEPHP_PLATFORM=ios');

        $runtime = WorkerConfig::maxBackgroundRuntimeSeconds();

        $this->assertEquals(25, $runtime);
    }

    public function test_android_max_background_runtime_is_6_hours(): void
    {
        putenv('NATIVEPHP_PLATFORM=android');

        $runtime = WorkerConfig::maxBackgroundRuntimeSeconds();

        $this->assertEquals(6 * 60 * 60, $runtime);
    }

    public function test_circuit_breaker_defaults(): void
    {
        $this->assertEquals(3, WorkerConfig::circuitBreakerThreshold());
        $this->assertEquals(5, WorkerConfig::circuitBreakerBackoff());
    }

    public function test_circuit_breaker_respects_config_override(): void
    {
        $this->app['config']->set('nativephp-worker.circuit_breaker_threshold', 5);
        $this->app['config']->set('nativephp-worker.circuit_breaker_backoff', 10);

        $this->assertEquals(5, WorkerConfig::circuitBreakerThreshold());
        $this->assertEquals(10, WorkerConfig::circuitBreakerBackoff());
    }

    public function test_memory_limit_default_is_64m(): void
    {
        $this->assertEquals('64M', WorkerConfig::memoryLimit());
    }

    public function test_override_sync_driver_defaults_to_true(): void
    {
        $this->assertTrue(WorkerConfig::overrideSyncDriver());
    }

    public function test_immediate_dispatch_defaults_to_true(): void
    {
        $this->assertTrue(WorkerConfig::immediateDispatch());
    }

    public function test_log_enabled_defaults_to_true(): void
    {
        $this->assertTrue(WorkerConfig::logEnabled());
    }

    public function test_default_connection_is_database(): void
    {
        $this->app['config']->set('queue.default', 'database');

        $this->assertEquals('database', WorkerConfig::connection());
    }

    public function test_default_queues_is_default(): void
    {
        $this->assertEquals('default', WorkerConfig::queues());
    }

    public function test_max_worker_hard_cap_is_4(): void
    {
        $this->assertEquals(4, WorkerConfig::MAX_WORKER_HARD_CAP);
    }
}
