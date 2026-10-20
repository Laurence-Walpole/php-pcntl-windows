<?php
declare(strict_types=1);
require __DIR__ . '/lib.php';

echo "01 basics\n";

ok(extension_loaded('pcntl'), 'extension loaded');
same(15, SIGTERM, 'SIGTERM constant uses Linux numbering');
same(6, SIGABRT, 'SIGABRT constant uses Linux numbering');
same(14, SIGALRM, 'SIGALRM constant');
same(17, SIGCHLD, 'SIGCHLD constant');
same(0, SIG_DFL, 'SIG_DFL');
same(1, SIG_IGN, 'SIG_IGN');
same(1, WNOHANG, 'WNOHANG');
ok(function_exists('posix_kill'), 'posix_kill() companion registered');
same(getmypid(), posix_getpid(), 'posix_getpid() matches getmypid()');
ok(posix_getppid() > 0, 'posix_getppid() returns a pid');

// default handler
same(SIG_DFL, pcntl_signal_get_handler(SIGUSR1), 'default disposition is SIG_DFL');

// handler registration + synchronous dispatch
$got = [];
ok(pcntl_signal(SIGUSR1, function (int $signo, mixed $info) use (&$got) { $got[] = [$signo, $info]; }), 'pcntl_signal() with closure');
ok(is_callable(pcntl_signal_get_handler(SIGUSR1)), 'pcntl_signal_get_handler() returns the closure');
ok(posix_kill(getmypid(), SIGUSR1), 'posix_kill(self, SIGUSR1)');
same([], $got, 'handler does not run before dispatch');
pcntl_signal_dispatch();
same(1, count($got), 'handler ran once after pcntl_signal_dispatch()');
same(SIGUSR1, $got[0][0], 'handler received the signal number');
same(getmypid(), $got[0][1]['pid'], 'siginfo contains the sender pid');
same(SI_USER, $got[0][1]['code'], 'siginfo code is SI_USER');

// async signals: the handler runs without an explicit dispatch
$got = [];
same(false, pcntl_async_signals(true), 'pcntl_async_signals() returns the previous value');
same(true, pcntl_async_signals(), 'pcntl_async_signals() reports enabled');
posix_kill(getmypid(), SIGUSR1);
$spin = 0;
while (count($got) === 0 && $spin++ < 1000000) {
    // the engine checks for interrupts on loop back-edges
}
same(1, count($got), 'handler ran asynchronously');

// ticks
pcntl_async_signals(false);
$got = [];
declare(ticks=1);
posix_kill(getmypid(), SIGUSR1);
$x = 1;
$x++;
same(1, count($got), 'handler ran via declare(ticks=1)');

// SIG_IGN keeps us alive on a fatal-by-default signal
ok(pcntl_signal(SIGTERM, SIG_IGN), 'pcntl_signal(SIGTERM, SIG_IGN)');
same(SIG_IGN, pcntl_signal_get_handler(SIGTERM), 'get_handler reports SIG_IGN');
ok(posix_kill(getmypid(), SIGTERM), 'posix_kill(self, SIGTERM) with SIG_IGN');
pcntl_signal_dispatch();
ok(true, 'still alive after ignored SIGTERM');

// handler for SIGTERM
$term = 0;
pcntl_signal(SIGTERM, function () use (&$term) { $term++; });
posix_kill(getmypid(), SIGTERM);
pcntl_signal_dispatch();
same(1, $term, 'SIGTERM handler ran');

// errors
try {
    pcntl_signal(0, SIG_IGN);
    ok(false, 'pcntl_signal(0) throws');
} catch (ValueError $e) {
    ok(str_contains($e->getMessage(), 'greater than or equal to 1'), 'pcntl_signal(0) throws ValueError');
}
try {
    pcntl_signal(SIGUSR2, 42);
    ok(false, 'pcntl_signal(.., 42) throws');
} catch (ValueError $e) {
    ok(true, 'pcntl_signal(.., 42) throws ValueError');
}
try {
    pcntl_signal(SIGUSR2, 'no_such_function_xyz');
    ok(false, 'non-callable throws');
} catch (TypeError $e) {
    ok(true, 'non-callable handler throws TypeError');
}
same(false, @pcntl_signal(SIGKILL, SIG_IGN), 'SIGKILL cannot be caught/ignored');
same(PCNTL_EINVAL, pcntl_get_last_error(), 'last error is EINVAL');
same(pcntl_get_last_error(), pcntl_errno(), 'pcntl_errno() alias');
ok(str_contains(strtolower(pcntl_strerror(PCNTL_ECHILD)), 'child'), 'pcntl_strerror(ECHILD)');

// alarm
$alarm = 0;
pcntl_signal(SIGALRM, function () use (&$alarm) { $alarm++; });
same(0, pcntl_alarm(1), 'pcntl_alarm(1) with no previous alarm returns 0');
$prev = pcntl_alarm(5);
ok($prev >= 1 && $prev <= 2, "re-arming returns remaining seconds ($prev)");
same(5, pcntl_alarm(0), 'cancelling returns remaining seconds');
pcntl_alarm(1);
$t = microtime(true);
while ($alarm === 0 && microtime(true) - $t < 5) {
    usleep(50000);
    pcntl_signal_dispatch();
}
same(1, $alarm, 'SIGALRM handler ran after pcntl_alarm(1)');
ok(microtime(true) - $t >= 0.9, 'alarm did not fire early');

// wait status helpers (glibc encoding)
ok(pcntl_wifexited(3 << 8) && pcntl_wexitstatus(3 << 8) === 3, 'wifexited/wexitstatus');
ok(pcntl_wifsignaled(9) && pcntl_wtermsig(9) === 9, 'wifsignaled/wtermsig');
ok(pcntl_wifstopped(0x7f | (19 << 8)) && pcntl_wstopsig(0x7f | (19 << 8)) === 19, 'wifstopped/wstopsig');
ok(pcntl_wifcontinued(0xffff), 'wifcontinued');

// cpu / qos
ok(pcntl_getcpu() >= 0, 'pcntl_getcpu()');
$aff = pcntl_getcpuaffinity();
ok(is_array($aff) && count($aff) >= 1, 'pcntl_getcpuaffinity() returns cpu ids');
ok(pcntl_setcpuaffinity(null, $aff), 'pcntl_setcpuaffinity() with the current mask');
same($aff, pcntl_getcpuaffinity(getmypid()), 'affinity unchanged');
try {
    pcntl_setcpuaffinity(null, [9999]);
    ok(false, 'invalid cpu id throws');
} catch (ValueError $e) {
    ok(true, 'invalid cpu id throws ValueError');
}
ok(pcntl_getqos_class() instanceof Pcntl\QosClass, 'pcntl_getqos_class() returns an enum');
pcntl_setqos_class(Pcntl\QosClass::Background);
same(Pcntl\QosClass::Background, pcntl_getqos_class(), 'QosClass::Background round-trips through power throttling');
pcntl_setqos_class();
same(Pcntl\QosClass::Default, pcntl_getqos_class(), 'QosClass::Default restores system-managed throttling');

// priority
same(0, pcntl_getpriority(), 'normal priority class maps to nice 0');
ok(pcntl_setpriority(10), 'pcntl_setpriority(10)');
same(10, pcntl_getpriority(getmypid()), 'below-normal maps to nice 10');
ok(pcntl_setpriority(0), 'restore priority');
same(false, @pcntl_getpriority(999999), 'unknown pid returns false');
same(PCNTL_ESRCH, pcntl_get_last_error(), 'last error ESRCH');

// fork disabled by default
same(-1, @pcntl_fork(), 'pcntl_fork() returns -1 when disabled');
same(PCNTL_ENOSYS, pcntl_get_last_error(), 'pcntl_fork() sets ENOSYS');

// no children
same(-1, pcntl_wait($st), 'pcntl_wait() with no children returns -1');
same(PCNTL_ECHILD, pcntl_get_last_error(), 'and sets ECHILD');

finish();
