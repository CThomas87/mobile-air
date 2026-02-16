<?php

namespace Native\Mobile\Facades;

use Illuminate\Support\Facades\Facade;

/**
 * @method static void flashlight()
 * @method static bool isAndroid()
 * @method static bool isIos()
 * @method static bool isMobile()
 * @method static void appSettings()
 * @method static bool startBackgroundWorker(array $options = [])
 * @method static void stopBackgroundWorker()
 * @method static array workerStatus()
 * @method static bool cancelJob(string $jobId)
 * @method static void requestBatteryExemption()
 * @method static void pushToWebView(string $event, array $data = [])
 */
class System extends Facade
{
    protected static function getFacadeAccessor()
    {
        return \Native\Mobile\System::class;
    }
}
