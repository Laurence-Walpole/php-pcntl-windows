# pcntl for Windows

`ext/pcntl` (process control: signals, `pcntl_fork`, `pcntl_wait*`, `pcntl_alarm`,
`pcntl_exec`, priorities, CPU affinity) is Unix-only. This project is a drop-in
`php_pcntl.dll` for Windows that implements the same functions and constants on
top of Win32, so code written for Linux (Laravel's queue worker, Symfony
Console's signal handling, Horizon-style supervisors, your own scripts) runs
unchanged.

It depends on nothing but `php8.dll` and the Windows system DLLs that are
already present on every machine (`kernel32`, `ntdll`). No Cygwin, no WSL, no
runtime to install.

## Install

```bat
install.cmd
```

That is the whole setup. The script finds the `php.exe` on your `PATH`, copies
the matching prebuilt DLL into its `extension_dir`, adds `extension=pcntl` to
its `php.ini` and verifies that `php -m` lists `pcntl`. To target a specific
PHP:

```bat
install.cmd -Php "C:\tools\php84\php.exe"
```

Manual install, if you prefer: copy `dist\php_pcntl-<8.x>-<nts|ts>-<x64>.dll`
to your `ext` directory as `php_pcntl.dll` and add `extension=pcntl` to
`php.ini`.

Prebuilt DLLs are provided for the PHP builds the extension was tested with.
For any other PHP 8.2+ build, `install.cmd` (or `build\build.cmd`) compiles one
on the spot; the only requirement for that is Visual Studio with the "Desktop
development with C++" workload (the free Build Tools edition is enough). The
build script downloads the matching PHP devel pack from windows.php.net
automatically.

| PHP | Thread safety | Arch | File |
| --- | --- | --- | --- |
| 8.2 | NTS | x64 | `dist/php_pcntl-8.2-nts-x64.dll` |
| 8.4 | NTS | x64 | `dist/php_pcntl-8.4-nts-x64.dll` |
| 8.5 | NTS | x64 | `dist/php_pcntl-8.5-nts-x64.dll` |
| 8.6 (beta) | NTS | x64 | `dist/php_pcntl-8.6-nts-x64.dll` |

Thread-safe (TS) builds compile but have not been tested; the console and
signal machinery assumes one PHP request per process, which is what the CLI
does.

## Quick check

```php
<?php
pcntl_async_signals(true);
pcntl_signal(SIGINT, function (int $signo, mixed $info) {
    echo "Caught SIGINT, shutting down cleanly\n";
    exit(0);
});
pcntl_signal(SIGTERM, fn () => exit(0));

echo "pid ", getmypid(), " - press Ctrl+C, or run: php -r \"posix_kill(", getmypid(), ", SIGTERM);\"\n";
while (true) {
    usleep(100_000);
}
```

## Composer and `ext-pcntl`

Once the DLL is loaded, Composer sees it as the real thing: `composer show
--platform` lists `ext-pcntl` with your PHP's version, so packages that
`require` `ext-pcntl` (Laravel Horizon, Symfony Console's signal support,
`spatie/async` and others) install and run without changes.

If Composer runs under a PHP that does not have the extension loaded (a CI
image, a different `php.exe`, or before you run `install.cmd`), tell it to
assume the extension is present on the target machine:

```json
{
    "config": {
        "platform": {
            "ext-pcntl": "8.4"
        }
    }
}
```

Use the same major.minor as your PHP. Remove the override once the extension
is loaded in the PHP that runs Composer.

## What works, and how

Every signal source on Windows is mapped onto a Win32 mechanism and funnelled
into one queue that the main PHP thread drains exactly the way upstream pcntl
does: on `pcntl_signal_dispatch()`, on ticks (`declare(ticks=1)`), or
asynchronously through the engine's interrupt hook when
`pcntl_async_signals(true)` is set.

| What happens | Reported as |
| --- | --- |
| Ctrl+C in the console | `SIGINT` |
| Ctrl+Break (also what `GenerateConsoleCtrlEvent` / `sapi_windows_generate_ctrl_event` send) | `SIGQUIT` (change with `pcntl.ctrl_break_signal`) |
| Console window closed, user logging off | `SIGHUP` |
| System shutdown | `SIGTERM` |
| `posix_kill($pid, $sig)` from another PHP process | that signal, with the sender's pid in `$info['pid']` |
| `pcntl_alarm($seconds)` elapsed | `SIGALRM` |
| A child process exits, stops or continues | `SIGCHLD` with `CLD_EXITED` / `CLD_KILLED` / `CLD_STOPPED` / `CLD_CONTINUED` |

Signal numbers follow Linux x86-64 (`SIGTERM` = 15, `SIGUSR1` = 10, ...), which
is what nearly all PHP code was written against.

Default dispositions are honoured: an unhandled `SIGTERM`, `SIGINT`, `SIGALRM`
and so on terminates the process (exit code `128 + signo`, the same convention
shells and Symfony Process use), `SIGCHLD`/`SIGURG`/`SIGWINCH` are ignored,
`SIGSTOP`/`SIGTSTP` suspend the process and `SIGCONT` resumes it. `SIG_IGN`
really ignores, including Ctrl+C.

### Function coverage

| Function | Status |
| --- | --- |
| `pcntl_signal`, `pcntl_signal_get_handler`, `pcntl_signal_dispatch`, `pcntl_async_signals` | full |
| `pcntl_sigprocmask`, `pcntl_sigwaitinfo`, `pcntl_sigtimedwait` | full (blocked signals stay pending and are delivered on unblock) |
| `pcntl_alarm` | full (returns the seconds remaining on the previous alarm) |
| `pcntl_wait`, `pcntl_waitpid`, `pcntl_waitid` with `WNOHANG`, `WUNTRACED`, `WCONTINUED`, `WNOWAIT`, resource usage | full. Children are tracked by process handle. `pcntl_wait(-1)` / `P_ALL` also sees children started with `proc_open()`, `popen()` etc. `pcntl_waitpid($pid)` works on any pid you can open. |
| `pcntl_wifexited`, `pcntl_wifsignaled`, `pcntl_wifstopped`, `pcntl_wifcontinued`, `pcntl_wexitstatus`, `pcntl_wtermsig`, `pcntl_wstopsig` | full (glibc status encoding; crashes decode to `SIGSEGV`/`SIGFPE`/`SIGILL`/`SIGABRT`, Ctrl+C termination to `SIGINT`) |
| `pcntl_exec` | runs the program with `CreateProcess`, waits and exits with its exit code. `.bat`/`.cmd` go through `cmd.exe`. The process image is not replaced (Windows cannot), but from the outside it behaves like exec. |
| `pcntl_getpriority`, `pcntl_setpriority` | nice values are mapped to Windows priority classes (same mapping as Cygwin). Only `PRIO_PROCESS`. |
| `pcntl_getcpuaffinity`, `pcntl_setcpuaffinity`, `pcntl_getcpu` | full (one processor group, up to 64 CPUs) |
| `pcntl_getqos_class`, `pcntl_setqos_class` | mapped onto Windows power throttling (EcoQoS): `Background`/`Utility` enable it, `UserInteractive`/`UserInitiated` disable it, `Default` lets the system manage it |
| `pcntl_get_last_error`, `pcntl_errno`, `pcntl_strerror` | full |
| `pcntl_fork` | **not possible on Windows.** Returns `-1` with `PCNTL_ENOSYS` by default. Opt-in emulation: `pcntl.fork=respawn` (see below). |
| `pcntl_unshare`, `pcntl_rfork`, `pcntl_forkx`, `pcntl_setns` | not provided (Linux/BSD/Solaris specific, upstream omits them on other platforms too) |

### `posix_kill()` and friends

`ext/posix` does not exist on Windows either, and without `posix_kill()` there
is no way to send a signal from PHP. So this extension also registers
`posix_kill()`, `posix_getpid()`, `posix_getppid()`, `posix_get_last_error()`,
`posix_errno()` and `posix_strerror()` (only if nothing else defines them; turn
off with `pcntl.posix_functions=0`).

Delivery works through a named pipe (`\\.\pipe\php-pcntl-<pid>`) that a process
opens the first time it installs a signal handler. If the target has no
handler, the default action is applied from the outside: `TerminateProcess`
for fatal signals, suspend/resume for `SIGSTOP`/`SIGCONT`. `posix_kill($pid, 0)`
checks existence, `posix_kill(0, $sig)` signals this process and the children
it has spawned.

### `pcntl_fork()` emulation (`pcntl.fork=respawn`)

Windows has no `fork()`. When `pcntl.fork=respawn` is set, `pcntl_fork()`
starts a new `php.exe` with the identical command line and environment, plus a
marker telling the child which `pcntl_fork()` call it is the child of. The
child re-runs the script from the top; earlier `pcntl_fork()` calls return the
same values the parent saw, the marked call returns `0`, and later calls fork
for real (nesting works). The parent gets the child's pid and can
`pcntl_waitpid()` it like on Unix.

This is faithful for the common "set up, then fork N workers that loop" pattern
and for libraries such as `spatie/async`, but **everything before the fork runs
again in each child**, including side effects such as database writes or HTTP
calls. That is why it is opt-in. Keep parent-only work after the fork, or guard
it with `getenv('PHP_PCNTL_FORK_CHILD_OF') === false` on the first line of the
script (the variable is removed from the environment after the extension reads
it, but `$_SERVER` still shows it).

### ini settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `pcntl.fork` | `disabled` | `disabled`: `pcntl_fork()` returns `-1`/`ENOSYS`. `respawn`: emulate by re-running the script. |
| `pcntl.ctrl_break_signal` | `3` (`SIGQUIT`) | Signal number reported for Ctrl+Break / `CTRL_BREAK_EVENT`. Set to `15` if your supervisor sends Ctrl+Break and your code listens for `SIGTERM`. |
| `pcntl.posix_functions` | `1` | Register the `posix_*` companions. |

### Known differences from Unix

- `sleep()`, `usleep()` and blocking I/O are not interrupted by signals. With
  `pcntl_async_signals(true)` the handler runs as soon as the blocking call
  returns. `pcntl_wait*()` and `pcntl_sigtimedwait()` are interruptible
  (`EINTR`) when the handler was registered with `restart_syscalls = false`.
- Children you did not start through `pcntl_fork()` (e.g. `proc_open()`) are
  discovered from a process snapshot when you call `pcntl_wait()`,
  `pcntl_waitid(P_ALL)` or install a `SIGCHLD` handler. Windows keeps no
  zombies, so a child that has already exited before the first of those calls
  is never seen by `pcntl_wait(-1)`. `pcntl_waitpid($pid)` on the specific pid
  still works while something (such as the `proc_open` resource) holds the
  process handle.
- There are no process groups or sessions. `pcntl_waitpid(0)`, `-1` and
  negative pids all mean "any child"; `posix_kill(-$pgid, ...)` signals the
  single pid.
- Hardware signals (`SIGSEGV`, `SIGFPE`, ...) cannot be caught in PHP; they are
  reported to the parent through the wait status instead.
- `SIGKILL` and `SIGSTOP` are implemented with `TerminateProcess` and
  `NtSuspendProcess`; a process killed this way exits with code `128 + signo`.
- Exit codes `129..159` of a child are decoded as "terminated by signal", the
  same ambiguity shells have.

## Building

```bat
build\build.cmd                              :: for the php.exe on PATH
build\build.cmd -Php C:\tools\php84\php.exe   :: for a specific PHP
build\build-all.ps1                           :: every Laravel Herd PHP 8.x, with tests
```

Requires Visual Studio 2017+ with the C++ workload. The script downloads the
matching devel pack (headers and `php8.lib`) from windows.php.net into
`build\devel\`, compiles `src\pcntl_win.c` and writes
`dist\php_pcntl-<ver>-<ts>-<arch>.dll`.

## Tests

```bat
php tests\run.php
```

Runs each `tests\NN_*.php` in a fresh process with the built DLL. The suite
covers handler registration and dispatch (sync, ticks, async), masks and
`sigwait`, `posix_kill` between processes, `SIGKILL`/`SIGTERM`/`SIG_IGN`
semantics, stop/continue with `WUNTRACED`/`WCONTINUED`, `SIGCHLD`, `waitid`,
rusage, respawn-fork (including nesting), `pcntl_exec`, Ctrl+C and Ctrl+Break
delivery, priorities, affinity and QoS.

## How this was built

This extension was created with [Claude](https://claude.com) (Claude Code)
and [REA, Reverse Engineer Anything](https://github.com/morluto/rea).

The behaviour was derived from the upstream `ext/pcntl` sources (PHP 8.4
branch) and PHP's own Windows console handling (`win32/signal.c`, which already
hooks the engine's interrupt function for `sapi_windows_set_ctrl_handler`).
REA was used for provider-free PE artifact inspection of `php8.dll` and of the
built module; the export surface of the shipped `php8.dll` was checked with
`dumpbin` to make sure every engine symbol the shim relies on is actually
exported. The test suite was used to drive the implementation and caught
several real bugs along the way.

## Licence

MIT (see `LICENSE`). Parts derived from php-src `ext/pcntl` are under the
PHP License 3.01; see `NOTICE`.
