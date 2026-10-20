<?php
declare(strict_types=1);
require __DIR__ . '/lib.php';

echo "05 pcntl_exec()\n";

// a child that execs php itself: the child's exit code must be the exec'd program's
$code = 'pcntl_exec(PHP_BINARY, ["-n", "-r", "echo getenv(\"PCNTL_EXEC_TEST\"), \" arg with space\", PHP_EOL; exit(23);"], ["PCNTL_EXEC_TEST" => "env ok", "PATH" => getenv("PATH")]); echo "exec returned\n"; exit(1);';
[$proc, $pid, $pipes] = spawn_php($code);
same($pid, pcntl_waitpid($pid, $status), 'waitpid on exec child');
same(23, pcntl_wexitstatus($status), 'exit code is the exec\'d program\'s exit code');
$out = drain($pipes[1], 2);
ok(str_contains($out, 'env ok arg with space'), 'environment and quoted arguments passed through');
ok(!str_contains($out, 'exec returned'), 'pcntl_exec() did not return');
proc_close($proc);

// failure returns false with ENOENT
same(false, @pcntl_exec('C:\\no\\such\\program.exe'), 'missing program returns false');
same(PCNTL_ENOENT, pcntl_get_last_error(), 'ENOENT');

// .cmd scripts are run through cmd.exe
$cmd = sys_get_temp_dir() . '\\pcntl_test_' . getmypid() . '.cmd';
file_put_contents($cmd, "@echo off\r\necho from cmd %1\r\nexit /b 5\r\n");
[$proc, $pid, $pipes] = spawn_php('pcntl_exec(' . var_export($cmd, true) . ', ["hello"]);');
pcntl_waitpid($pid, $status);
same(5, pcntl_wexitstatus($status), 'cmd script exit code');
ok(str_contains(drain($pipes[1], 2), 'from cmd hello'), 'cmd script ran with its argument');
proc_close($proc);
unlink($cmd);

finish();
