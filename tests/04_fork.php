<?php
declare(strict_types=1);
require __DIR__ . '/lib.php';

// This test is started by run.php with -d pcntl.fork=respawn
if (ini_get('pcntl.fork') !== 'respawn') {
    echo "04 fork: skipped (needs -d pcntl.fork=respawn)\n";
    exit(0);
}

// Everything above the first fork runs in every child again (respawn semantics),
// so keep the parent-only side effects below the fork.
$children = [];
$ppid = getmypid();

for ($i = 1; $i <= 3; $i++) {
    $pid = pcntl_fork();
    if ($pid === -1) {
        echo "fork failed\n";
        exit(1);
    }
    if ($pid === 0) {
        // child: the ppid env hint lets posix_getppid() point at the real parent
        $mine = getmypid();
        $parent = posix_getppid();
        // the second child forks a grandchild to prove nesting works
        if ($i === 2) {
            $g = pcntl_fork();
            if ($g === 0) {
                exit(42);
            }
            pcntl_waitpid($g, $gs);
            exit(pcntl_wexitstatus($gs) === 42 ? 20 : 99);
        }
        fwrite(STDERR, "child $i pid=$mine parent=$parent\n");
        exit(10 + $i);
    }
    $children[$i] = $pid;
}

echo "04 pcntl_fork() respawn emulation\n";
ok(count(array_unique($children)) === 3 && min($children) > 0, 'three distinct child pids');
ok(!in_array(getmypid(), $children, true), 'children are different processes');

$codes = [];
foreach ($children as $i => $pid) {
    same($pid, pcntl_waitpid($pid, $status), "waitpid child $i");
    ok(pcntl_wifexited($status), "child $i exited normally");
    $codes[$i] = pcntl_wexitstatus($status);
}
same(11, $codes[1], 'child 1 returned 0 from its fork and exited 11');
same(20, $codes[2], 'child 2 forked a grandchild which exited 42');
same(13, $codes[3], 'child 3 exited 13');

same(-1, pcntl_wait($status), 'no children left');
same(PCNTL_ECHILD, pcntl_get_last_error(), 'ECHILD');

finish();
