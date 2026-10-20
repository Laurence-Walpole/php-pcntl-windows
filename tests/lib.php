<?php
// Tiny assertion helpers shared by the test scripts (no external dependencies).

declare(strict_types=1);

$GLOBALS['__tests_failed'] = 0;
$GLOBALS['__tests_passed'] = 0;

function ok(bool $cond, string $what): void
{
    if ($cond) {
        $GLOBALS['__tests_passed']++;
        echo "  ok   - $what\n";
    } else {
        $GLOBALS['__tests_failed']++;
        echo "  FAIL - $what\n";
    }
}

function same(mixed $expected, mixed $actual, string $what): void
{
    ok($expected === $actual, $what . ' (expected ' . var_export($expected, true) . ', got ' . var_export($actual, true) . ')');
}

function finish(): never
{
    $f = $GLOBALS['__tests_failed'];
    $p = $GLOBALS['__tests_passed'];
    echo $f === 0 ? "PASS ($p assertions)\n" : "FAILED ($f of " . ($f + $p) . " assertions)\n";
    exit($f === 0 ? 0 : 1);
}

function php_bin(): string
{
    return PHP_BINARY;
}

function ext_arg(): string
{
    // the extension this process was started with, so children load the same DLL
    return (string) getenv('PCNTL_TEST_EXT');
}

/**
 * Start a child php process running $code (with the extension loaded) and return [proc, pid, pipes].
 */
function spawn_php(string $code, array $extraOptions = [], array $extraIni = []): array
{
    $cmd = [php_bin(), '-n', '-d', 'extension=' . ext_arg()];
    foreach ($extraIni as $k => $v) {
        $cmd[] = '-d';
        $cmd[] = "$k=$v";
    }
    $cmd[] = '-r';
    $cmd[] = $code;
    $desc = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $proc = proc_open($cmd, $desc, $pipes, null, null, $extraOptions + ['bypass_shell' => true]);
    if (!is_resource($proc)) {
        throw new RuntimeException('proc_open failed');
    }
    $status = proc_get_status($proc);
    return [$proc, $status['pid'], $pipes];
}

/** Wait until the child has printed $marker on stdout (it must flush!). */
function wait_for_marker($pipe, string $marker, float $timeout = 10.0): string
{
    $deadline = microtime(true) + $timeout;
    $buf = '';
    stream_set_blocking($pipe, false);
    while (microtime(true) < $deadline) {
        $chunk = fread($pipe, 8192);
        if ($chunk !== false && $chunk !== '') {
            $buf .= $chunk;
            if (str_contains($buf, $marker)) {
                return $buf;
            }
        } else {
            usleep(20000);
        }
    }
    throw new RuntimeException("timed out waiting for marker '$marker'; got: $buf");
}

function drain($pipe, float $timeout = 10.0): string
{
    $deadline = microtime(true) + $timeout;
    $buf = '';
    stream_set_blocking($pipe, false);
    while (microtime(true) < $deadline) {
        $chunk = fread($pipe, 8192);
        if ($chunk === false || $chunk === '') {
            if (feof($pipe)) {
                break;
            }
            usleep(20000);
            continue;
        }
        $buf .= $chunk;
    }
    return $buf;
}
