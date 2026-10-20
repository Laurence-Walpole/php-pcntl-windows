<?php
declare(strict_types=1);
require __DIR__ . '/lib.php';

echo "06 console control events\n";

// Ctrl+Break -> SIGQUIT (default mapping). The child is started in its own
// process group so GenerateConsoleCtrlEvent() can target it alone.
$code = 'pcntl_async_signals(true); pcntl_signal(SIGQUIT, function($s, $i) { echo "SIGQUIT code={$i["code"]}\n"; exit(3); }); echo "ready\n"; while (true) usleep(10000);';
[$proc, $pid, $pipes] = spawn_php($code, ['create_process_group' => true]);
wait_for_marker($pipes[1], "ready");
ok(sapi_windows_generate_ctrl_event(PHP_WINDOWS_EVENT_CTRL_BREAK, $pid), 'sent CTRL_BREAK to the child process group');
same($pid, pcntl_waitpid($pid, $status), 'child exited');
same(3, pcntl_wexitstatus($status), 'SIGQUIT handler ran and exited 3');
ok(str_contains(drain($pipes[1], 2), 'SIGQUIT code=' . SI_KERNEL), 'siginfo code SI_KERNEL for console events');
proc_close($proc);

// Ctrl+Break with the mapping changed to SIGTERM via pcntl.ctrl_break_signal
$code = 'pcntl_async_signals(true); pcntl_signal(SIGTERM, function() { exit(4); }); echo "ready\n"; while (true) usleep(10000);';
[$proc, $pid, $pipes] = spawn_php($code, ['create_process_group' => true], ['pcntl.ctrl_break_signal' => SIGTERM]);
wait_for_marker($pipes[1], "ready");
sapi_windows_generate_ctrl_event(PHP_WINDOWS_EVENT_CTRL_BREAK, $pid);
pcntl_waitpid($pid, $status);
same(4, pcntl_wexitstatus($status), 'pcntl.ctrl_break_signal=SIGTERM honoured');
proc_close($proc);

// Ctrl+Break ignored with SIG_IGN
$code = 'pcntl_signal(SIGQUIT, SIG_IGN); echo "ready\n"; $t = microtime(true); while (microtime(true) - $t < 1.0) usleep(10000); exit(6);';
[$proc, $pid, $pipes] = spawn_php($code, ['create_process_group' => true]);
wait_for_marker($pipes[1], "ready");
sapi_windows_generate_ctrl_event(PHP_WINDOWS_EVENT_CTRL_BREAK, $pid);
pcntl_waitpid($pid, $status);
same(6, pcntl_wexitstatus($status), 'SIG_IGN swallowed Ctrl+Break');
proc_close($proc);

// Ctrl+Break with default disposition kills the process (Windows default handler)
$code = 'echo "ready\n"; sleep(10); exit(6);';
[$proc, $pid, $pipes] = spawn_php($code, ['create_process_group' => true]);
wait_for_marker($pipes[1], "ready");
sapi_windows_generate_ctrl_event(PHP_WINDOWS_EVENT_CTRL_BREAK, $pid);
pcntl_waitpid($pid, $status);
ok(pcntl_wifsignaled($status) && pcntl_wtermsig($status) === SIGINT, 'default disposition: Windows terminated the child (STATUS_CONTROL_C_EXIT decodes as SIGINT)');
proc_close($proc);

// Ctrl+C -> SIGINT. CTRL_C_EVENT can only be generated for the caller's own
// console, so the child runs in a new console and raises it on itself via FFI.
$ffiDll = dirname(PHP_BINARY) . '\\ext\\php_ffi.dll';
if (!is_file($ffiDll)) {
    echo "  skip - php_ffi.dll not available, Ctrl+C test skipped\n";
} else {
    $code = 'pcntl_async_signals(true); $hit = 0; pcntl_signal(SIGINT, function() use (&$hit) { $hit++; });'
        . '$k = FFI::cdef("int GenerateConsoleCtrlEvent(unsigned int e, unsigned int g);", "kernel32.dll");'
        . 'file_put_contents(getenv("PCNTL_OUT"), "ready");'
        . '$k->GenerateConsoleCtrlEvent(0, 0); $t = microtime(true); while ($hit === 0 && microtime(true) - $t < 3) usleep(10000);'
        . 'file_put_contents(getenv("PCNTL_OUT"), "hit=$hit"); exit($hit === 1 ? 0 : 1);';
    $outFile = sys_get_temp_dir() . '\\pcntl_ctrlc_' . getmypid() . '.txt';
    @unlink($outFile);
    putenv("PCNTL_OUT=$outFile");
    $cmd = [php_bin(), '-n', '-d', 'extension=' . ext_arg(), '-d', "extension=$ffiDll", '-d', 'ffi.enable=1', '-r', $code];
    $proc = proc_open($cmd, [], $p, null, null, ['bypass_shell' => true, 'create_new_console' => true]);
    $pid = proc_get_status($proc)['pid'];
    pcntl_waitpid($pid, $status);
    same(0, pcntl_wexitstatus($status), 'Ctrl+C delivered as SIGINT to the PHP handler');
    same('hit=1', @file_get_contents($outFile), 'handler ran exactly once');
    @unlink($outFile);
    proc_close($proc);
}

finish();
