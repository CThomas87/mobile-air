<?php

define('ARTISAN_START', microtime(true));

error_log('[ARTISAN-DIAG] === artisan.php START ===');
error_log('[ARTISAN-DIAG] __DIR__=' . __DIR__);
error_log('[ARTISAN-DIAG] $_SERVER[argv]=' . json_encode($_SERVER['argv'] ?? 'NOT_SET'));
error_log('[ARTISAN-DIAG] $_SERVER[argc]=' . ($_SERVER['argc'] ?? 'NOT_SET'));
error_log('[ARTISAN-DIAG] register_argc_argv=' . ini_get('register_argc_argv'));

if (! isset($argv)) {
    global $argv;
    $argv = $_SERVER['argv'] ?? ['artisan'];
}
error_log('[ARTISAN-DIAG] $argv=' . json_encode($argv));

require __DIR__.'/vendor/autoload.php';
error_log('[ARTISAN-DIAG] autoload loaded OK');

$app = require_once __DIR__.'/bootstrap/app.php';
error_log('[ARTISAN-DIAG] app bootstrapped OK');

use Illuminate\Contracts\Console\Kernel;
use Symfony\Component\Console\Input\ArgvInput;
use Symfony\Component\Console\Output\StreamOutput;

// ✅ Redirect output to php://output so ub_write captures it
$stdout = fopen('php://output', 'w');
$output = new StreamOutput($stdout);

$kernel = $app->make(Kernel::class);
error_log('[ARTISAN-DIAG] kernel created, calling handle()...');

$input = new ArgvInput;
error_log('[ARTISAN-DIAG] ArgvInput tokens=' . json_encode($input->__toString()));

$status = $kernel->handle(
    $input,
    $output
);

error_log('[ARTISAN-DIAG] handle() returned status=' . $status);

// Flush output stream before terminate/exit
if (is_resource($stdout)) {
    fflush($stdout);
}

$kernel->terminate($input, $status);
error_log('[ARTISAN-DIAG] === artisan.php END status=' . $status . ' ===');

exit($status);
