/*
   +----------------------------------------------------------------------+
   | pcntl for Windows: an emulation of ext/pcntl on top of Win32          |
   +----------------------------------------------------------------------+
   | Parts of this file (argument handling, error messages and the        |
   | dispatch logic) are derived from php-src ext/pcntl/pcntl.c,          |
   | Copyright (c) The PHP Group, PHP License 3.01.                       |
   | The Windows emulation layer is released under the MIT license.       |
   +----------------------------------------------------------------------+

   How the emulation works
   -----------------------
   POSIX signals do not exist on Windows, so every signal source is mapped
   onto a Win32 mechanism and funnelled into one queue that the main PHP
   thread drains exactly the way ext/pcntl does (ticks, pcntl_signal_dispatch()
   and the zend_interrupt_function hook for pcntl_async_signals()).

   Source                     Win32 mechanism                        Signal
   -------------------------  -------------------------------------  --------
   Ctrl+C                     SetConsoleCtrlHandler CTRL_C_EVENT     SIGINT
   Ctrl+Break                 SetConsoleCtrlHandler CTRL_BREAK_EVENT SIGQUIT (pcntl.ctrl_break_signal)
   console window closed      CTRL_CLOSE_EVENT / CTRL_LOGOFF_EVENT   SIGHUP
   system shutdown            CTRL_SHUTDOWN_EVENT                    SIGTERM
   posix_kill() from PHP      named pipe \\.\pipe\php-pcntl-<pid>    any
   pcntl_alarm()              timer-queue timer                      SIGALRM
   child process exit         RegisterWaitForSingleObject            SIGCHLD

   Processes are tracked with real process handles, so pcntl_wait*(),
   WNOHANG, WUNTRACED and WCONTINUED work; SIGSTOP/SIGCONT are emulated
   with NtSuspendProcess/NtResumeProcess. pcntl_exec() runs the program with
   CreateProcess and exits with its exit code. pcntl_fork() cannot exist on
   Windows; it fails with ENOSYS unless pcntl.fork=respawn is set, in which
   case the script is re-run in a child process that returns 0 from the
   matching pcntl_fork() call (see README).
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "SAPI.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_fibers.h"
#include "Zend/zend_enum.h"
#include "Zend/zend_atomic.h"
#include "main/php_ticks.h"

#include <windows.h>
#include <tlhelp32.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include "pcntl_win.h"
#include "pcntl_win_arginfo.h"

/* ------------------------------------------------------------------------ */
/* Module boilerplate                                                       */
/* ------------------------------------------------------------------------ */

static zend_class_entry *QosClass_ce;

ZEND_DECLARE_MODULE_GLOBALS(pcntl)
static PHP_GINIT_FUNCTION(pcntl);

PHP_MINIT_FUNCTION(pcntl);
PHP_MSHUTDOWN_FUNCTION(pcntl);
PHP_RINIT_FUNCTION(pcntl);
PHP_RSHUTDOWN_FUNCTION(pcntl);
PHP_MINFO_FUNCTION(pcntl);

PHP_INI_BEGIN()
	/* "disabled" (pcntl_fork() returns -1 / ENOSYS) or "respawn" (re-run the script in a child process) */
	PHP_INI_ENTRY("pcntl.fork", "disabled", PHP_INI_SYSTEM, NULL)
	/* register posix_kill(), posix_getpid(), posix_getppid(), posix_get_last_error(), posix_errno(), posix_strerror() */
	PHP_INI_ENTRY("pcntl.posix_functions", "1", PHP_INI_SYSTEM, NULL)
	/* which signal Ctrl+Break is reported as (default SIGQUIT=3, the .NET convention) */
	PHP_INI_ENTRY("pcntl.ctrl_break_signal", "3", PHP_INI_ALL, NULL)
PHP_INI_END()

zend_module_entry pcntl_module_entry = {
	STANDARD_MODULE_HEADER,
	"pcntl",
	ext_functions,
	PHP_MINIT(pcntl),
	PHP_MSHUTDOWN(pcntl),
	PHP_RINIT(pcntl),
	PHP_RSHUTDOWN(pcntl),
	PHP_MINFO(pcntl),
	PHP_PCNTL_VERSION,
	PHP_MODULE_GLOBALS(pcntl),
	PHP_GINIT(pcntl),
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_PCNTL
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(pcntl)
#endif

/* ------------------------------------------------------------------------ */
/* Process-wide state shared with worker threads                            */
/* ------------------------------------------------------------------------ */

typedef struct {
	int signo;
	int code;        /* SI_* / CLD_* */
	zend_long pid;   /* sender or child pid */
	zend_long status;
	zend_long value;
} pcntl_sig;

typedef struct pcntl_child {
	struct pcntl_child *next;
	DWORD pid;
	HANDLE hproc;
	HANDLE hwait;
	int exited;
	int chld_sent;   /* SIGCHLD for the exit has been queued */
	DWORD exit_code;
	int stopped;
	int stop_sig;
	int stop_reported;
	int continued;
	int adopted;     /* not spawned by us: waitpid() on an arbitrary pid */
} pcntl_child;

typedef struct {
	uint32_t magic;
	int32_t signo;
	uint32_t sender;
	int32_t value;
	uint32_t flags;
} pcntl_pipe_msg;

#define PCNTL_PIPE_MAGIC 0x544E4350u /* 'PCNT' */
#define PCNTL_PIPE_HAS_VALUE 1u
#define PCNTL_RING_SIZE 512

enum { DISPO_DFL = 0, DISPO_IGN = 1, DISPO_HANDLER = 2 };
enum { ACT_NONE = 0, ACT_TERMINATE = 1 };
enum { DEF_TERM, DEF_IGN, DEF_STOP, DEF_CONT };
enum { FORK_DISABLED = 0, FORK_RESPAWN = 1 };

static struct {
	CRITICAL_SECTION lock;
	int lock_ready;

	/* queue of signals waiting for the main thread */
	pcntl_sig ring[PCNTL_RING_SIZE];
	unsigned ring_head, ring_count;
	volatile LONG pending;
	volatile LONG eintr;

	/* sigprocmask() state */
	uint32_t blocked;
	uint32_t pending_mask;
	pcntl_sig pending_info[PCNTL_NSIG];

	unsigned char dispo[PCNTL_NSIG];
	unsigned char restart[PCNTL_NSIG];
	volatile LONG async;
	zend_atomic_bool *vm_interrupt;

	HANDLE wake;   /* auto-reset: something arrived (signal or child state) */
	HANDLE ack;    /* auto-reset: a dispatch pass finished */

	/* console */
	int ctrl_installed;
	int ctrl_break_signal;
	volatile LONG exec_waiting;

	/* named-pipe listener for posix_kill() */
	HANDLE pipe, pipe_thread, pipe_stop, pipe_io;

	/* pcntl_alarm() */
	HANDLE alarm_timer;
	ULONGLONG alarm_due;
	volatile LONG alarm_gen;

	/* children */
	pcntl_child *children;
	DWORD reaped[256];       /* recently reaped pids: waiting on them again is ECHILD, like a reaped zombie */
	unsigned reaped_next;
	FILETIME own_start;

	/* pcntl_fork() respawn emulation */
	int fork_mode;
	zend_long fork_calls;
	zend_long replay_k;
	zend_long *replay_returns;
	int replay_count;
	zend_long ppid_hint;
	zend_long *history;
	int history_count, history_cap;

	int is_cli;
	char pipe_name_a[64];
} W;

typedef LONG (NTAPI *NtSuspendProcess_t)(HANDLE);
typedef LONG (NTAPI *NtResumeProcess_t)(HANDLE);
typedef LONG (NTAPI *NtQueryInformationProcess_t)(HANDLE, int, PVOID, ULONG, PULONG);
static NtSuspendProcess_t pNtSuspendProcess;
static NtResumeProcess_t pNtResumeProcess;
static NtQueryInformationProcess_t pNtQueryInformationProcess;

static void (*orig_interrupt_function)(zend_execute_data *execute_data);

static void pcntl_signal_dispatch(void);
static void pcntl_signal_dispatch_tick_function(int dummy_int, void *dummy_pointer);
static void pcntl_interrupt_function(zend_execute_data *execute_data);
static int pcntl_deliver(const pcntl_sig *s);
static bool pcntl_ensure_listener(void);

#define LOCK()   EnterCriticalSection(&W.lock)
#define UNLOCK() LeaveCriticalSection(&W.lock)
#define SIGBIT(s) (1u << (s))

/* ------------------------------------------------------------------------ */
/* Small helpers                                                            */
/* ------------------------------------------------------------------------ */

static int pcntl_default_action(int signo)
{
	switch (signo) {
		case PSIG_CHLD: case PSIG_URG: case PSIG_WINCH:
			return DEF_IGN;
		case PSIG_STOP: case PSIG_TSTP: case PSIG_TTIN: case PSIG_TTOU:
			return DEF_STOP;
		case PSIG_CONT:
			return DEF_CONT;
		default:
			return DEF_TERM;
	}
}

static int pcntl_win_errno(DWORD err)
{
	switch (err) {
		case ERROR_INVALID_PARAMETER:
		case ERROR_NOT_FOUND:
			return ESRCH;
		case ERROR_ACCESS_DENIED:
			return EPERM;
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
			return ENOENT;
		case ERROR_BAD_EXE_FORMAT:
			return ENOEXEC;
		case ERROR_NOT_ENOUGH_MEMORY:
		case ERROR_OUTOFMEMORY:
			return ENOMEM;
		case ERROR_PIPE_BUSY:
			return EAGAIN;
		default:
			return EINVAL;
	}
}

static ZEND_NORETURN void pcntl_terminate_self(int signo)
{
	TerminateProcess(GetCurrentProcess(), PCNTL_SIGNAL_EXIT_CODE(signo));
	/* not reached, but keep the compiler happy */
	ExitProcess(PCNTL_SIGNAL_EXIT_CODE(signo));
}

/* Decode a Windows exit code into a POSIX wait status word. */
static int pcntl_exitcode_to_status(DWORD code)
{
	switch (code) {
		case 0xC000013Au: /* STATUS_CONTROL_C_EXIT */
			return PSIG_INT;
		case 0xC0000005u: /* STATUS_ACCESS_VIOLATION */
		case 0xC00000FDu: /* STATUS_STACK_OVERFLOW */
		case 0xC0000006u: /* STATUS_IN_PAGE_ERROR */
			return PSIG_SEGV;
		case 0xC0000094u: /* STATUS_INTEGER_DIVIDE_BY_ZERO */
		case 0xC000008Eu: /* STATUS_FLOAT_DIVIDE_BY_ZERO */
		case 0xC0000095u: /* STATUS_INTEGER_OVERFLOW */
			return PSIG_FPE;
		case 0xC000001Du: /* STATUS_ILLEGAL_INSTRUCTION */
		case 0xC0000096u: /* STATUS_PRIVILEGED_INSTRUCTION */
			return PSIG_ILL;
		case 0xC0000409u: /* STATUS_STACK_BUFFER_OVERRUN (fail fast) */
		case 0xC0000374u: /* STATUS_HEAP_CORRUPTION */
		case 0xC0000017u: /* STATUS_NO_MEMORY */
			return PSIG_ABRT;
		case 0x40010004u: /* DBG_TERMINATE_PROCESS */
			return PSIG_KILL;
		default:
			break;
	}
	if (code > 128 && code < 128 + PCNTL_NSIG) {
		return (int) (code - 128); /* terminated through this extension (or a shell convention exit) */
	}
	return (int) ((code & 0xff) << 8);
}

static wchar_t *pcntl_utf8_to_wide(const char *s, size_t len)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, (int) len, NULL, 0);
	wchar_t *w = emalloc(((size_t) n + 1) * sizeof(wchar_t));
	if (n > 0) {
		MultiByteToWideChar(CP_UTF8, 0, s, (int) len, w, n);
	}
	w[n] = 0;
	return w;
}

static void pcntl_resolve_ntdll(void)
{
	HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	if (!nt) {
		return;
	}
	pNtSuspendProcess = (NtSuspendProcess_t) GetProcAddress(nt, "NtSuspendProcess");
	pNtResumeProcess = (NtResumeProcess_t) GetProcAddress(nt, "NtResumeProcess");
	pNtQueryInformationProcess = (NtQueryInformationProcess_t) GetProcAddress(nt, "NtQueryInformationProcess");
}

static zend_long pcntl_parent_pid(void)
{
	struct {
		PVOID Reserved1;
		PVOID PebBaseAddress;
		PVOID Reserved2[2];
		ULONG_PTR UniqueProcessId;
		ULONG_PTR InheritedFromUniqueProcessId;
	} pbi;
	ULONG len = 0;

	if (pNtQueryInformationProcess
		&& pNtQueryInformationProcess(GetCurrentProcess(), 0 /* ProcessBasicInformation */, &pbi, sizeof(pbi), &len) == 0) {
		return (zend_long) pbi.InheritedFromUniqueProcessId;
	}
	return W.ppid_hint;
}

/* ------------------------------------------------------------------------ */
/* Signal queue (any thread may enqueue, only the main thread drains)        */
/* ------------------------------------------------------------------------ */

static void pcntl_ring_push_locked(const pcntl_sig *s)
{
	if (W.ring_count >= PCNTL_RING_SIZE) {
		return; /* too many signals for us to track, so we'll forget about this one (as upstream does) */
	}
	W.ring[(W.ring_head + W.ring_count) % PCNTL_RING_SIZE] = *s;
	W.ring_count++;
	InterlockedExchange(&W.pending, 1);
}

static void pcntl_ring_push_front_locked(const pcntl_sig *s)
{
	if (W.ring_count >= PCNTL_RING_SIZE) {
		return;
	}
	W.ring_head = (W.ring_head + PCNTL_RING_SIZE - 1) % PCNTL_RING_SIZE;
	W.ring[W.ring_head] = *s;
	W.ring_count++;
	InterlockedExchange(&W.pending, 1);
}

static void pcntl_request_interrupt(void)
{
	if (W.async && W.vm_interrupt) {
		zend_atomic_bool_store_ex(W.vm_interrupt, true);
	}
}

/*
 * Deliver a signal to this process. Returns ACT_TERMINATE when the default
 * disposition says the process must die; the caller decides how (console
 * handlers return FALSE so Windows does it, threads call pcntl_terminate_self).
 * Must be called without the lock held.
 */
static int pcntl_deliver_locked(const pcntl_sig *s)
{
	int signo = s->signo;

	if (signo <= 0 || signo >= PCNTL_NSIG) {
		return ACT_NONE;
	}

	if ((W.blocked & SIGBIT(signo)) && signo != PSIG_KILL && signo != PSIG_STOP) {
		W.pending_mask |= SIGBIT(signo);
		W.pending_info[signo] = *s;
		SetEvent(W.wake);
		return ACT_NONE;
	}

	switch (W.dispo[signo]) {
		case DISPO_HANDLER:
			pcntl_ring_push_locked(s);
			if (!W.restart[signo]) {
				InterlockedExchange(&W.eintr, 1);
			}
			SetEvent(W.wake);
			pcntl_request_interrupt();
			return ACT_NONE;
		case DISPO_IGN:
			return ACT_NONE;
		default:
			break;
	}

	switch (pcntl_default_action(signo)) {
		case DEF_IGN:
		case DEF_CONT:
			return ACT_NONE;
		case DEF_STOP:
			if (pNtSuspendProcess) {
				pNtSuspendProcess(GetCurrentProcess());
			}
			return ACT_NONE;
		default:
			return ACT_TERMINATE;
	}
}

static int pcntl_deliver(const pcntl_sig *s)
{
	int act;
	LOCK();
	act = pcntl_deliver_locked(s);
	UNLOCK();
	return act;
}

/* Re-deliver signals that became unblocked. Call with the lock held; returns ACT_TERMINATE if one of them is fatal. */
static int pcntl_flush_unblocked_locked(uint32_t previously_blocked)
{
	uint32_t now_deliverable = previously_blocked & ~W.blocked & W.pending_mask;
	int act = ACT_NONE;
	int signo;

	for (signo = 1; signo < PCNTL_NSIG && act == ACT_NONE; signo++) {
		if (now_deliverable & SIGBIT(signo)) {
			pcntl_sig s = W.pending_info[signo];
			W.pending_mask &= ~SIGBIT(signo);
			act = pcntl_deliver_locked(&s);
		}
	}
	return act;
}

/* ------------------------------------------------------------------------ */
/* Console control events                                                   */
/* ------------------------------------------------------------------------ */

static BOOL WINAPI pcntl_console_ctrl_handler(DWORD evt)
{
	pcntl_sig s;
	int act;

	if (W.exec_waiting && (evt == CTRL_C_EVENT || evt == CTRL_BREAK_EVENT)) {
		return TRUE; /* pcntl_exec() child owns the console now */
	}

	memset(&s, 0, sizeof(s));
	s.code = PCNTL_SI_KERNEL;
	switch (evt) {
		case CTRL_C_EVENT:        s.signo = PSIG_INT; break;
		case CTRL_BREAK_EVENT:    s.signo = W.ctrl_break_signal; break;
		case CTRL_CLOSE_EVENT:
		case CTRL_LOGOFF_EVENT:   s.signo = PSIG_HUP; break;
		case CTRL_SHUTDOWN_EVENT: s.signo = PSIG_TERM; break;
		default:
			return FALSE;
	}

	act = pcntl_deliver(&s);
	if (act == ACT_TERMINATE) {
		return FALSE; /* default disposition: let Windows terminate the process */
	}

	if (evt == CTRL_CLOSE_EVENT || evt == CTRL_LOGOFF_EVENT || evt == CTRL_SHUTDOWN_EVENT) {
		/* Windows kills us when this returns; give the PHP handler a chance to run first. */
		WaitForSingleObject(W.ack, evt == CTRL_CLOSE_EVENT ? 4500 : 15000);
	}
	return TRUE;
}

static void pcntl_install_console_handler(void)
{
	if (!W.ctrl_installed && W.is_cli) {
		if (SetConsoleCtrlHandler(pcntl_console_ctrl_handler, TRUE)) {
			W.ctrl_installed = 1;
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Named-pipe listener: how posix_kill() reaches another PHP process        */
/* ------------------------------------------------------------------------ */

static void pcntl_pipe_name(wchar_t *buf, size_t n, DWORD pid)
{
	_snwprintf(buf, n, L"\\\\.\\pipe\\php-pcntl-%lu", (unsigned long) pid);
	buf[n - 1] = 0;
}

static DWORD WINAPI pcntl_pipe_thread(LPVOID arg)
{
	HANDLE waits[2];
	(void) arg;
	waits[0] = W.pipe_stop;
	waits[1] = W.pipe_io;

	for (;;) {
		OVERLAPPED ov;
		BOOL ok;
		DWORD err, n = 0, r;
		pcntl_pipe_msg msg;

		ZeroMemory(&ov, sizeof(ov));
		ov.hEvent = W.pipe_io;
		ResetEvent(W.pipe_io);

		ok = ConnectNamedPipe(W.pipe, &ov);
		err = GetLastError();
		if (!ok && err == ERROR_IO_PENDING) {
			if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
				CancelIo(W.pipe);
				break;
			}
		} else if (!ok && err != ERROR_PIPE_CONNECTED) {
			if (WaitForSingleObject(W.pipe_stop, 50) == WAIT_OBJECT_0) {
				break;
			}
			continue;
		}
		if (WaitForSingleObject(W.pipe_stop, 0) == WAIT_OBJECT_0) {
			DisconnectNamedPipe(W.pipe);
			break;
		}

		ZeroMemory(&ov, sizeof(ov));
		ov.hEvent = W.pipe_io;
		ResetEvent(W.pipe_io);
		ZeroMemory(&msg, sizeof(msg));
		ok = ReadFile(W.pipe, &msg, sizeof(msg), &n, &ov);
		if (!ok && GetLastError() == ERROR_IO_PENDING) {
			r = WaitForMultipleObjects(2, waits, FALSE, 2000);
			if (r == WAIT_OBJECT_0) {
				CancelIo(W.pipe);
				DisconnectNamedPipe(W.pipe);
				break;
			}
			if (r == WAIT_TIMEOUT) {
				CancelIo(W.pipe);
				ok = FALSE;
			} else {
				ok = GetOverlappedResult(W.pipe, &ov, &n, FALSE);
			}
		}
		DisconnectNamedPipe(W.pipe);

		if (ok && n == sizeof(msg) && msg.magic == PCNTL_PIPE_MAGIC && msg.signo > 0 && msg.signo < PCNTL_NSIG) {
			pcntl_sig s;
			memset(&s, 0, sizeof(s));
			s.signo = msg.signo;
			s.code = (msg.flags & PCNTL_PIPE_HAS_VALUE) ? PCNTL_SI_QUEUE : PCNTL_SI_USER;
			s.pid = msg.sender;
			s.value = msg.value;
			if (pcntl_deliver(&s) == ACT_TERMINATE) {
				pcntl_terminate_self(msg.signo);
			}
		}
	}
	return 0;
}

static bool pcntl_ensure_listener(void)
{
	wchar_t name[64];

	if (W.pipe) {
		return true;
	}

	pcntl_pipe_name(name, 64, GetCurrentProcessId());
	W.pipe = CreateNamedPipeW(name,
		PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		1, 256, 256, 0, NULL);
	if (W.pipe == INVALID_HANDLE_VALUE) {
		W.pipe = NULL;
		return false;
	}
	W.pipe_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	W.pipe_io = CreateEventW(NULL, TRUE, FALSE, NULL);
	W.pipe_thread = CreateThread(NULL, 0, pcntl_pipe_thread, NULL, 0, NULL);
	if (!W.pipe_thread) {
		CloseHandle(W.pipe);
		W.pipe = NULL;
		return false;
	}
	return true;
}

static void pcntl_stop_listener(void)
{
	if (!W.pipe) {
		return;
	}
	SetEvent(W.pipe_stop);
	if (WaitForSingleObject(W.pipe_thread, 1000) == WAIT_TIMEOUT) {
		TerminateThread(W.pipe_thread, 0);
	}
	CloseHandle(W.pipe_thread);
	CloseHandle(W.pipe);
	CloseHandle(W.pipe_stop);
	CloseHandle(W.pipe_io);
	W.pipe = W.pipe_thread = W.pipe_stop = W.pipe_io = NULL;
}

/* 1 = delivered, 0 = no listener in the target, -1 = error (errno in *err) */
static int pcntl_pipe_send(DWORD pid, int signo, zend_long value, int has_value, int *err)
{
	wchar_t name[64];
	int attempt;

	pcntl_pipe_name(name, 64, pid);
	for (attempt = 0; attempt < 40; attempt++) {
		HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		DWORD e;
		if (h != INVALID_HANDLE_VALUE) {
			pcntl_pipe_msg msg;
			DWORD written = 0;
			BOOL ok;
			msg.magic = PCNTL_PIPE_MAGIC;
			msg.signo = signo;
			msg.sender = GetCurrentProcessId();
			msg.value = (int32_t) value;
			msg.flags = has_value ? PCNTL_PIPE_HAS_VALUE : 0;
			ok = WriteFile(h, &msg, sizeof(msg), &written, NULL);
			FlushFileBuffers(h);
			CloseHandle(h);
			if (ok && written == sizeof(msg)) {
				return 1;
			}
			*err = EIO;
			return -1;
		}
		e = GetLastError();
		if (e == ERROR_PIPE_BUSY) {
			WaitNamedPipeW(name, 50);
			continue;
		}
		if (e == ERROR_FILE_NOT_FOUND) {
			return 0;
		}
		*err = pcntl_win_errno(e);
		return -1;
	}
	*err = EAGAIN;
	return -1;
}

/* ------------------------------------------------------------------------ */
/* pcntl_alarm()                                                            */
/* ------------------------------------------------------------------------ */

static VOID CALLBACK pcntl_alarm_callback(PVOID param, BOOLEAN fired)
{
	pcntl_sig s;
	(void) fired;
	if ((LONG) (intptr_t) param != W.alarm_gen) {
		return; /* stale timer */
	}
	memset(&s, 0, sizeof(s));
	s.signo = PSIG_ALRM;
	s.code = PCNTL_SI_TIMER;
	W.alarm_due = 0;
	if (pcntl_deliver(&s) == ACT_TERMINATE) {
		pcntl_terminate_self(PSIG_ALRM);
	}
}

static zend_long pcntl_alarm_set(zend_long seconds)
{
	zend_long remaining = 0;
	ULONGLONG now = GetTickCount64();
	HANDLE old = W.alarm_timer;

	if (W.alarm_due > now) {
		remaining = (zend_long) ((W.alarm_due - now + 999) / 1000);
	}
	InterlockedIncrement(&W.alarm_gen);
	W.alarm_timer = NULL;
	W.alarm_due = 0;
	if (old) {
		DeleteTimerQueueTimer(NULL, old, NULL);
	}
	if (seconds > 0) {
		DWORD ms = (seconds > 0x7FFFFFFF / 1000) ? 0x7FFFFFFF : (DWORD) (seconds * 1000);
		W.alarm_due = now + ms;
		if (!CreateTimerQueueTimer(&W.alarm_timer, NULL, pcntl_alarm_callback,
				(PVOID) (intptr_t) W.alarm_gen, ms, 0, WT_EXECUTEONLYONCE)) {
			W.alarm_timer = NULL;
			W.alarm_due = 0;
		}
	}
	return remaining;
}

/* ------------------------------------------------------------------------ */
/* Children                                                                 */
/* ------------------------------------------------------------------------ */

/* lock must be held: record the exit and queue SIGCHLD exactly once, whichever side noticed first */
static void pcntl_child_mark_exited_locked(pcntl_child *c)
{
	pcntl_sig s;
	int status;

	if (!c->exited) {
		DWORD code = 0;
		if (!GetExitCodeProcess(c->hproc, &code)) {
			code = 0;
		}
		c->exited = 1;
		c->exit_code = code;
	}
	if (c->chld_sent) {
		return;
	}
	c->chld_sent = 1;

	status = pcntl_exitcode_to_status(c->exit_code);
	memset(&s, 0, sizeof(s));
	s.signo = PSIG_CHLD;
	s.pid = c->pid;
	if (PCNTL_WIFSIGNALED(status)) {
		s.code = PCNTL_CLD_KILLED;
		s.status = PCNTL_WTERMSIG(status);
	} else {
		s.code = PCNTL_CLD_EXITED;
		s.status = PCNTL_WEXITSTATUS(status);
	}
	pcntl_deliver_locked(&s); /* SIGCHLD can never be fatal */
	SetEvent(W.wake);
}

static VOID CALLBACK pcntl_child_exit_callback(PVOID param, BOOLEAN timed_out)
{
	pcntl_child *c = (pcntl_child *) param, *it;
	(void) timed_out;

	LOCK();
	/* the entry may already have been reaped and unlinked; it is only freed after this callback completes */
	for (it = W.children; it; it = it->next) {
		if (it == c) {
			pcntl_child_mark_exited_locked(c);
			break;
		}
	}
	UNLOCK();
}

/* lock must be held */
static pcntl_child *pcntl_child_add_locked(DWORD pid, HANDLE hproc, int adopted)
{
	pcntl_child *c = calloc(1, sizeof(*c));
	if (!c) {
		return NULL;
	}
	c->pid = pid;
	c->hproc = hproc;
	c->adopted = adopted;
	c->next = W.children;
	W.children = c;
	RegisterWaitForSingleObject(&c->hwait, hproc, pcntl_child_exit_callback, c, INFINITE, WT_EXECUTEONLYONCE);
	return c;
}

static pcntl_child *pcntl_child_find_locked(DWORD pid)
{
	pcntl_child *c;
	for (c = W.children; c; c = c->next) {
		if (c->pid == pid) {
			return c;
		}
	}
	return NULL;
}

/* lock must be held: find the child, or adopt the pid so stop/continue state can be reported by waitpid() */
static pcntl_child *pcntl_child_track_locked(DWORD pid)
{
	pcntl_child *c = pcntl_child_find_locked(pid);
	if (!c) {
		HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (h) {
			c = pcntl_child_add_locked(pid, h, 1);
			if (!c) {
				CloseHandle(h);
			}
		}
	}
	return c;
}

/* lock must be held: take the entry out of the list; destroy it with pcntl_child_destroy() after unlocking */
static void pcntl_child_unlink_locked(pcntl_child *c, int reaped)
{
	pcntl_child **pp;
	for (pp = &W.children; *pp; pp = &(*pp)->next) {
		if (*pp == c) {
			*pp = c->next;
			break;
		}
	}
	if (reaped) {
		W.reaped[W.reaped_next++ % 256] = c->pid;
	}
}

/* must be called WITHOUT the lock: waits for a pending exit callback so the memory can be freed safely */
static void pcntl_child_destroy(pcntl_child *c)
{
	if (c->hwait) {
		UnregisterWaitEx(c->hwait, INVALID_HANDLE_VALUE);
	}
	CloseHandle(c->hproc);
	free(c);
}

static int pcntl_was_reaped_locked(DWORD pid)
{
	unsigned i;
	for (i = 0; i < 256; i++) {
		if (W.reaped[i] == pid) {
			return 1;
		}
	}
	return 0;
}

/* lock must be held: adopt every live direct child of this process (proc_open(), popen(), ...) so wait(-1) sees them */
static void pcntl_adopt_direct_children_locked(void)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32W pe;
	DWORD me = GetCurrentProcessId();

	if (snap == INVALID_HANDLE_VALUE) {
		return;
	}
	pe.dwSize = sizeof(pe);
	if (Process32FirstW(snap, &pe)) {
		do {
			if (pe.th32ParentProcessID != me || pe.th32ProcessID == me) {
				continue;
			}
			if (pcntl_child_find_locked(pe.th32ProcessID) || pcntl_was_reaped_locked(pe.th32ProcessID)) {
				continue;
			}
			{
				HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
				FILETIME ct, x;
				if (!h) {
					continue;
				}
				/* a recycled pid whose "parent" is a stale pid must have started before we did */
				if (!GetProcessTimes(h, &ct, &x, &x, &x) || CompareFileTime(&ct, &W.own_start) < 0) {
					CloseHandle(h);
					continue;
				}
				if (!pcntl_child_add_locked(pe.th32ProcessID, h, 1)) {
					CloseHandle(h);
				}
			}
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
}

static void pcntl_notify_child_state(DWORD pid, int cld_code, int sig)
{
	pcntl_sig s;
	memset(&s, 0, sizeof(s));
	s.signo = PSIG_CHLD;
	s.code = cld_code;
	s.pid = pid;
	s.status = sig;
	pcntl_deliver(&s);
	SetEvent(W.wake);
}

typedef struct {
	zend_long pid;
	int status;
	int kind;        /* 0 exited, 1 stopped, 2 continued */
	FILETIME kernel_time, user_time;
	SIZE_T peak_rss;
} pcntl_wait_result;

enum { WANT_EXITED = 1, WANT_STOPPED = 2, WANT_CONTINUED = 4, WANT_NOHANG = 8, WANT_NOWAIT = 16 };

typedef BOOL (WINAPI *K32GetProcessMemoryInfo_t)(HANDLE, void *, DWORD);

static void pcntl_fill_times(pcntl_child *c, pcntl_wait_result *r)
{
	FILETIME ct, et;
	struct {
		DWORD cb;
		DWORD PageFaultCount;
		SIZE_T PeakWorkingSetSize;
		SIZE_T WorkingSetSize;
		SIZE_T QuotaPeakPagedPoolUsage;
		SIZE_T QuotaPagedPoolUsage;
		SIZE_T QuotaPeakNonPagedPoolUsage;
		SIZE_T QuotaNonPagedPoolUsage;
		SIZE_T PagefileUsage;
		SIZE_T PeakPagefileUsage;
	} pmc;
	static K32GetProcessMemoryInfo_t pGetProcessMemoryInfo;
	static int resolved;

	GetProcessTimes(c->hproc, &ct, &et, &r->kernel_time, &r->user_time);
	if (!resolved) {
		HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
		if (k32) {
			pGetProcessMemoryInfo = (K32GetProcessMemoryInfo_t) GetProcAddress(k32, "K32GetProcessMemoryInfo");
		}
		resolved = 1;
	}
	memset(&pmc, 0, sizeof(pmc));
	pmc.cb = sizeof(pmc);
	if (pGetProcessMemoryInfo && pGetProcessMemoryInfo(c->hproc, &pmc, sizeof(pmc))) {
		r->peak_rss = pmc.PeakWorkingSetSize;
	}
}

/*
 * Core of pcntl_wait(), pcntl_waitpid() and pcntl_waitid().
 * Returns 1 with *r filled, 0 if WNOHANG and nothing changed, -1 with *err set.
 */
static int pcntl_wait_core(zend_long pid, int want, pcntl_wait_result *r, int *err)
{
	InterlockedExchange(&W.eintr, 0);

	if (pid > 0) {
		LOCK();
		if (pid == (zend_long) GetCurrentProcessId() || pcntl_was_reaped_locked((DWORD) pid)) {
			UNLOCK();
			*err = ECHILD;
			return -1;
		}
		if (!pcntl_child_find_locked((DWORD) pid)) {
			HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
			if (!h) {
				UNLOCK();
				*err = ECHILD;
				return -1;
			}
			if (!pcntl_child_add_locked((DWORD) pid, h, 1)) {
				CloseHandle(h);
				UNLOCK();
				*err = ENOMEM;
				return -1;
			}
		}
		UNLOCK();
	}

	for (;;) {
		pcntl_child *c, *hit = NULL;
		HANDLE hs[MAXIMUM_WAIT_OBJECTS];
		int nh = 0, any = 0, kind = 0;
		DWORD wr;

		LOCK();
		if (pid <= 0) {
			pcntl_adopt_direct_children_locked();
		}
		hs[nh++] = W.wake;
		for (c = W.children; c; c = c->next) {
			if (pid > 0 && c->pid != (DWORD) pid) {
				continue;
			}
			any++;
			if (!c->exited && WaitForSingleObject(c->hproc, 0) == WAIT_OBJECT_0) {
				pcntl_child_mark_exited_locked(c);
			}
			if (c->exited) {
				if (want & WANT_EXITED) { hit = c; kind = 0; break; }
				continue;
			}
			if ((want & WANT_STOPPED) && c->stopped && !c->stop_reported) { hit = c; kind = 1; break; }
			if ((want & WANT_CONTINUED) && c->continued) { hit = c; kind = 2; break; }
			if (nh < MAXIMUM_WAIT_OBJECTS) {
				hs[nh++] = c->hproc;
			}
		}

		if (hit) {
			memset(r, 0, sizeof(*r));
			r->pid = hit->pid;
			r->kind = kind;
			if (kind == 0) {
				r->status = pcntl_exitcode_to_status(hit->exit_code);
				pcntl_fill_times(hit, r);
				if (!(want & WANT_NOWAIT)) {
					pcntl_child_unlink_locked(hit, 1);
					UNLOCK();
					pcntl_child_destroy(hit);
					return 1;
				}
			} else if (kind == 1) {
				r->status = 0x7f | (hit->stop_sig << 8);
				if (!(want & WANT_NOWAIT)) {
					hit->stop_reported = 1;
				}
			} else {
				r->status = 0xffff;
				if (!(want & WANT_NOWAIT)) {
					hit->continued = 0;
				}
			}
			UNLOCK();
			return 1;
		}

		if (!any) {
			UNLOCK();
			*err = ECHILD;
			return -1;
		}
		UNLOCK();

		if (want & WANT_NOHANG) {
			return 0;
		}

		wr = WaitForMultipleObjects(nh, hs, FALSE, nh >= MAXIMUM_WAIT_OBJECTS ? 50 : INFINITE);
		(void) wr;
		if (InterlockedExchange(&W.eintr, 0)) {
			*err = EINTR;
			return -1;
		}
	}
}

static void pcntl_rusage_to_array(const pcntl_wait_result *r, zval *arr)
{
	ULARGE_INTEGER u, k;
	u.LowPart = r->user_time.dwLowDateTime;  u.HighPart = r->user_time.dwHighDateTime;
	k.LowPart = r->kernel_time.dwLowDateTime; k.HighPart = r->kernel_time.dwHighDateTime;

	add_assoc_long(arr, "ru_oublock", 0);
	add_assoc_long(arr, "ru_inblock", 0);
	add_assoc_long(arr, "ru_msgsnd", 0);
	add_assoc_long(arr, "ru_msgrcv", 0);
	add_assoc_long(arr, "ru_maxrss", (zend_long) (r->peak_rss / 1024));
	add_assoc_long(arr, "ru_ixrss", 0);
	add_assoc_long(arr, "ru_idrss", 0);
	add_assoc_long(arr, "ru_minflt", 0);
	add_assoc_long(arr, "ru_majflt", 0);
	add_assoc_long(arr, "ru_nsignals", 0);
	add_assoc_long(arr, "ru_nvcsw", 0);
	add_assoc_long(arr, "ru_nivcsw", 0);
	add_assoc_long(arr, "ru_nswap", 0);
	add_assoc_long(arr, "ru_utime.tv_usec", (zend_long) ((u.QuadPart / 10) % 1000000));
	add_assoc_long(arr, "ru_utime.tv_sec", (zend_long) (u.QuadPart / 10000000));
	add_assoc_long(arr, "ru_stime.tv_usec", (zend_long) ((k.QuadPart / 10) % 1000000));
	add_assoc_long(arr, "ru_stime.tv_sec", (zend_long) (k.QuadPart / 10000000));
}

/* ------------------------------------------------------------------------ */
/* Sending signals (posix_kill)                                             */
/* ------------------------------------------------------------------------ */

static HANDLE pcntl_open_process(DWORD pid, DWORD access, int *err)
{
	HANDLE h = OpenProcess(access, FALSE, pid);
	if (!h) {
		*err = pcntl_win_errno(GetLastError());
	}
	return h;
}

static bool pcntl_process_alive(DWORD pid, int *err)
{
	DWORD code = 0;
	HANDLE h = pcntl_open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION, err);
	if (!h) {
		if (*err == EPERM) {
			return true; /* it exists, we just cannot touch it (kill(pid, 0) semantics) */
		}
		return false;
	}
	if (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE) {
		CloseHandle(h);
		*err = ESRCH;
		return false;
	}
	CloseHandle(h);
	return true;
}

static bool pcntl_kill_one(DWORD pid, int signo, int *err)
{
	HANDLE h;
	int sent;

	if (signo == 0) {
		return pcntl_process_alive(pid, err);
	}

	if (pid == GetCurrentProcessId() && signo != PSIG_KILL && signo != PSIG_STOP) {
		pcntl_sig s;
		memset(&s, 0, sizeof(s));
		s.signo = signo;
		s.code = PCNTL_SI_USER;
		s.pid = pid;
		if (pcntl_deliver(&s) == ACT_TERMINATE) {
			pcntl_terminate_self(signo);
		}
		return true;
	}

	switch (signo) {
		case PSIG_KILL:
			h = pcntl_open_process(pid, PROCESS_TERMINATE, err);
			if (!h) {
				return false;
			}
			if (!TerminateProcess(h, PCNTL_SIGNAL_EXIT_CODE(signo))) {
				*err = pcntl_win_errno(GetLastError());
				CloseHandle(h);
				return *err == ESRCH ? false : false;
			}
			CloseHandle(h);
			return true;

		case PSIG_STOP:
			if (!pNtSuspendProcess) {
				*err = ENOSYS;
				return false;
			}
			h = pcntl_open_process(pid, PROCESS_SUSPEND_RESUME, err);
			if (!h) {
				return false;
			}
			if (pNtSuspendProcess(h) < 0) {
				*err = EPERM;
				CloseHandle(h);
				return false;
			}
			CloseHandle(h);
			LOCK();
			{
				pcntl_child *c = pcntl_child_track_locked(pid);
				if (c) {
					c->stopped = 1;
					c->stop_sig = signo;
					c->stop_reported = 0;
					c->continued = 0;
				}
				UNLOCK();
				if (c) {
					pcntl_notify_child_state(pid, PCNTL_CLD_STOPPED, signo);
				}
			}
			return true;

		case PSIG_CONT:
			if (pNtResumeProcess) {
				h = pcntl_open_process(pid, PROCESS_SUSPEND_RESUME, err);
				if (!h) {
					return false;
				}
				pNtResumeProcess(h);
				CloseHandle(h);
				LOCK();
				{
					pcntl_child *c = pcntl_child_track_locked(pid);
					int was_stopped = 0;
					if (c && c->stopped) {
						was_stopped = 1;
						c->stopped = 0;
						c->continued = 1;
					}
					UNLOCK();
					if (was_stopped) {
						pcntl_notify_child_state(pid, PCNTL_CLD_CONTINUED, signo);
					}
				}
			}
			/* also let a handler in the target run */
			sent = pcntl_pipe_send(pid, signo, 0, 0, err);
			if (sent < 0) {
				return false;
			}
			return true;

		default:
			break;
	}

	sent = pcntl_pipe_send(pid, signo, 0, 0, err);
	if (sent == 1) {
		return true;
	}
	if (sent < 0) {
		return false;
	}

	/* No listener in the target: apply the default disposition from the outside. */
	switch (pcntl_default_action(signo)) {
		case DEF_IGN:
		case DEF_CONT:
			return pcntl_process_alive(pid, err);
		case DEF_STOP:
			return pcntl_kill_one(pid, PSIG_STOP, err);
		default:
			h = pcntl_open_process(pid, PROCESS_TERMINATE, err);
			if (!h) {
				return false;
			}
			if (!TerminateProcess(h, PCNTL_SIGNAL_EXIT_CODE(signo))) {
				*err = pcntl_win_errno(GetLastError());
				CloseHandle(h);
				return false;
			}
			CloseHandle(h);
			return true;
	}
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                */
/* ------------------------------------------------------------------------ */

static PHP_GINIT_FUNCTION(pcntl)
{
#if defined(COMPILE_DL_PCNTL) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	memset(pcntl_globals, 0, sizeof(*pcntl_globals));
}

static void pcntl_read_respawn_env(void)
{
	char buf[64];
	DWORD n;

	n = GetEnvironmentVariableA("PHP_PCNTL_FORK_CHILD_OF", buf, sizeof(buf));
	if (n > 0 && n < sizeof(buf)) {
		W.replay_k = ZEND_STRTOL(buf, NULL, 10);
	}
	n = GetEnvironmentVariableA("PHP_PCNTL_FORK_PPID", buf, sizeof(buf));
	if (n > 0 && n < sizeof(buf)) {
		W.ppid_hint = ZEND_STRTOL(buf, NULL, 10);
	}
	n = GetEnvironmentVariableA("PHP_PCNTL_FORK_RETURNS", NULL, 0);
	if (n > 1) {
		char *list = malloc(n), *p, *end;
		if (list && GetEnvironmentVariableA("PHP_PCNTL_FORK_RETURNS", list, n) > 0) {
			int count = 1, i = 0;
			for (p = list; *p; p++) {
				if (*p == ',') count++;
			}
			W.replay_returns = calloc((size_t) count, sizeof(zend_long));
			if (W.replay_returns) {
				p = list;
				while (p && *p && i < count) {
					W.replay_returns[i++] = ZEND_STRTOL(p, &end, 10);
					p = (*end == ',') ? end + 1 : NULL;
				}
				W.replay_count = i;
			}
		}
		free(list);
	}
	/* make sure nothing we start through other means (proc_open etc.) inherits these */
	SetEnvironmentVariableA("PHP_PCNTL_FORK_CHILD_OF", NULL);
	SetEnvironmentVariableA("PHP_PCNTL_FORK_RETURNS", NULL);
	SetEnvironmentVariableA("PHP_PCNTL_FORK_PPID", NULL);
	_putenv("PHP_PCNTL_FORK_CHILD_OF=");
	_putenv("PHP_PCNTL_FORK_RETURNS=");
	_putenv("PHP_PCNTL_FORK_PPID=");
}

PHP_MINIT_FUNCTION(pcntl)
{
	const char *mode;

	REGISTER_INI_ENTRIES();

	memset(&W, 0, sizeof(W));
	InitializeCriticalSection(&W.lock);
	W.lock_ready = 1;
	W.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	W.ack = CreateEventW(NULL, FALSE, FALSE, NULL);
	{
		FILETIME x;
		GetProcessTimes(GetCurrentProcess(), &W.own_start, &x, &x, &x);
	}
	W.is_cli = sapi_module.name
		&& (strcmp(sapi_module.name, "cli") == 0 || strcmp(sapi_module.name, "cli-server") == 0 || strcmp(sapi_module.name, "phpdbg") == 0);
	pcntl_resolve_ntdll();

	mode = zend_ini_string_ex("pcntl.fork", sizeof("pcntl.fork") - 1, 0, NULL);
	W.fork_mode = (mode && _stricmp(mode, "respawn") == 0) ? FORK_RESPAWN : FORK_DISABLED;
	pcntl_read_respawn_env();

	QosClass_ce = register_class_Pcntl_QosClass();
	register_pcntl_symbols(module_number);

	if (zend_ini_long("pcntl.posix_functions", sizeof("pcntl.posix_functions") - 1, 0)
		&& !zend_hash_str_exists(CG(function_table), "posix_kill", sizeof("posix_kill") - 1)
		&& !zend_hash_str_exists(CG(function_table), "posix_getpid", sizeof("posix_getpid") - 1)
		&& !zend_hash_str_exists(CG(function_table), "posix_get_last_error", sizeof("posix_get_last_error") - 1)
		&& !zend_hash_str_exists(CG(function_table), "posix_strerror", sizeof("posix_strerror") - 1)) {
		zend_register_functions(NULL, posix_shim_functions, NULL, MODULE_PERSISTENT);
	}

	orig_interrupt_function = zend_interrupt_function;
	zend_interrupt_function = pcntl_interrupt_function;

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(pcntl)
{
	pcntl_child *c;

	if (zend_interrupt_function == pcntl_interrupt_function) {
		zend_interrupt_function = orig_interrupt_function;
	}
	pcntl_alarm_set(0);
	pcntl_stop_listener();
	if (W.ctrl_installed) {
		SetConsoleCtrlHandler(pcntl_console_ctrl_handler, FALSE);
		W.ctrl_installed = 0;
	}
	for (;;) {
		LOCK();
		c = W.children;
		if (c) {
			pcntl_child_unlink_locked(c, 0);
		}
		UNLOCK();
		if (!c) {
			break;
		}
		pcntl_child_destroy(c);
	}
	free(W.replay_returns);
	free(W.history);
	if (W.wake) CloseHandle(W.wake);
	if (W.ack) CloseHandle(W.ack);
	if (W.lock_ready) {
		DeleteCriticalSection(&W.lock);
		W.lock_ready = 0;
	}
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

PHP_RINIT_FUNCTION(pcntl)
{
	zend_long brk;

	php_add_tick_function(pcntl_signal_dispatch_tick_function, NULL);
	zend_hash_init(&PCNTL_G(php_signal_table), 16, NULL, ZVAL_PTR_DTOR, 0);
	PCNTL_G(async_signals) = 0;
	PCNTL_G(last_error) = 0;
	PCNTL_G(num_signals) = PCNTL_NSIG;
	PCNTL_G(processing_signal_queue) = 0;

	W.vm_interrupt = &EG(vm_interrupt);
	InterlockedExchange(&W.async, 0);
	brk = zend_ini_long("pcntl.ctrl_break_signal", sizeof("pcntl.ctrl_break_signal") - 1, 0);
	W.ctrl_break_signal = (brk > 0 && brk < PCNTL_NSIG) ? (int) brk : PSIG_QUIT;

	pcntl_install_console_handler();
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(pcntl)
{
	int i;

	zend_hash_destroy(&PCNTL_G(php_signal_table));

	/* Reset all signals to their default disposition */
	LOCK();
	for (i = 0; i < PCNTL_NSIG; i++) {
		W.dispo[i] = DISPO_DFL;
		W.restart[i] = 1;
	}
	W.blocked = 0;
	W.pending_mask = 0;
	W.ring_head = W.ring_count = 0;
	InterlockedExchange(&W.pending, 0);
	InterlockedExchange(&W.async, 0);
	UNLOCK();
	pcntl_alarm_set(0);
	return SUCCESS;
}

PHP_MINFO_FUNCTION(pcntl)
{
	char buf[128];

	php_info_print_table_start();
	php_info_print_table_row(2, "pcntl support", "enabled");
	php_info_print_table_row(2, "Windows emulation version", PHP_PCNTL_WIN_VERSION);
	php_info_print_table_row(2, "pcntl_fork() mode", W.fork_mode == FORK_RESPAWN ? "respawn" : "disabled (returns -1, ENOSYS)");
	snprintf(buf, sizeof(buf), "\\\\.\\pipe\\php-pcntl-%lu%s", (unsigned long) GetCurrentProcessId(), W.pipe ? " (listening)" : " (started on first pcntl_signal())");
	php_info_print_table_row(2, "Signal delivery pipe", buf);
	php_info_print_table_row(2, "Console events", W.ctrl_installed ? "Ctrl+C=SIGINT, Ctrl+Break=pcntl.ctrl_break_signal, close/logoff=SIGHUP, shutdown=SIGTERM" : "not attached (non-CLI SAPI)");
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}

/* ------------------------------------------------------------------------ */
/* Dispatch                                                                 */
/* ------------------------------------------------------------------------ */

/* fill an already initialised array (e.g. one returned by zend_try_array_init()) */
static void pcntl_sig_fill_array(const pcntl_sig *s, zval *arr)
{
	add_assoc_long_ex(arr, "signo", sizeof("signo") - 1, s->signo);
	add_assoc_long_ex(arr, "errno", sizeof("errno") - 1, 0);
	add_assoc_long_ex(arr, "code", sizeof("code") - 1, s->code);
	if (s->signo == PSIG_CHLD) {
		add_assoc_long_ex(arr, "status", sizeof("status") - 1, s->status);
		add_assoc_long_ex(arr, "pid", sizeof("pid") - 1, s->pid);
		add_assoc_long_ex(arr, "uid", sizeof("uid") - 1, 0);
	} else if (s->code == PCNTL_SI_USER || s->code == PCNTL_SI_QUEUE || s->signo == PSIG_USR1 || s->signo == PSIG_USR2) {
		add_assoc_long_ex(arr, "pid", sizeof("pid") - 1, s->pid);
		add_assoc_long_ex(arr, "uid", sizeof("uid") - 1, 0);
	}
	if (s->code == PCNTL_SI_QUEUE) {
		add_assoc_long_ex(arr, "value", sizeof("value") - 1, s->value);
	}
}

static void pcntl_sig_to_zval(const pcntl_sig *s, zval *arr)
{
	array_init(arr);
	pcntl_sig_fill_array(s, arr);
}

static void pcntl_signal_dispatch(void)
{
	zval params[2], *handle, retval;
	pcntl_sig *queue;
	unsigned count, i;
	zend_object *old_exception;
	const zend_op *old_opline_before_exception = NULL;
	const zend_op *old_opline = NULL;

	if (!W.pending) {
		return;
	}

	LOCK();
	if (!W.ring_count || PCNTL_G(processing_signal_queue)) {
		UNLOCK();
		return;
	}
	PCNTL_G(processing_signal_queue) = 1;
	count = W.ring_count;
	queue = emalloc(sizeof(pcntl_sig) * count);
	for (i = 0; i < count; i++) {
		queue[i] = W.ring[(W.ring_head + i) % PCNTL_RING_SIZE];
	}
	W.ring_head = W.ring_count = 0;
	InterlockedExchange(&W.pending, 0);
	UNLOCK();

	/* Prevent switching fibers when handling signals */
	zend_fiber_switch_block();

	/* Dispatching can happen with an exception pending (see upstream pcntl.c); set it aside while handlers run. */
	old_exception = EG(exception);
	if (old_exception) {
		if (EG(current_execute_data)) {
			old_opline = EG(current_execute_data)->opline;
		}
		old_opline_before_exception = EG(opline_before_exception);
		EG(exception) = NULL;
	}

	for (i = 0; i < count; i++) {
		bool handler_threw = false;

		if ((handle = zend_hash_index_find(&PCNTL_G(php_signal_table), queue[i].signo)) != NULL) {
			if (Z_TYPE_P(handle) != IS_LONG) {
				ZVAL_NULL(&retval);
				ZVAL_LONG(&params[0], queue[i].signo);
				pcntl_sig_to_zval(&queue[i], &params[1]);

				/* Call php signal handler - Note that we do not report errors, and we ignore the return value */
				call_user_function(NULL, NULL, handle, &retval, 2, params);
				zval_ptr_dtor(&retval);
				zval_ptr_dtor(&params[1]);
				handler_threw = NULL != EG(exception);
			}
		}

		if (handler_threw) {
			i++;
			break;
		}
	}

	if (old_exception) {
		if (EG(current_execute_data)) {
			EG(current_execute_data)->opline = old_opline;
		}
		EG(opline_before_exception) = old_opline_before_exception;
		if (EG(exception)) {
			zend_exception_set_previous(EG(exception), old_exception);
		} else {
			EG(exception) = old_exception;
		}
	}

	if (i < count) {
		/* Put back what the throwing handler did not get to, and ask the engine to come back later. */
		unsigned j;
		LOCK();
		for (j = count; j > i; j--) {
			pcntl_ring_push_front_locked(&queue[j - 1]);
		}
		pcntl_request_interrupt();
		UNLOCK();
	}
	efree(queue);

	PCNTL_G(processing_signal_queue) = 0;
	zend_fiber_switch_unblock();
	SetEvent(W.ack);
}

static void pcntl_signal_dispatch_tick_function(int dummy_int, void *dummy_pointer)
{
	(void) dummy_int;
	(void) dummy_pointer;
	pcntl_signal_dispatch();
}

static void pcntl_interrupt_function(zend_execute_data *execute_data)
{
	pcntl_signal_dispatch();
	if (orig_interrupt_function) {
		orig_interrupt_function(execute_data);
	}
}

/* ------------------------------------------------------------------------ */
/* pcntl_fork() / pcntl_exec()                                              */
/* ------------------------------------------------------------------------ */

static void pcntl_history_push(zend_long value)
{
	if (W.history_count == W.history_cap) {
		int cap = W.history_cap ? W.history_cap * 2 : 16;
		zend_long *n = realloc(W.history, (size_t) cap * sizeof(zend_long));
		if (!n) {
			return;
		}
		W.history = n;
		W.history_cap = cap;
	}
	W.history[W.history_count++] = value;
}

/* Build a UTF-16 environment block from the current environment plus overrides (NULL value removes). */
static wchar_t *pcntl_env_block(const wchar_t **names, const wchar_t **values, int n)
{
	wchar_t *cur = GetEnvironmentStringsW();
	size_t total = 0, len;
	const wchar_t *p;
	wchar_t *out, *o;
	int i;

	for (p = cur; *p; p += len + 1) {
		len = wcslen(p);
		total += len + 1;
	}
	for (i = 0; i < n; i++) {
		if (values[i]) {
			total += wcslen(names[i]) + 1 + wcslen(values[i]) + 1;
		}
	}
	out = emalloc((total + 2) * sizeof(wchar_t));
	o = out;
	for (p = cur; *p; p += len + 1) {
		const wchar_t *eq = wcschr(p, L'=');
		int skip = 0;
		len = wcslen(p);
		if (eq && eq != p) {
			for (i = 0; i < n; i++) {
				if ((size_t) (eq - p) == wcslen(names[i]) && _wcsnicmp(p, names[i], (size_t) (eq - p)) == 0) {
					skip = 1;
					break;
				}
			}
		}
		if (!skip) {
			memcpy(o, p, (len + 1) * sizeof(wchar_t));
			o += len + 1;
		}
	}
	for (i = 0; i < n; i++) {
		if (values[i]) {
			size_t nl = wcslen(names[i]), vl = wcslen(values[i]);
			memcpy(o, names[i], nl * sizeof(wchar_t)); o += nl;
			*o++ = L'=';
			memcpy(o, values[i], vl * sizeof(wchar_t)); o += vl;
			*o++ = 0;
		}
	}
	*o++ = 0;
	*o = 0;
	FreeEnvironmentStringsW(cur);
	return out;
}

static void pcntl_default_startupinfo(STARTUPINFOW *si)
{
	ZeroMemory(si, sizeof(*si));
	si->cb = sizeof(*si);
	si->dwFlags = STARTF_USESTDHANDLES;
	si->hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si->hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si->hStdError = GetStdHandle(STD_ERROR_HANDLE);
}

static zend_long pcntl_fork_respawn(void)
{
	wchar_t exe[MAX_PATH * 2];
	wchar_t *cmdline, *env;
	wchar_t kbuf[32], ppid[32];
	wchar_t *returns;
	size_t rl = 0, i;
	const wchar_t *names[3] = { L"PHP_PCNTL_FORK_CHILD_OF", L"PHP_PCNTL_FORK_RETURNS", L"PHP_PCNTL_FORK_PPID" };
	const wchar_t *values[3];
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	BOOL ok;

	if (!GetModuleFileNameW(NULL, exe, MAX_PATH * 2)) {
		PCNTL_G(last_error) = EINVAL;
		return -1;
	}
	cmdline = _wcsdup(GetCommandLineW());
	if (!cmdline) {
		PCNTL_G(last_error) = ENOMEM;
		return -1;
	}

	_snwprintf(kbuf, 32, L"%lld", (long long) W.fork_calls);
	_snwprintf(ppid, 32, L"%lu", (unsigned long) GetCurrentProcessId());
	returns = emalloc(((size_t) W.history_count * 24 + 1) * sizeof(wchar_t));
	returns[0] = 0;
	for (i = 0; i < (size_t) W.history_count; i++) {
		rl += (size_t) _snwprintf(returns + rl, 24, i ? L",%lld" : L"%lld", (long long) W.history[i]);
	}
	values[0] = kbuf;
	values[1] = W.history_count ? returns : NULL;
	values[2] = ppid;
	env = pcntl_env_block(names, values, 3);

	pcntl_default_startupinfo(&si);
	ZeroMemory(&pi, sizeof(pi));
	ok = CreateProcessW(exe, cmdline, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, env, NULL, &si, &pi);
	free(cmdline);
	efree(env);
	efree(returns);

	if (!ok) {
		PCNTL_G(last_error) = pcntl_win_errno(GetLastError());
		php_error_docref(NULL, E_WARNING, "Error %d: %s", PCNTL_G(last_error), strerror(PCNTL_G(last_error)));
		return -1;
	}
	CloseHandle(pi.hThread);
	LOCK();
	if (!pcntl_child_add_locked(pi.dwProcessId, pi.hProcess, 0)) {
		CloseHandle(pi.hProcess);
	}
	UNLOCK();
	return (zend_long) pi.dwProcessId;
}

/* {{{ Forks the currently running process following the same behavior as the UNIX fork() system call */
PHP_FUNCTION(pcntl_fork)
{
	zend_long id;

	ZEND_PARSE_PARAMETERS_NONE();

	if (W.fork_mode != FORK_RESPAWN || !W.is_cli) {
		PCNTL_G(last_error) = ENOSYS;
		php_error_docref(NULL, E_WARNING, "Error %d: Unimplemented (fork() does not exist on Windows; set pcntl.fork=respawn in php.ini to emulate it by re-running the script in a child process)", ENOSYS);
		RETURN_LONG(-1);
	}

	W.fork_calls++;

	if (W.replay_k > 0 && W.fork_calls <= W.replay_k) {
		if (W.fork_calls == W.replay_k) {
			/* this is the fork() call we were spawned for */
			id = 0;
		} else if (W.fork_calls - 1 < W.replay_count) {
			id = W.replay_returns[W.fork_calls - 1];
		} else {
			id = 0x7F000000 + W.fork_calls; /* unknown sibling: synthesise a pid-looking value */
		}
		pcntl_history_push(id);
		RETURN_LONG(id);
	}

	id = pcntl_fork_respawn();
	if (id > 0) {
		pcntl_history_push(id);
	}
	RETURN_LONG(id);
}
/* }}} */

/* Quote one argument following the rules CommandLineToArgvW / the MSVC CRT use. */
static void pcntl_append_quoted(wchar_t **buf, size_t *len, size_t *cap, const wchar_t *arg)
{
	size_t need = wcslen(arg) * 2 + 4, i, bs;
	int quote = (*arg == 0) || wcspbrk(arg, L" \t\n\v\"") != NULL;
	wchar_t *o;

	if (*len + need + 2 > *cap) {
		*cap = (*len + need + 2) * 2;
		*buf = erealloc(*buf, *cap * sizeof(wchar_t));
	}
	o = *buf + *len;
	if (*len) {
		*o++ = L' ';
	}
	if (!quote) {
		wcscpy(o, arg);
		o += wcslen(arg);
	} else {
		*o++ = L'"';
		for (i = 0; ; i++) {
			bs = 0;
			while (arg[i] == L'\\') { bs++; i++; }
			if (arg[i] == 0) {
				while (bs--) { *o++ = L'\\'; *o++ = L'\\'; }
				break;
			} else if (arg[i] == L'"') {
				while (bs--) { *o++ = L'\\'; *o++ = L'\\'; }
				*o++ = L'\\';
				*o++ = L'"';
			} else {
				while (bs--) { *o++ = L'\\'; }
				*o++ = arg[i];
			}
		}
		*o++ = L'"';
	}
	*o = 0;
	*len = (size_t) (o - *buf);
}

/* {{{ Executes specified program in current process space as defined by exec(2) */
PHP_FUNCTION(pcntl_exec)
{
	zval *args = NULL, *envs = NULL, *element;
	char *path;
	size_t path_len;
	wchar_t *wpath, *cmdline = NULL, *env = NULL, *app = NULL;
	size_t cl_len = 0, cl_cap = 0;
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	BOOL ok;
	DWORD code = 0, err;
	bool is_script;
	const wchar_t *ext;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_PATH(path, path_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY(args)
		Z_PARAM_ARRAY(envs)
	ZEND_PARSE_PARAMETERS_END();

	wpath = pcntl_utf8_to_wide(path, path_len);
	ext = wcsrchr(wpath, L'.');
	is_script = ext && (_wcsicmp(ext, L".bat") == 0 || _wcsicmp(ext, L".cmd") == 0);

	if (is_script) {
		wchar_t sysdir[MAX_PATH];
		GetSystemDirectoryW(sysdir, MAX_PATH);
		app = emalloc((wcslen(sysdir) + 16) * sizeof(wchar_t));
		wcscpy(app, sysdir);
		wcscat(app, L"\\cmd.exe");
		pcntl_append_quoted(&cmdline, &cl_len, &cl_cap, app);
		pcntl_append_quoted(&cmdline, &cl_len, &cl_cap, L"/c");
	}
	pcntl_append_quoted(&cmdline, &cl_len, &cl_cap, wpath);

	if (args) {
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(args), element) {
			zend_string *str = zval_try_get_string(element);
			wchar_t *w;
			if (!str) {
				efree(cmdline);
				efree(wpath);
				if (app) efree(app);
				RETURN_THROWS();
			}
			w = pcntl_utf8_to_wide(ZSTR_VAL(str), ZSTR_LEN(str));
			pcntl_append_quoted(&cmdline, &cl_len, &cl_cap, w);
			efree(w);
			zend_string_release(str);
		} ZEND_HASH_FOREACH_END();
	}

	if (envs) {
		/* exact environment, as execve(); SystemRoot is added when missing because Windows cannot start anything without it */
		zend_string *key;
		zend_ulong key_num;
		size_t total = 0, pos = 0;
		bool has_sysroot = false;
		wchar_t sysroot[MAX_PATH];
		DWORD sr = GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH);

		ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(envs), key_num, key, element) {
			zend_string *val = zval_try_get_string(element);
			if (!val) {
				efree(cmdline);
				efree(wpath);
				if (app) efree(app);
				RETURN_THROWS();
			}
			total += (key ? ZSTR_LEN(key) : 24) + 1 + ZSTR_LEN(val) + 1;
			zend_string_release(val);
		} ZEND_HASH_FOREACH_END();
		total += (sr ? sr : 0) + 16 + 2;
		env = emalloc(total * sizeof(wchar_t) + 2);

		ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(envs), key_num, key, element) {
			zend_string *val = zval_try_get_string(element);
			zend_string *k = key ? zend_string_copy(key) : zend_long_to_str((zend_long) key_num);
			wchar_t *wk = pcntl_utf8_to_wide(ZSTR_VAL(k), ZSTR_LEN(k));
			wchar_t *wv = pcntl_utf8_to_wide(ZSTR_VAL(val), ZSTR_LEN(val));
			if (_wcsicmp(wk, L"SystemRoot") == 0) {
				has_sysroot = true;
			}
			wcscpy(env + pos, wk); pos += wcslen(wk);
			env[pos++] = L'=';
			wcscpy(env + pos, wv); pos += wcslen(wv) + 1;
			efree(wk);
			efree(wv);
			zend_string_release(k);
			zend_string_release(val);
		} ZEND_HASH_FOREACH_END();
		if (!has_sysroot && sr) {
			wcscpy(env + pos, L"SystemRoot="); pos += 11;
			wcscpy(env + pos, sysroot); pos += wcslen(sysroot) + 1;
		}
		env[pos++] = 0;
		env[pos] = 0;
	}

	pcntl_default_startupinfo(&si);
	ZeroMemory(&pi, sizeof(pi));
	ok = CreateProcessW(app ? app : wpath, cmdline, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, env, NULL, &si, &pi);
	err = GetLastError();
	efree(cmdline);
	efree(wpath);
	if (app) efree(app);
	if (env) efree(env);

	if (!ok) {
		PCNTL_G(last_error) = pcntl_win_errno(err);
		php_error_docref(NULL, E_WARNING, "Error has occurred: (errno %d) %s", PCNTL_G(last_error), strerror(PCNTL_G(last_error)));
		RETURN_FALSE;
	}

	/* From here on we "are" the new program: wait for it and exit with its status, like exec() never returning. */
	CloseHandle(pi.hThread);
	InterlockedExchange(&W.exec_waiting, 1);
	fflush(stdout);
	fflush(stderr);
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	ExitProcess(code);
}
/* }}} */

/* ------------------------------------------------------------------------ */
/* Waiting                                                                  */
/* ------------------------------------------------------------------------ */

static void pcntl_wait_common(INTERNAL_FUNCTION_PARAMETERS, zend_long pid, zend_long options, zval *z_status, zval *z_rusage)
{
	pcntl_wait_result r;
	int err = 0, rc;
	int want = WANT_EXITED;
	zend_long status;

	if (options & PCNTL_WNOHANG) want |= WANT_NOHANG;
	if (options & PCNTL_WUNTRACED) want |= WANT_STOPPED;
	if (options & PCNTL_WCONTINUED) want |= WANT_CONTINUED;

	status = zval_get_long(z_status);

	if (z_rusage) {
		z_rusage = zend_try_array_init(z_rusage);
		if (!z_rusage) {
			RETURN_THROWS();
		}
	}

	rc = pcntl_wait_core(pid, want, &r, &err);
	if (rc < 0) {
		PCNTL_G(last_error) = err;
		ZEND_TRY_ASSIGN_REF_LONG(z_status, status);
		RETURN_LONG(-1);
	}
	if (rc == 0) {
		ZEND_TRY_ASSIGN_REF_LONG(z_status, status);
		RETURN_LONG(0);
	}
	if (z_rusage && r.kind == 0) {
		pcntl_rusage_to_array(&r, z_rusage);
	}
	ZEND_TRY_ASSIGN_REF_LONG(z_status, r.status);
	RETURN_LONG(r.pid);
}

/* {{{ Waits on or returns the status of a forked child as defined by the waitpid() system call */
PHP_FUNCTION(pcntl_waitpid)
{
	zend_long pid, options = 0;
	zval *z_status = NULL, *z_rusage = NULL;

	ZEND_PARSE_PARAMETERS_START(2, 4)
		Z_PARAM_LONG(pid)
		Z_PARAM_ZVAL(z_status)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(options)
		Z_PARAM_ZVAL(z_rusage)
	ZEND_PARSE_PARAMETERS_END();

	/* process groups do not exist here: 0 and -pgid mean "any child" */
	if (pid < -1 || pid == 0) {
		pid = -1;
	}
	pcntl_wait_common(INTERNAL_FUNCTION_PARAM_PASSTHRU, pid, options, z_status, z_rusage);
}
/* }}} */

/* {{{ Waits on or returns the status of a forked child as defined by the waitpid() system call */
PHP_FUNCTION(pcntl_wait)
{
	zend_long options = 0;
	zval *z_status = NULL, *z_rusage = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_ZVAL(z_status)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(options)
		Z_PARAM_ZVAL(z_rusage)
	ZEND_PARSE_PARAMETERS_END();

	pcntl_wait_common(INTERNAL_FUNCTION_PARAM_PASSTHRU, -1, options, z_status, z_rusage);
}
/* }}} */

PHP_FUNCTION(pcntl_waitid)
{
	zend_long idtype = PCNTL_P_ALL;
	zend_long id = 0;
	bool id_is_null = 1;
	zval *user_siginfo = NULL;
	zend_long options = PCNTL_WEXITED;
	pcntl_wait_result r;
	int want = 0, err = 0, rc;
	pcntl_sig s;

	ZEND_PARSE_PARAMETERS_START(0, 4)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(idtype)
		Z_PARAM_LONG_OR_NULL(id, id_is_null)
		Z_PARAM_ZVAL(user_siginfo)
		Z_PARAM_LONG(options)
	ZEND_PARSE_PARAMETERS_END();

	if (options & PCNTL_WEXITED) want |= WANT_EXITED;
	if (options & PCNTL_WSTOPPED) want |= WANT_STOPPED;
	if (options & PCNTL_WCONTINUED) want |= WANT_CONTINUED;
	if (options & PCNTL_WNOHANG) want |= WANT_NOHANG;
	if (options & PCNTL_WNOWAIT) want |= WANT_NOWAIT;

	if (!(want & (WANT_EXITED | WANT_STOPPED | WANT_CONTINUED)) || (idtype != PCNTL_P_ALL && idtype != PCNTL_P_PID && idtype != PCNTL_P_PGID)) {
		PCNTL_G(last_error) = EINVAL;
		RETURN_FALSE;
	}

	rc = pcntl_wait_core(idtype == PCNTL_P_PID && !id_is_null ? id : -1, want, &r, &err);
	if (rc < 0) {
		PCNTL_G(last_error) = err;
		RETURN_FALSE;
	}

	memset(&s, 0, sizeof(s));
	if (rc > 0) {
		s.signo = PSIG_CHLD;
		s.pid = r.pid;
		if (r.kind == 1) {
			s.code = PCNTL_CLD_STOPPED;
			s.status = PCNTL_WSTOPSIG(r.status);
		} else if (r.kind == 2) {
			s.code = PCNTL_CLD_CONTINUED;
			s.status = PSIG_CONT;
		} else if (PCNTL_WIFSIGNALED(r.status)) {
			s.code = PCNTL_CLD_KILLED;
			s.status = PCNTL_WTERMSIG(r.status);
		} else {
			s.code = PCNTL_CLD_EXITED;
			s.status = PCNTL_WEXITSTATUS(r.status);
		}
	}
	if (user_siginfo) {
		user_siginfo = zend_try_array_init(user_siginfo);
		if (!user_siginfo) {
			RETURN_THROWS();
		}
		if (rc > 0) {
			pcntl_sig_fill_array(&s, user_siginfo);
		} else {
			/* nothing happened (WNOHANG): zeroed info like the kernel returns */
			add_assoc_long_ex(user_siginfo, "signo", sizeof("signo") - 1, 0);
			add_assoc_long_ex(user_siginfo, "errno", sizeof("errno") - 1, 0);
			add_assoc_long_ex(user_siginfo, "code", sizeof("code") - 1, 0);
			add_assoc_long_ex(user_siginfo, "status", sizeof("status") - 1, 0);
			add_assoc_long_ex(user_siginfo, "pid", sizeof("pid") - 1, 0);
			add_assoc_long_ex(user_siginfo, "uid", sizeof("uid") - 1, 0);
		}
	}
	RETURN_TRUE;
}

#define PCNTL_STATUS_FUNCTION(name, expr) \
	PHP_FUNCTION(name) \
	{ \
		zend_long status_word; \
		ZEND_PARSE_PARAMETERS_START(1, 1) \
			Z_PARAM_LONG(status_word) \
		ZEND_PARSE_PARAMETERS_END(); \
		{ int s = (int) status_word; expr; } \
	}

PCNTL_STATUS_FUNCTION(pcntl_wifexited, RETURN_BOOL(PCNTL_WIFEXITED(s)))
PCNTL_STATUS_FUNCTION(pcntl_wifstopped, RETURN_BOOL(PCNTL_WIFSTOPPED(s)))
PCNTL_STATUS_FUNCTION(pcntl_wifsignaled, RETURN_BOOL(PCNTL_WIFSIGNALED(s)))
PCNTL_STATUS_FUNCTION(pcntl_wifcontinued, RETURN_BOOL(PCNTL_WIFCONTINUED(s)))
PCNTL_STATUS_FUNCTION(pcntl_wexitstatus, RETURN_LONG(PCNTL_WEXITSTATUS(s)))
PCNTL_STATUS_FUNCTION(pcntl_wtermsig, RETURN_LONG(PCNTL_WTERMSIG(s)))
PCNTL_STATUS_FUNCTION(pcntl_wstopsig, RETURN_LONG(PCNTL_WSTOPSIG(s)))

/* ------------------------------------------------------------------------ */
/* Signal handlers                                                          */
/* ------------------------------------------------------------------------ */

/* {{{ Assigns a system signal handler to a PHP function */
PHP_FUNCTION(pcntl_signal)
{
	zval *handle;
	zend_long signo;
	bool restart_syscalls = 1;
	bool restart_syscalls_is_null = 1;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_LONG(signo)
		Z_PARAM_ZVAL(handle)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL_OR_NULL(restart_syscalls, restart_syscalls_is_null)
	ZEND_PARSE_PARAMETERS_END();

	if (signo < 1) {
		zend_argument_value_error(1, "must be greater than or equal to 1");
		RETURN_THROWS();
	}

	if (signo >= PCNTL_G(num_signals)) {
		zend_argument_value_error(1, "must be less than %d", PCNTL_G(num_signals));
		RETURN_THROWS();
	}

	/* If restart_syscalls was not explicitly specified and the signal is SIGALRM, then default
	 * restart_syscalls to false (upstream keeps this differing default for BC). */
	if (restart_syscalls_is_null && signo == PSIG_ALRM) {
		restart_syscalls = 0;
	}

	/* Special long value case for SIG_DFL and SIG_IGN */
	if (Z_TYPE_P(handle) == IS_LONG) {
		if (Z_LVAL_P(handle) != PCNTL_SIG_DFL && Z_LVAL_P(handle) != PCNTL_SIG_IGN) {
			zend_argument_value_error(2, "must be either SIG_DFL or SIG_IGN when an integer value is given");
			RETURN_THROWS();
		}
		if (signo == PSIG_KILL || signo == PSIG_STOP) {
			PCNTL_G(last_error) = EINVAL;
			php_error_docref(NULL, E_WARNING, "Error assigning signal");
			RETURN_FALSE;
		}
		if (Z_LVAL_P(handle) == PCNTL_SIG_IGN) {
			pcntl_ensure_listener();
		}
		if (signo == PSIG_INT && W.is_cli) {
			/* the console-level "ignore Ctrl+C" flag is inherited from parents; keep it in sync with the disposition */
			SetConsoleCtrlHandler(NULL, Z_LVAL_P(handle) == PCNTL_SIG_IGN);
		}
		LOCK();
		W.dispo[signo] = (Z_LVAL_P(handle) == PCNTL_SIG_IGN) ? DISPO_IGN : DISPO_DFL;
		W.restart[signo] = restart_syscalls ? 1 : 0;
		UNLOCK();
		zend_hash_index_update(&PCNTL_G(php_signal_table), signo, handle);
		Z_TRY_ADDREF_P(handle);
		RETURN_TRUE;
	}

	if (!zend_is_callable_ex(handle, NULL, 0, NULL, NULL, NULL)) {
		PCNTL_G(last_error) = EINVAL;
		zend_argument_type_error(2, "must be of type callable|int, %s given", pcntl_zval_value_name(handle));
		RETURN_THROWS();
	}

	if (signo == PSIG_KILL || signo == PSIG_STOP) {
		PCNTL_G(last_error) = EINVAL;
		php_error_docref(NULL, E_WARNING, "Error assigning signal");
		RETURN_FALSE;
	}

	pcntl_ensure_listener();
	pcntl_install_console_handler();
	if (signo == PSIG_CHLD) {
		/* start watching children that already exist (proc_open() etc.) so their exit raises SIGCHLD */
		LOCK();
		pcntl_adopt_direct_children_locked();
		UNLOCK();
	}
	if (signo == PSIG_INT && W.is_cli) {
		/* a parent may have left Ctrl+C disabled for us (SetConsoleCtrlHandler(NULL, TRUE) is inherited); re-enable it */
		SetConsoleCtrlHandler(NULL, FALSE);
	}

	/* Add the function name to our signal table */
	handle = zend_hash_index_update(&PCNTL_G(php_signal_table), signo, handle);
	Z_TRY_ADDREF_P(handle);
	LOCK();
	W.dispo[signo] = DISPO_HANDLER;
	W.restart[signo] = restart_syscalls ? 1 : 0;
	UNLOCK();
	RETURN_TRUE;
}
/* }}} */

/* {{{ Gets signal handler */
PHP_FUNCTION(pcntl_signal_get_handler)
{
	zval *prev_handle;
	zend_long signo;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(signo)
	ZEND_PARSE_PARAMETERS_END();

	if (signo < 1 || signo > PCNTL_NSIG - 1) {
		zend_argument_value_error(1, "must be between 1 and %d", PCNTL_NSIG - 1);
		RETURN_THROWS();
	}

	if ((prev_handle = zend_hash_index_find(&PCNTL_G(php_signal_table), signo)) != NULL) {
		RETURN_COPY(prev_handle);
	} else {
		RETURN_LONG(PCNTL_SIG_DFL);
	}
}
/* }}} */

/* {{{ Dispatch signals to signal handlers */
PHP_FUNCTION(pcntl_signal_dispatch)
{
	ZEND_PARSE_PARAMETERS_NONE();

	pcntl_signal_dispatch();
	RETURN_TRUE;
}
/* }}} */

/* {{{ Enable/disable asynchronous signal handling and return the old setting. */
PHP_FUNCTION(pcntl_async_signals)
{
	bool on, on_is_null = 1;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL_OR_NULL(on, on_is_null)
	ZEND_PARSE_PARAMETERS_END();

	if (on_is_null) {
		RETURN_BOOL(PCNTL_G(async_signals));
	}

	RETVAL_BOOL(PCNTL_G(async_signals));
	PCNTL_G(async_signals) = on;
	InterlockedExchange(&W.async, on ? 1 : 0);
	if (on && W.pending) {
		pcntl_request_interrupt();
	}
}
/* }}} */

/* Common helper: build a signal bitmask from a PHP array of signal numbers */
static bool php_pcntl_set_user_signal_infos(HashTable *user_signals, uint32_t *set, size_t arg_num, bool allow_empty_signal_array)
{
	zval *user_signal_no;

	if (!allow_empty_signal_array && zend_hash_num_elements(user_signals) == 0) {
		pcntl_argument_must_not_be_empty_error((uint32_t) arg_num);
		return false;
	}

	*set = 0;
	ZEND_HASH_FOREACH_VAL(user_signals, user_signal_no) {
		bool failed;
		zend_long tmp = pcntl_zval_try_get_long(user_signal_no, &failed);

		if (failed) {
			zend_argument_type_error((uint32_t) arg_num, "signals must be of type int, %s given", pcntl_zval_value_name(user_signal_no));
			return false;
		}
		if (tmp < 1 || tmp >= PCNTL_G(num_signals)) {
			zend_argument_value_error((uint32_t) arg_num, "signals must be between 1 and %d", PCNTL_G(num_signals) - 1);
			return false;
		}
		*set |= SIGBIT((int) tmp);
	} ZEND_HASH_FOREACH_END();
	return true;
}

/* {{{ Examine and change blocked signals */
PHP_FUNCTION(pcntl_sigprocmask)
{
	zend_long how;
	HashTable *user_set;
	zval *user_old_set = NULL;
	uint32_t set, old_set;
	int act;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_LONG(how)
		Z_PARAM_ARRAY_HT(user_set)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(user_old_set)
	ZEND_PARSE_PARAMETERS_END();

	if (how != PCNTL_SIG_BLOCK && how != PCNTL_SIG_UNBLOCK && how != PCNTL_SIG_SETMASK) {
		zend_argument_value_error(1, "must be one of SIG_BLOCK, SIG_UNBLOCK, or SIG_SETMASK");
		RETURN_THROWS();
	}

	if (!php_pcntl_set_user_signal_infos(user_set, &set, 2, how == PCNTL_SIG_SETMASK)) {
		RETURN_FALSE;
	}
	set &= ~(SIGBIT(PSIG_KILL) | SIGBIT(PSIG_STOP));

	if (set) {
		pcntl_ensure_listener();
	}

	LOCK();
	old_set = W.blocked;
	switch (how) {
		case PCNTL_SIG_BLOCK:   W.blocked |= set; break;
		case PCNTL_SIG_UNBLOCK: W.blocked &= ~set; break;
		default:                W.blocked = set; break;
	}
	act = pcntl_flush_unblocked_locked(old_set);
	UNLOCK();
	if (act == ACT_TERMINATE) {
		pcntl_terminate_self(0);
	}

	if (user_old_set != NULL) {
		unsigned signal_no;
		user_old_set = zend_try_array_init(user_old_set);
		if (!user_old_set) {
			RETURN_THROWS();
		}
		for (signal_no = 1; signal_no < PCNTL_G(num_signals); ++signal_no) {
			if (old_set & SIGBIT(signal_no)) {
				add_next_index_long(user_old_set, signal_no);
			}
		}
	}

	RETURN_TRUE;
}
/* }}} */

/*
 * Shared implementation of sigwaitinfo()/sigtimedwait(). The requested set
 * is treated as blocked for the duration of the wait, so the usual
 * "sigprocmask(SIG_BLOCK) then sigwaitinfo()" pattern and the sloppier
 * "just sigwaitinfo()" pattern both work.
 */
static void pcntl_sigwait_common(INTERNAL_FUNCTION_PARAMETERS, HashTable *user_set, zval *user_siginfo, zend_long timeout_ms)
{
	uint32_t set, saved;
	ULONGLONG deadline = 0;
	int act;

	if (!php_pcntl_set_user_signal_infos(user_set, &set, 1, false)) {
		RETURN_FALSE;
	}
	pcntl_ensure_listener();
	InterlockedExchange(&W.eintr, 0);

	if (timeout_ms >= 0) {
		deadline = GetTickCount64() + (ULONGLONG) timeout_ms;
	}

	LOCK();
	saved = W.blocked;
	W.blocked |= set;
	for (;;) {
		uint32_t ready = W.pending_mask & set;
		if (ready) {
			int signo;
			pcntl_sig s;
			for (signo = 1; signo < PCNTL_NSIG; signo++) {
				if (ready & SIGBIT(signo)) {
					break;
				}
			}
			s = W.pending_info[signo];
			W.pending_mask &= ~SIGBIT(signo);
			W.blocked = saved;
			act = pcntl_flush_unblocked_locked(set | saved);
			UNLOCK();
			if (act == ACT_TERMINATE) {
				pcntl_terminate_self(0);
			}
			if (user_siginfo) {
				user_siginfo = zend_try_array_init(user_siginfo);
				if (!user_siginfo) {
					RETURN_THROWS();
				}
				pcntl_sig_fill_array(&s, user_siginfo);
			}
			RETURN_LONG(signo);
		}
		if (W.pending) {
			/* a handled signal outside the set arrived: behave like EINTR so the handler can run */
			W.blocked = saved;
			act = pcntl_flush_unblocked_locked(set | saved);
			UNLOCK();
			if (act == ACT_TERMINATE) {
				pcntl_terminate_self(0);
			}
			PCNTL_G(last_error) = EINTR;
			php_error_docref(NULL, E_WARNING, "%s", strerror(EINTR));
			RETURN_FALSE;
		}
		UNLOCK();

		{
			DWORD wait_ms = INFINITE;
			if (timeout_ms >= 0) {
				ULONGLONG now = GetTickCount64();
				if (now >= deadline) {
					LOCK();
					if (!(W.pending_mask & set)) {
						W.blocked = saved;
						act = pcntl_flush_unblocked_locked(set | saved);
						UNLOCK();
						if (act == ACT_TERMINATE) {
							pcntl_terminate_self(0);
						}
						PCNTL_G(last_error) = EAGAIN;
						RETURN_FALSE;
					}
					continue; /* lock held, loop re-checks */
				}
				wait_ms = (DWORD) (deadline - now);
			}
			WaitForSingleObject(W.wake, wait_ms);
		}
		LOCK();
	}
}

/* {{{ Synchronously wait for queued signals */
PHP_FUNCTION(pcntl_sigwaitinfo)
{
	HashTable *user_set;
	zval *user_siginfo = NULL;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_ARRAY_HT(user_set)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(user_siginfo)
	ZEND_PARSE_PARAMETERS_END();

	pcntl_sigwait_common(INTERNAL_FUNCTION_PARAM_PASSTHRU, user_set, user_siginfo, -1);
}
/* }}} */

/* {{{ Wait for queued signals */
PHP_FUNCTION(pcntl_sigtimedwait)
{
	HashTable *user_set;
	zval *user_siginfo = NULL;
	zend_long tv_sec = 0;
	zend_long tv_nsec = 0;
	zend_long ms;

	ZEND_PARSE_PARAMETERS_START(1, 4)
		Z_PARAM_ARRAY_HT(user_set)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(user_siginfo)
		Z_PARAM_LONG(tv_sec)
		Z_PARAM_LONG(tv_nsec)
	ZEND_PARSE_PARAMETERS_END();

	if (tv_sec < 0) {
		zend_argument_value_error(3, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (tv_nsec < 0 || tv_nsec >= 1000000000) {
		zend_argument_value_error(4, "must be between 0 and 1e9");
		RETURN_THROWS();
	}
	if (UNEXPECTED(tv_sec == 0 && tv_nsec == 0)) {
		zend_value_error("pcntl_sigtimedwait(): At least one of argument #3 ($seconds) or argument #4 ($nanoseconds) must be greater than 0");
		RETURN_THROWS();
	}

	ms = tv_sec * 1000 + (tv_nsec + 999999) / 1000000;
	if (ms > 0x7FFFFFFE) {
		ms = 0x7FFFFFFE;
	}
	pcntl_sigwait_common(INTERNAL_FUNCTION_PARAM_PASSTHRU, user_set, user_siginfo, ms);
}
/* }}} */

/* {{{ Set an alarm clock for delivery of a signal */
PHP_FUNCTION(pcntl_alarm)
{
	zend_long seconds;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(seconds)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_LONG(pcntl_alarm_set(seconds));
}
/* }}} */

/* {{{ Retrieve the error number set by the last pcntl function which failed. */
PHP_FUNCTION(pcntl_get_last_error)
{
	ZEND_PARSE_PARAMETERS_NONE();

	RETURN_LONG(PCNTL_G(last_error));
}
/* }}} */

/* {{{ Retrieve the system error message associated with the given errno. */
PHP_FUNCTION(pcntl_strerror)
{
	zend_long error;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(error)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_STRING(strerror((int) error));
}
/* }}} */

/* ------------------------------------------------------------------------ */
/* Priority (nice <-> Windows priority class, the mapping Cygwin uses)      */
/* ------------------------------------------------------------------------ */

static DWORD pcntl_nice_to_class(zend_long nice)
{
	if (nice < -13) return REALTIME_PRIORITY_CLASS;
	if (nice < -7)  return HIGH_PRIORITY_CLASS;
	if (nice < 0)   return ABOVE_NORMAL_PRIORITY_CLASS;
	if (nice == 0)  return NORMAL_PRIORITY_CLASS;
	if (nice <= 10) return BELOW_NORMAL_PRIORITY_CLASS;
	return IDLE_PRIORITY_CLASS;
}

static zend_long pcntl_class_to_nice(DWORD cls)
{
	switch (cls) {
		case REALTIME_PRIORITY_CLASS:     return -20;
		case HIGH_PRIORITY_CLASS:         return -13;
		case ABOVE_NORMAL_PRIORITY_CLASS: return -7;
		case BELOW_NORMAL_PRIORITY_CLASS: return 10;
		case IDLE_PRIORITY_CLASS:         return 19;
		default:                          return 0;
	}
}

/* {{{ Get the priority of any process */
PHP_FUNCTION(pcntl_getpriority)
{
	zend_long who = PCNTL_PRIO_PROCESS;
	zend_long pid;
	bool pid_is_null = 1;
	HANDLE h;
	DWORD cls;
	int err = 0;

	ZEND_PARSE_PARAMETERS_START(0, 2)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG_OR_NULL(pid, pid_is_null)
		Z_PARAM_LONG(who)
	ZEND_PARSE_PARAMETERS_END();

	if (who != PCNTL_PRIO_PROCESS) {
		zend_argument_value_error(2, "must be PRIO_PROCESS (PRIO_PGRP and PRIO_USER are not supported on Windows)");
		RETURN_THROWS();
	}

	pid = (pid_is_null || pid == 0) ? (zend_long) GetCurrentProcessId() : pid;
	h = pcntl_open_process((DWORD) pid, PROCESS_QUERY_LIMITED_INFORMATION, &err);
	if (!h) {
		PCNTL_G(last_error) = err;
		if (err == ESRCH) {
			php_error_docref(NULL, E_WARNING, "Error %d: No process was located using the given parameters", err);
		} else {
			php_error_docref(NULL, E_WARNING, "Error %d: %s", err, strerror(err));
		}
		RETURN_FALSE;
	}
	cls = GetPriorityClass(h);
	CloseHandle(h);
	if (!cls) {
		PCNTL_G(last_error) = EPERM;
		php_error_docref(NULL, E_WARNING, "Unknown error %d has occurred", EPERM);
		RETURN_FALSE;
	}
	RETURN_LONG(pcntl_class_to_nice(cls));
}
/* }}} */

/* {{{ Change the priority of any process */
PHP_FUNCTION(pcntl_setpriority)
{
	zend_long who = PCNTL_PRIO_PROCESS;
	zend_long pid;
	bool pid_is_null = 1;
	zend_long pri;
	HANDLE h;
	int err = 0;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_LONG(pri)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG_OR_NULL(pid, pid_is_null)
		Z_PARAM_LONG(who)
	ZEND_PARSE_PARAMETERS_END();

	if (who != PCNTL_PRIO_PROCESS) {
		zend_argument_value_error(3, "must be PRIO_PROCESS (PRIO_PGRP and PRIO_USER are not supported on Windows)");
		RETURN_THROWS();
	}

	pid = (pid_is_null || pid == 0) ? (zend_long) GetCurrentProcessId() : pid;
	h = pcntl_open_process((DWORD) pid, PROCESS_SET_INFORMATION, &err);
	if (!h) {
		PCNTL_G(last_error) = err;
		if (err == ESRCH) {
			php_error_docref(NULL, E_WARNING, "Error %d: No process was located using the given parameters", err);
		} else if (err == EPERM) {
			php_error_docref(NULL, E_WARNING, "Error %d: A process was located, but neither its effective nor real user ID matched the effective user ID of the caller", err);
		} else {
			php_error_docref(NULL, E_WARNING, "Unknown error %d has occurred", err);
		}
		RETURN_FALSE;
	}
	if (!SetPriorityClass(h, pcntl_nice_to_class(pri))) {
		err = pcntl_win_errno(GetLastError());
		CloseHandle(h);
		PCNTL_G(last_error) = err == EPERM ? EACCES : err;
		if (err == EPERM) {
			php_error_docref(NULL, E_WARNING, "Error %d: Only a super user may attempt to increase the process priority", EACCES);
		} else {
			php_error_docref(NULL, E_WARNING, "Unknown error %d has occurred", err);
		}
		RETURN_FALSE;
	}
	CloseHandle(h);
	RETURN_TRUE;
}
/* }}} */

/* ------------------------------------------------------------------------ */
/* CPU affinity                                                             */
/* ------------------------------------------------------------------------ */

PHP_FUNCTION(pcntl_getcpuaffinity)
{
	zend_long pid;
	bool pid_is_null = 1;
	HANDLE h;
	DWORD_PTR proc_mask = 0, sys_mask = 0;
	int err = 0, i;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG_OR_NULL(pid, pid_is_null)
	ZEND_PARSE_PARAMETERS_END();

	pid = (pid_is_null || pid == 0) ? (zend_long) GetCurrentProcessId() : pid;
	h = pcntl_open_process((DWORD) pid, PROCESS_QUERY_LIMITED_INFORMATION, &err);
	if (!h) {
		PCNTL_G(last_error) = err;
		if (err == ESRCH) {
			zend_argument_value_error(1, "invalid process (" ZEND_LONG_FMT ")", pid);
			RETURN_THROWS();
		}
		php_error_docref(NULL, E_WARNING, "Calling process not having the proper privileges");
		RETURN_FALSE;
	}
	if (!GetProcessAffinityMask(h, &proc_mask, &sys_mask)) {
		PCNTL_G(last_error) = pcntl_win_errno(GetLastError());
		CloseHandle(h);
		php_error_docref(NULL, E_WARNING, "Error %d", PCNTL_G(last_error));
		RETURN_FALSE;
	}
	CloseHandle(h);

	array_init(return_value);
	for (i = 0; i < (int) (sizeof(DWORD_PTR) * 8); i++) {
		if (proc_mask & ((DWORD_PTR) 1 << i)) {
			add_next_index_long(return_value, i);
		}
	}
}

PHP_FUNCTION(pcntl_setcpuaffinity)
{
	zend_long pid;
	bool pid_is_null = 1;
	zval *hmask = NULL, *ncpu;
	HANDLE h;
	DWORD_PTR proc_mask = 0, sys_mask = 0, mask = 0;
	zend_long maxcpus = 0;
	int err = 0, i;

	ZEND_PARSE_PARAMETERS_START(0, 2)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG_OR_NULL(pid, pid_is_null)
		Z_PARAM_ARRAY(hmask)
	ZEND_PARSE_PARAMETERS_END();

	if (!hmask || zend_hash_num_elements(Z_ARRVAL_P(hmask)) == 0) {
		pcntl_argument_must_not_be_empty_error(2);
		RETURN_THROWS();
	}

	pid = (pid_is_null || pid == 0) ? (zend_long) GetCurrentProcessId() : pid;
	h = pcntl_open_process((DWORD) pid, PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, &err);
	if (!h) {
		PCNTL_G(last_error) = err;
		if (err == ESRCH) {
			zend_argument_value_error(1, "invalid process (" ZEND_LONG_FMT ")", pid);
			RETURN_THROWS();
		}
		php_error_docref(NULL, E_WARNING, "Calling process not having the proper privileges");
		RETURN_FALSE;
	}
	GetProcessAffinityMask(h, &proc_mask, &sys_mask);
	for (i = 0; i < (int) (sizeof(DWORD_PTR) * 8); i++) {
		if (sys_mask & ((DWORD_PTR) 1 << i)) {
			maxcpus = i + 1;
		}
	}

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(hmask), ncpu) {
		zend_long cpu;
		ZVAL_DEREF(ncpu);
		if (Z_TYPE_P(ncpu) != IS_LONG) {
			if (Z_TYPE_P(ncpu) == IS_STRING) {
				zend_ulong tmp;
				if (!ZEND_HANDLE_NUMERIC(Z_STR_P(ncpu), tmp)) {
					CloseHandle(h);
					zend_argument_value_error(2, "cpu id invalid value (%s)", ZSTR_VAL(Z_STR_P(ncpu)));
					RETURN_THROWS();
				}
				cpu = (zend_long) tmp;
			} else {
				CloseHandle(h);
				zend_argument_type_error(2, "value must be of type int|string, %s given", pcntl_zval_value_name(ncpu));
				RETURN_THROWS();
			}
		} else {
			cpu = Z_LVAL_P(ncpu);
		}

		if (cpu < 0 || cpu >= maxcpus) {
			CloseHandle(h);
			zend_argument_value_error(2, "cpu id must be between 0 and " ZEND_LONG_FMT " (" ZEND_LONG_FMT ")", maxcpus - 1, cpu);
			RETURN_THROWS();
		}
		mask |= (DWORD_PTR) 1 << cpu;
	} ZEND_HASH_FOREACH_END();

	if (!SetProcessAffinityMask(h, mask)) {
		DWORD e = GetLastError();
		CloseHandle(h);
		PCNTL_G(last_error) = pcntl_win_errno(e);
		if (e == ERROR_INVALID_PARAMETER) {
			zend_argument_value_error(2, "invalid cpu affinity mask size or unmapped cpu id(s)");
			RETURN_THROWS();
		}
		if (e == ERROR_ACCESS_DENIED) {
			php_error_docref(NULL, E_WARNING, "Calling process not having the proper privileges");
		} else {
			php_error_docref(NULL, E_WARNING, "Error %d", PCNTL_G(last_error));
		}
		RETURN_FALSE;
	}
	CloseHandle(h);
	RETURN_TRUE;
}

PHP_FUNCTION(pcntl_getcpu)
{
	ZEND_PARSE_PARAMETERS_NONE();

	RETURN_LONG((zend_long) GetCurrentProcessorNumber());
}

/* ------------------------------------------------------------------------ */
/* QoS class (mapped onto Windows power throttling / EcoQoS)                */
/* ------------------------------------------------------------------------ */

typedef struct {
	ULONG Version;
	ULONG ControlMask;
	ULONG StateMask;
} pcntl_power_throttling_state;

#define PCNTL_PROCESS_POWER_THROTTLING 4
#define PCNTL_POWER_THROTTLING_EXECUTION_SPEED 1

typedef BOOL (WINAPI *SetProcessInformation_t)(HANDLE, int, LPVOID, DWORD);
typedef BOOL (WINAPI *GetProcessInformation_t)(HANDLE, int, LPVOID, DWORD);

static zend_object *qos_name_to_case(const char *name)
{
	return zend_enum_get_case_cstr(QosClass_ce, name);
}

PHP_FUNCTION(pcntl_getqos_class)
{
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	GetProcessInformation_t pGet = k32 ? (GetProcessInformation_t) GetProcAddress(k32, "GetProcessInformation") : NULL;
	pcntl_power_throttling_state st;
	const char *entry = "Default";

	ZEND_PARSE_PARAMETERS_NONE();

	memset(&st, 0, sizeof(st));
	st.Version = 1;
	if (pGet && pGet(GetCurrentProcess(), PCNTL_PROCESS_POWER_THROTTLING, &st, sizeof(st))) {
		if (st.ControlMask & PCNTL_POWER_THROTTLING_EXECUTION_SPEED) {
			entry = (st.StateMask & PCNTL_POWER_THROTTLING_EXECUTION_SPEED) ? "Background" : "UserInitiated";
		}
	}
	RETURN_OBJ_COPY(qos_name_to_case(entry));
}

PHP_FUNCTION(pcntl_setqos_class)
{
	zval *qos_obj = NULL;
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	SetProcessInformation_t pSet = k32 ? (SetProcessInformation_t) GetProcAddress(k32, "SetProcessInformation") : NULL;
	pcntl_power_throttling_state st;
	zend_string *entry;

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_OBJECT_OF_CLASS(qos_obj, QosClass_ce)
	ZEND_PARSE_PARAMETERS_END();

	memset(&st, 0, sizeof(st));
	st.Version = 1;
	if (qos_obj) {
		entry = Z_STR_P(zend_enum_fetch_case_name(Z_OBJ_P(qos_obj)));
		if (zend_string_equals_literal(entry, "Background") || zend_string_equals_literal(entry, "Utility")) {
			st.ControlMask = PCNTL_POWER_THROTTLING_EXECUTION_SPEED;
			st.StateMask = PCNTL_POWER_THROTTLING_EXECUTION_SPEED;
		} else if (zend_string_equals_literal(entry, "UserInteractive") || zend_string_equals_literal(entry, "UserInitiated")) {
			st.ControlMask = PCNTL_POWER_THROTTLING_EXECUTION_SPEED;
			st.StateMask = 0;
		}
	}

	if (!pSet || !pSet(GetCurrentProcess(), PCNTL_PROCESS_POWER_THROTTLING, &st, sizeof(st))) {
		PCNTL_G(last_error) = pSet ? pcntl_win_errno(GetLastError()) : ENOSYS;
		zend_throw_error(NULL, "pcntl_setqos_class failed");
		RETURN_THROWS();
	}
}

/* ------------------------------------------------------------------------ */
/* posix_* companions                                                       */
/* ------------------------------------------------------------------------ */

PHP_FUNCTION(posix_kill)
{
	zend_long pid, sig;
	int err = 0;
	bool ok;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_LONG(pid)
		Z_PARAM_LONG(sig)
	ZEND_PARSE_PARAMETERS_END();

	if (sig < 0 || sig >= PCNTL_NSIG) {
		PCNTL_G(last_error) = EINVAL;
		RETURN_FALSE;
	}

	if (pid == -1) {
		/* "every process we may signal" is not something we are willing to emulate */
		PCNTL_G(last_error) = EPERM;
		RETURN_FALSE;
	}

	if (pid == 0) {
		/* our "process group": ourselves and the children we spawned */
		pcntl_child *c;
		DWORD pids[256];
		int n = 0, i;
		LOCK();
		for (c = W.children; c && n < 256; c = c->next) {
			if (!c->exited) {
				pids[n++] = c->pid;
			}
		}
		UNLOCK();
		ok = true;
		for (i = 0; i < n; i++) {
			if (!pcntl_kill_one(pids[i], (int) sig, &err)) {
				ok = false;
			}
		}
		if (!pcntl_kill_one(GetCurrentProcessId(), (int) sig, &err)) {
			ok = false;
		}
		if (!ok) {
			PCNTL_G(last_error) = err;
		}
		RETURN_BOOL(ok);
	}

	if (pid < -1) {
		pid = -pid;
	}

	ok = pcntl_kill_one((DWORD) pid, (int) sig, &err);
	if (!ok) {
		PCNTL_G(last_error) = err;
	}
	RETURN_BOOL(ok);
}

PHP_FUNCTION(posix_getpid)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG((zend_long) GetCurrentProcessId());
}

PHP_FUNCTION(posix_getppid)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG(pcntl_parent_pid());
}

PHP_FUNCTION(posix_get_last_error)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG(PCNTL_G(last_error));
}

PHP_FUNCTION(posix_strerror)
{
	zend_long error;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(error)
	ZEND_PARSE_PARAMETERS_END();

	RETURN_STRING(strerror((int) error));
}
