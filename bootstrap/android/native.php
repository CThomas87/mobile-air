<?php

use Illuminate\Contracts\Http\Kernel;
use Illuminate\Http\Request;

$_timing = ['start' => microtime(true)];

$storagePath = $_SERVER['LARAVEL_STORAGE_PATH'] ?? null;
if (!$storagePath) {
    $storagePath = dirname(__DIR__, 3).'/storage';
}
$nativePhpLogFile = rtrim($storagePath, '/').'/logs/native-http-bootstrap.log';
$nativePhpLog = static function (string $message) use ($nativePhpLogFile): void {
    @error_log(date('c').' '.$message.PHP_EOL, 3, $nativePhpLogFile);
};

$maxExecutionSeconds = (int) (getenv('NATIVEPHP_HTTP_MAX_EXECUTION_SECONDS') ?: '0');
if ($maxExecutionSeconds > 0) {
    @ini_set('max_execution_time', (string) $maxExecutionSeconds);
    @set_time_limit($maxExecutionSeconds);
} else {
    @ini_set('max_execution_time', '0');
    @set_time_limit(0);
}

$defaultSocketTimeout = (int) (getenv('NATIVEPHP_DEFAULT_SOCKET_TIMEOUT_SECONDS') ?: '10');
if ($defaultSocketTimeout > 0) {
    @ini_set('default_socket_timeout', (string) $defaultSocketTimeout);
}

$nativePhpLog('stage=bootstrap_start max_execution='.$maxExecutionSeconds.' socket_timeout='.$defaultSocketTimeout.' uri='.($_SERVER['REQUEST_URI'] ?? '(null)'));

// Capture OPcache status early (will be logged later with timing)
$buildOpcacheInfo = static function (): string {
    $engineInfo = (string) ($_SERVER['NATIVEPHP_OPCACHE_ENGINE'] ?? $_ENV['NATIVEPHP_OPCACHE_ENGINE'] ?? '');
    $engineSuffix = $engineInfo !== '' ? ',engine=' . $engineInfo : '';

    $opcacheLoaded = extension_loaded('Zend OPcache') || extension_loaded('opcache');
    if (! $opcacheLoaded) {
        return 'NOT_AVAILABLE' . $engineSuffix;
    }

    $opcacheEnabled = filter_var(ini_get('opcache.enable'), FILTER_VALIDATE_BOOLEAN)
        || filter_var(ini_get('opcache.enable_cli'), FILTER_VALIDATE_BOOLEAN);

    if (! function_exists('opcache_get_status')) {
        return 'AVAILABLE,status=FN_MISSING,ini=' . ($opcacheEnabled ? 'YES' : 'NO') . $engineSuffix;
    }

    $opcacheStatus = @opcache_get_status(false);
    if (! is_array($opcacheStatus)) {
        return 'AVAILABLE,status=UNAVAILABLE,ini=' . ($opcacheEnabled ? 'YES' : 'NO') . $engineSuffix;
    }

    $statusEnabled = (bool) ($opcacheStatus['opcache_enabled'] ?? false);
    $statistics = $opcacheStatus['opcache_statistics'] ?? [];
    $memory = $opcacheStatus['memory_usage'] ?? [];
    $cachedScripts = (int) ($statistics['num_cached_scripts'] ?? 0);
    $hits = (int) ($statistics['hits'] ?? 0);
    $usedMemoryMb = isset($memory['used_memory']) ? round(((int) $memory['used_memory']) / 1048576, 1) : 0;

    return 'AVAILABLE,enabled=' . ($statusEnabled ? 'YES' : 'NO')
        . ',ini=' . ($opcacheEnabled ? 'YES' : 'NO')
        . ',cached=' . $cachedScripts
        . ',hits=' . $hits
        . ',mem=' . $usedMemoryMb . 'MB'
        . $engineSuffix;
};

$_opcacheInfo = $buildOpcacheInfo();

define('LARAVEL_START', microtime(true));

require $_SERVER['COMPOSER_AUTOLOADER_PATH'];
$_timing['autoload'] = microtime(true);

$app = require_once $_SERVER['LARAVEL_BOOTSTRAP_PATH'].'/app.php';
$_timing['bootstrap'] = microtime(true);

/*
|--------------------------------------------------------------------------
| Normalize incoming environment
|--------------------------------------------------------------------------
| We want to make sure Laravel sees:
| - full query params (even for POSTs)
| - real cookies (without mangling)
| - raw input untouched (for JSON & file uploads)
|--------------------------------------------------------------------------
*/

// ✅ Preserve cookies as-is
if (isset($_SERVER['HTTP_COOKIE'])) {
    $cookiePairs = explode('; ', $_SERVER['HTTP_COOKIE']);
    $cookies = [];
    foreach ($cookiePairs as $pair) {
        $parts = explode('=', $pair, 2);
        if (count($parts) === 2) {
            $cookies[$parts[0]] = urldecode($parts[1]);
        }
    }
    $_COOKIE = $cookies;
}

// ✅ Preserve query params for ALL request methods
if (isset($_SERVER['QUERY_STRING']) && $_SERVER['QUERY_STRING'] !== '') {
    parse_str($_SERVER['QUERY_STRING'], $_GET);
}

// ✅ Let Laravel handle POST parsing itself (important for multipart/form-data)
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    // Don't manually parse php://input — Laravel will handle JSON/form-data properly
}

/*
|--------------------------------------------------------------------------
| Handle Laravel request
|--------------------------------------------------------------------------
*/

$kernel = $app->make(Kernel::class);
$_timing['kernel'] = microtime(true);

try {
    $nativePhpLog('stage=request_capture_start');
    error_log('PerfTiming: PHP stage=request_capture_start');
    $request = Request::capture();
    $_timing['capture'] = microtime(true);

    $nativePhpLog('stage=kernel_bootstrap_start');
    error_log('PerfTiming: PHP stage=kernel_bootstrap_start');
    $kernel->bootstrap();
    $_timing['kernel_bootstrap'] = microtime(true);

    $nativePhpLog('stage=handle_start');
    error_log('PerfTiming: PHP stage=handle_start');
    $response = $kernel->handle($request);
    $_timing['handle'] = microtime(true);

    $shouldTerminate = filter_var(
        getenv('NATIVEPHP_HTTP_TERMINATE') ?: 'false',
        FILTER_VALIDATE_BOOLEAN
    );

    if ($shouldTerminate) {
        $nativePhpLog('stage=terminate_start enabled=true');
        error_log('PerfTiming: PHP stage=terminate_start enabled=true');
        $kernel->terminate($request, $response);
    } else {
        $nativePhpLog('stage=terminate_skipped enabled=false');
        error_log('PerfTiming: PHP stage=terminate_skipped enabled=false');
    }
    $_timing['terminate'] = microtime(true);

    // Refresh OPcache status late in the request to capture real cache stats when available.
    $_opcacheInfo = $buildOpcacheInfo();

    // Calculate timing breakdown (in ms)
    $autoloadMs = round(($_timing['autoload'] - $_timing['start']) * 1000, 1);
    $bootstrapMs = round(($_timing['bootstrap'] - $_timing['autoload']) * 1000, 1);
    $kernelMs = round(($_timing['kernel'] - $_timing['bootstrap']) * 1000, 1);
    $captureMs = round(($_timing['capture'] - $_timing['kernel']) * 1000, 1);
    $kernelBootMs = round(($_timing['kernel_bootstrap'] - $_timing['capture']) * 1000, 1);
    $handleMs = round(($_timing['handle'] - $_timing['kernel_bootstrap']) * 1000, 1);
    $terminateMs = round(($_timing['terminate'] - $_timing['handle']) * 1000, 1);
    $totalMs = round(($_timing['terminate'] - $_timing['start']) * 1000, 1);

    // Log timing via error_log (shows in logcat)
    $nativePhpLog("stage=timing_summary opcache={$_opcacheInfo} autoload={$autoloadMs}ms bootstrap={$bootstrapMs}ms kernel={$kernelMs}ms capture={$captureMs}ms kernel_boot={$kernelBootMs}ms handle={$handleMs}ms terminate={$terminateMs}ms total={$totalMs}ms");
    error_log("PerfTiming: PHP opcache={$_opcacheInfo} autoload={$autoloadMs}ms bootstrap={$bootstrapMs}ms kernel={$kernelMs}ms capture={$captureMs}ms kernel_boot={$kernelBootMs}ms handle={$handleMs}ms terminate={$terminateMs}ms TOTAL={$totalMs}ms");

    // Send headers and body manually (for your bridge)
    @ignore_user_abort(true);
    $code = $response->getStatusCode();
    $status = \Symfony\Component\HttpFoundation\Response::$statusTexts[$code] ?? 'OK';

    $headerLines = [
        "HTTP/1.1 {$code} {$status}",
        "X-PHP-Timing: opcache={$_opcacheInfo},autoload={$autoloadMs}ms,bootstrap={$bootstrapMs}ms,kernel_boot={$kernelBootMs}ms,handle={$handleMs}ms,total={$totalMs}ms",
    ];

    foreach ($response->headers->all() as $name => $values) {
        foreach ($values as $value) {
            $headerLines[] = "{$name}: {$value}";
        }
    }

    ob_start();
    $response->sendContent();
    $body = (string) ob_get_clean();

    $payload = implode("\r\n", $headerLines)."\r\n\r\n".$body;
    echo $payload;

} catch (Throwable $e) {
    $errorId = uniqid('nativephp_', true);
    $nativePhpLog('stage=exception id='.$errorId.' type='.get_class($e).' message='.$e->getMessage());
    $nativePhpLog('stage=exception_trace id='.$errorId.' trace='.$e->getTraceAsString());

    echo "HTTP/1.1 500 Internal Server Error\r\n";
    echo "Content-Type: text/html; charset=UTF-8\r\n\r\n";
    echo "<html><body><h2>Application error</h2><p>Reference: {$errorId}</p></body></html>";
}
