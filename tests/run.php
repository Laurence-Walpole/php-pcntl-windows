<?php
/**
 * Runs every tests/NN_*.php script in a fresh php process with the built DLL.
 *
 *   php tests\run.php [path\to\php_pcntl.dll] [--php C:\path\php.exe]
 *
 * Without arguments it uses dist\php_pcntl-<major.minor>-<nts|ts>-<arch>.dll for the running PHP.
 */
declare(strict_types=1);

$args = array_slice($argv, 1);
$php = PHP_BINARY;
$dll = null;
for ($i = 0; $i < count($args); $i++) {
    if ($args[$i] === '--php') {
        $php = $args[++$i];
    } else {
        $dll = $args[$i];
    }
}
if ($dll === null) {
    $dll = sprintf('%s/dist/php_pcntl-%d.%d-%s-%s.dll', dirname(__DIR__), PHP_MAJOR_VERSION, PHP_MINOR_VERSION, PHP_ZTS ? 'ts' : 'nts', PHP_INT_SIZE === 8 ? 'x64' : 'x86');
}
$dll = str_replace('/', '\\', realpath($dll) ?: $dll);
if (!is_file($dll)) {
    fwrite(STDERR, "DLL not found: $dll\n");
    exit(2);
}
echo "Using $php\nExtension $dll\n\n";
putenv("PCNTL_TEST_EXT=$dll");

$failed = 0;
foreach (glob(__DIR__ . '/[0-9][0-9]_*.php') as $test) {
    $extra = [];
    if (str_contains(basename($test), 'fork')) {
        $extra = ['-d', 'pcntl.fork=respawn'];
    }
    $cmd = array_merge([$php, '-n', '-d', "extension=$dll", '-d', 'display_errors=stderr'], $extra, [$test]);
    $proc = proc_open($cmd, [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, null, ['bypass_shell' => true]);
    fclose($pipes[0]);
    stream_set_blocking($pipes[1], false);
    stream_set_blocking($pipes[2], false);
    $out = $err = '';
    $start = microtime(true);
    while (true) {
        $out .= (string) stream_get_contents($pipes[1]);
        $err .= (string) stream_get_contents($pipes[2]);
        $st = proc_get_status($proc);
        if (!$st['running']) {
            $out .= (string) stream_get_contents($pipes[1]);
            $err .= (string) stream_get_contents($pipes[2]);
            break;
        }
        if (microtime(true) - $start > 120) {
            proc_terminate($proc, 9);
            $err .= "\n[test timed out]\n";
            break;
        }
        usleep(20000);
    }
    $code = $st['exitcode'];
    proc_close($proc);
    echo $out;
    if ($err !== '') {
        echo "  stderr:\n" . preg_replace('/^/m', '    ', trim($err)) . "\n";
    }
    if ($code !== 0) {
        $failed++;
        echo "  >>> " . basename($test) . " exited with $code\n";
    }
    echo "\n";
}
echo $failed === 0 ? "ALL TESTS PASSED\n" : "$failed TEST SCRIPT(S) FAILED\n";
exit($failed === 0 ? 0 : 1);
