<?php
declare(strict_types=1);
require __DIR__ . '/lib.php';

echo "02 sigprocmask / sigwaitinfo / sigtimedwait\n";

$seen = [];
pcntl_signal(SIGUSR1, function (int $s) use (&$seen) { $seen[] = $s; });
pcntl_signal(SIGUSR2, function (int $s) use (&$seen) { $seen[] = $s; });

// block, send, verify it is held back, unblock, verify delivery
ok(pcntl_sigprocmask(SIG_BLOCK, [SIGUSR1], $old), 'SIG_BLOCK');
same([], $old, 'old set was empty');
posix_kill(getmypid(), SIGUSR1);
pcntl_signal_dispatch();
same([], $seen, 'blocked signal is not delivered');
ok(pcntl_sigprocmask(SIG_UNBLOCK, [SIGUSR1], $old), 'SIG_UNBLOCK');
same([SIGUSR1], $old, 'old set contained SIGUSR1');
pcntl_signal_dispatch();
same([SIGUSR1], $seen, 'pending signal delivered after unblock');

// SIG_SETMASK with an empty array clears everything
pcntl_sigprocmask(SIG_BLOCK, [SIGUSR1, SIGUSR2]);
ok(pcntl_sigprocmask(SIG_SETMASK, [], $old), 'SIG_SETMASK []');
sort($old);
same([SIGUSR1, SIGUSR2], $old, 'old mask reported');

// sigtimedwait: timeout
$t = microtime(true);
same(false, pcntl_sigtimedwait([SIGUSR2], $info, 0, 200_000_000), 'sigtimedwait times out');
same(PCNTL_EAGAIN, pcntl_get_last_error(), 'timeout sets EAGAIN');
ok(microtime(true) - $t >= 0.18, 'waited roughly the requested time');

// sigtimedwait: signal already pending while blocked
pcntl_sigprocmask(SIG_BLOCK, [SIGUSR2]);
posix_kill(getmypid(), SIGUSR2);
same(SIGUSR2, pcntl_sigtimedwait([SIGUSR2], $info, 1), 'sigtimedwait returns the pending signal');
same(SIGUSR2, $info['signo'], 'info.signo');
same(getmypid(), $info['pid'], 'info.pid is the sender');
pcntl_sigprocmask(SIG_UNBLOCK, [SIGUSR2]);
pcntl_signal_dispatch();
same([SIGUSR1], $seen, 'consumed signal is not also dispatched to the handler');

// sigwaitinfo: delivered from another process while we wait
[$proc, $pid, $pipes] = spawn_php('usleep(300000); posix_kill(' . getmypid() . ', SIGUSR2); echo "sent\n";');
$t = microtime(true);
same(SIGUSR2, pcntl_sigwaitinfo([SIGUSR2], $info), 'sigwaitinfo woke up on a signal from another process');
same($pid, $info['pid'], 'sender pid reported');
ok(microtime(true) - $t >= 0.25, 'actually waited');
proc_close($proc);

// errors
try {
    pcntl_sigprocmask(99, [SIGUSR1]);
    ok(false, 'bad mode throws');
} catch (ValueError $e) {
    ok(true, 'bad mode throws ValueError');
}
try {
    pcntl_sigwaitinfo([]);
    ok(false, 'empty set throws');
} catch (ValueError $e) {
    ok(true, 'empty set throws ValueError');
}
try {
    pcntl_sigtimedwait([SIGUSR1], $i, 0, 0);
    ok(false, 'zero timeout throws');
} catch (ValueError $e) {
    ok(true, 'zero timeout throws ValueError');
}

finish();
