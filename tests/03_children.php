<?php
declare(strict_types=1);
require __DIR__ . '/lib.php';

echo "03 children: waitpid / posix_kill / stop & continue / SIGCHLD\n";

$chld = [];
pcntl_signal(SIGCHLD, function (int $s, mixed $info) use (&$chld) { $chld[] = $info; });

// 1. waitpid() on a process we did not fork (adopted pid) + exit status
[$proc, $pid, $pipes] = spawn_php('usleep(200000); exit(3);');
$t = microtime(true);
same($pid, pcntl_waitpid($pid, $status), 'waitpid returns the pid');
ok(microtime(true) - $t >= 0.15, 'waitpid blocked until the child exited');
ok(pcntl_wifexited($status), 'child exited normally');
same(3, pcntl_wexitstatus($status), 'exit status 3');
proc_close($proc);
pcntl_signal_dispatch();
same(1, count($chld), 'SIGCHLD delivered once');
same($pid, $chld[0]['pid'], 'SIGCHLD info.pid');
same(CLD_EXITED, $chld[0]['code'], 'SIGCHLD info.code CLD_EXITED');
same(3, $chld[0]['status'], 'SIGCHLD info.status');

// 2. WNOHANG
$chld = [];
[$proc, $pid, $pipes] = spawn_php('usleep(400000); exit(0);');
same(0, pcntl_waitpid($pid, $status, WNOHANG), 'WNOHANG returns 0 while running');
usleep(700000);
same($pid, pcntl_waitpid($pid, $status, WNOHANG), 'WNOHANG returns pid once exited');
same(0, pcntl_wexitstatus($status), 'exit status 0');
same(-1, pcntl_waitpid($pid, $status, WNOHANG), 'second waitpid on a reaped pid fails');
same(PCNTL_ECHILD, pcntl_get_last_error(), 'with ECHILD');
proc_close($proc);

// 3. SIGKILL -> wifsignaled
[$proc, $pid, $pipes] = spawn_php('sleep(30);');
usleep(300000);
ok(posix_kill($pid, 0), 'posix_kill(pid, 0) says the process exists');
ok(posix_kill($pid, SIGKILL), 'posix_kill(pid, SIGKILL)');
same($pid, pcntl_waitpid($pid, $status), 'waitpid after kill');
ok(pcntl_wifsignaled($status), 'reported as signaled');
same(SIGKILL, pcntl_wtermsig($status), 'termsig SIGKILL');
proc_close($proc);
same(false, posix_kill($pid, 0), 'posix_kill(deadpid, 0) is false');
same(PCNTL_ESRCH, posix_get_last_error(), 'with ESRCH');

// 4. SIGTERM with no handler in the target terminates it
[$proc, $pid, $pipes] = spawn_php('sleep(30);');
usleep(300000);
ok(posix_kill($pid, SIGTERM), 'posix_kill(pid, SIGTERM) without a listener');
pcntl_waitpid($pid, $status);
ok(pcntl_wifsignaled($status) && pcntl_wtermsig($status) === SIGTERM, 'child died "from SIGTERM" (exit code 143)');
proc_close($proc);

// 5. SIGTERM with a handler in the target runs the handler
$code = 'pcntl_async_signals(true); pcntl_signal(SIGTERM, function($s, $i) { echo "got TERM from {$i["pid"]}\n"; exit(7); }); echo "ready\n"; while (true) { usleep(10000); }';
[$proc, $pid, $pipes] = spawn_php($code);
wait_for_marker($pipes[1], "ready");
ok(posix_kill($pid, SIGTERM), 'posix_kill(pid, SIGTERM) to a listening child');
same($pid, pcntl_waitpid($pid, $status), 'child exited');
ok(pcntl_wifexited($status) && pcntl_wexitstatus($status) === 7, 'exit code set by the PHP handler');
$out = drain($pipes[1], 2);
ok(str_contains($out, 'got TERM from ' . getmypid()), 'handler saw the sender pid');
proc_close($proc);

// 6. SIG_IGN in the target
$code = 'pcntl_signal(SIGTERM, SIG_IGN); echo "ready\n"; $t = microtime(true); while (microtime(true) - $t < 1.5) { usleep(10000); } exit(9);';
[$proc, $pid, $pipes] = spawn_php($code);
wait_for_marker($pipes[1], "ready");
ok(posix_kill($pid, SIGTERM), 'posix_kill to a child ignoring SIGTERM');
pcntl_waitpid($pid, $status);
ok(pcntl_wifexited($status) && pcntl_wexitstatus($status) === 9, 'ignored signal did not kill the child');
proc_close($proc);

// 7. SIGSTOP / SIGCONT with WUNTRACED / WCONTINUED
$code = 'echo "ready\n"; $n = 0; while ($n < 100) { usleep(50000); $n++; } exit(5);';
[$proc, $pid, $pipes] = spawn_php($code);
wait_for_marker($pipes[1], "ready");
pcntl_signal_dispatch(); // flush SIGCHLD from the previous children
$chld = [];
ok(posix_kill($pid, SIGSTOP), 'posix_kill(pid, SIGSTOP)');
same($pid, pcntl_waitpid($pid, $status, WUNTRACED), 'waitpid(WUNTRACED) reports the stopped child');
ok(pcntl_wifstopped($status), 'wifstopped');
same(SIGSTOP, pcntl_wstopsig($status), 'wstopsig SIGSTOP');
same(0, pcntl_waitpid($pid, $status, WUNTRACED | WNOHANG), 'stop reported only once');
pcntl_signal_dispatch();
ok(count($chld) === 1 && $chld[0]['code'] === CLD_STOPPED && $chld[0]['pid'] === $pid, 'SIGCHLD with CLD_STOPPED');
ok(posix_kill($pid, SIGCONT), 'posix_kill(pid, SIGCONT)');
same($pid, pcntl_waitpid($pid, $status, WCONTINUED), 'waitpid(WCONTINUED) reports the continued child');
ok(pcntl_wifcontinued($status), 'wifcontinued');
same($pid, pcntl_waitpid($pid, $status), 'child finishes');
same(5, pcntl_wexitstatus($status), 'exit code 5 after resume');
proc_close($proc);

// 8. pcntl_wait(-1) picks any child, with rusage
// children must still be alive when pcntl_wait() first looks (see README, "Known differences")
[$p1, $pid1, $pp1] = spawn_php('usleep(500000); exit(1);');
[$p2, $pid2, $pp2] = spawn_php('usleep(1200000); exit(2);');
$first = pcntl_wait($status, 0, $rusage);
same($pid1, $first, 'pcntl_wait returns the first child to finish');
same(1, pcntl_wexitstatus($status), 'first exit code');
ok(is_array($rusage) && array_key_exists('ru_utime.tv_sec', $rusage) && array_key_exists('ru_maxrss', $rusage), 'rusage array populated');
$second = pcntl_wait($status);
same($pid2, $second, 'then the second');
same(2, pcntl_wexitstatus($status), 'second exit code');
proc_close($p1);
proc_close($p2);

// 9. waitid
[$proc, $pid, $pipes] = spawn_php('usleep(100000); exit(4);');
ok(pcntl_waitid(P_PID, $pid, $info, WEXITED), 'pcntl_waitid(P_PID)');
same(CLD_EXITED, $info['code'], 'waitid info.code');
same(4, $info['status'], 'waitid info.status');
same($pid, $info['pid'], 'waitid info.pid');
same(SIGCHLD, $info['signo'], 'waitid info.signo');
proc_close($proc);
ok(pcntl_waitid(P_ALL, null, $info, WEXITED | WNOHANG) === false, 'waitid with no children is false');
same(PCNTL_ECHILD, pcntl_get_last_error(), 'waitid sets ECHILD');

// 10. posix_kill(0, sig) reaches our own children
$code = 'pcntl_async_signals(true); pcntl_signal(SIGUSR1, function() { exit(11); }); echo "ready\n"; while (true) usleep(10000);';
[$proc, $pid, $pipes] = spawn_php($code);
wait_for_marker($pipes[1], "ready");
pcntl_waitpid($pid, $status, WNOHANG); // registers the pid as a tracked child
pcntl_signal(SIGUSR1, SIG_IGN);
ok(posix_kill(0, SIGUSR1), 'posix_kill(0, SIGUSR1)');
same($pid, pcntl_waitpid($pid, $status), 'child got the group signal');
same(11, pcntl_wexitstatus($status), 'child exit code from its handler');
proc_close($proc);

finish();
