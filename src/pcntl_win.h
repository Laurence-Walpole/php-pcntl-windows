/*
   +----------------------------------------------------------------------+
   | pcntl for Windows: an emulation of ext/pcntl on top of Win32          |
   +----------------------------------------------------------------------+
   | Parts of this extension (function signatures, argument handling,     |
   | error messages and dispatch logic) are derived from php-src          |
   | ext/pcntl, Copyright (c) The PHP Group, PHP License 3.01.            |
   | The Windows emulation layer is released under the MIT license.       |
   +----------------------------------------------------------------------+
*/

#ifndef PHP_PCNTL_WIN_H
#define PHP_PCNTL_WIN_H

extern zend_module_entry pcntl_module_entry;
#define phpext_pcntl_ptr &pcntl_module_entry

#define PHP_PCNTL_WIN_VERSION "1.0.0"
/* Reported as the module version so `php --ri pcntl` and Reflection see the real PHP version like upstream. */
#include "php_version.h"
#define PHP_PCNTL_VERSION PHP_VERSION

/*
 * Signal numbers. We use the Linux x86-64 numbering (the numbering most PHP
 * code was written against) rather than the MSVC CRT numbering, which only
 * knows six signals and gives SIGABRT the value 22. The six CRT signals that
 * exist (SIGINT=2, SIGILL=4, SIGFPE=8, SIGSEGV=11, SIGTERM=15) have the same
 * numbers on Linux, so nothing user-visible collides except SIGABRT.
 */
enum {
	PSIG_HUP    = 1,  PSIG_INT   = 2,  PSIG_QUIT   = 3,  PSIG_ILL   = 4,  PSIG_TRAP  = 5,
	PSIG_ABRT   = 6,  PSIG_BUS   = 7,  PSIG_FPE    = 8,  PSIG_KILL  = 9,  PSIG_USR1  = 10,
	PSIG_SEGV   = 11, PSIG_USR2  = 12, PSIG_PIPE   = 13, PSIG_ALRM  = 14, PSIG_TERM  = 15,
	PSIG_STKFLT = 16, PSIG_CHLD  = 17, PSIG_CONT   = 18, PSIG_STOP  = 19, PSIG_TSTP  = 20,
	PSIG_TTIN   = 21, PSIG_TTOU  = 22, PSIG_URG    = 23, PSIG_XCPU  = 24, PSIG_XFSZ  = 25,
	PSIG_VTALRM = 26, PSIG_PROF  = 27, PSIG_WINCH  = 28, PSIG_IO    = 29, PSIG_PWR   = 30,
	PSIG_SYS    = 31
};
#define PCNTL_NSIG 32 /* valid signals are 1..31 */

/* Handler dispositions (integer values are what PHP userland sees for SIG_DFL/SIG_IGN) */
#define PCNTL_SIG_DFL 0
#define PCNTL_SIG_IGN 1
#define PCNTL_SIG_ERR -1

/* sigprocmask() modes (Linux values) */
#define PCNTL_SIG_BLOCK   0
#define PCNTL_SIG_UNBLOCK 1
#define PCNTL_SIG_SETMASK 2

/* wait options (Linux values) */
#define PCNTL_WNOHANG    0x00000001
#define PCNTL_WUNTRACED  0x00000002
#define PCNTL_WSTOPPED   0x00000002
#define PCNTL_WEXITED    0x00000004
#define PCNTL_WCONTINUED 0x00000008
#define PCNTL_WNOWAIT    0x01000000

/* waitid() id types (Linux values) */
#define PCNTL_P_ALL  0
#define PCNTL_P_PID  1
#define PCNTL_P_PGID 2

/* siginfo code values (Linux values) */
#define PCNTL_SI_USER     0
#define PCNTL_SI_KERNEL   0x80
#define PCNTL_SI_QUEUE    (-1)
#define PCNTL_SI_TIMER    (-2)
#define PCNTL_SI_MESGQ    (-3)
#define PCNTL_SI_ASYNCIO  (-4)
#define PCNTL_SI_TKILL    (-6)
#define PCNTL_CLD_EXITED    1
#define PCNTL_CLD_KILLED    2
#define PCNTL_CLD_DUMPED    3
#define PCNTL_CLD_TRAPPED   4
#define PCNTL_CLD_STOPPED   5
#define PCNTL_CLD_CONTINUED 6

/* priority modes (Linux values) */
#define PCNTL_PRIO_PROCESS 0
#define PCNTL_PRIO_PGRP    1
#define PCNTL_PRIO_USER    2

/* wait status word encoding/decoding, identical to glibc */
#define PCNTL_WIFEXITED(s)    (((s) & 0x7f) == 0)
#define PCNTL_WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define PCNTL_WIFSIGNALED(s)  ((((s) & 0x7f) + 1) >= 2 && (((s) & 0x7f) + 1) <= 0x7f)
#define PCNTL_WTERMSIG(s)     ((s) & 0x7f)
#define PCNTL_WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define PCNTL_WSTOPSIG(s)     PCNTL_WEXITSTATUS(s)
#define PCNTL_WIFCONTINUED(s) ((s) == 0xffff)

/*
 * Exit code used when a process is terminated "by a signal" through this
 * extension (posix_kill() without a handler in the target, or a default
 * disposition in the receiving process). 128+signo is the convention shells
 * and Symfony Process already use, so other tooling understands it too.
 */
#define PCNTL_SIGNAL_EXIT_CODE(signo) (128u + (unsigned)(signo))

/* ensure the compat shims exist on older 8.x */
#if PHP_VERSION_ID < 80300
# define pcntl_zval_value_name(zv) zend_zval_type_name(zv)
#else
# define pcntl_zval_value_name(zv) zend_zval_value_name(zv)
#endif

#if PHP_VERSION_ID < 80300
static zend_always_inline zend_long pcntl_zval_try_get_long(zval *op, bool *failed)
{
	if (Z_TYPE_P(op) == IS_LONG) {
		*failed = false;
		return Z_LVAL_P(op);
	}
	*failed = !(Z_TYPE_P(op) == IS_DOUBLE || Z_TYPE_P(op) == IS_STRING || Z_TYPE_P(op) == IS_TRUE || Z_TYPE_P(op) == IS_FALSE || Z_TYPE_P(op) == IS_NULL);
	return *failed ? 0 : zval_get_long(op);
}
#else
# define pcntl_zval_try_get_long(op, failed) zval_try_get_long((op), (failed))
#endif

#if PHP_VERSION_ID < 80400
# define pcntl_argument_must_not_be_empty_error(n) zend_argument_value_error((n), "must not be empty")
#else
# define pcntl_argument_must_not_be_empty_error(n) zend_argument_must_not_be_empty_error(n)
#endif

ZEND_BEGIN_MODULE_GLOBALS(pcntl)
	HashTable php_signal_table;
	int processing_signal_queue;
	int last_error;
	bool async_signals;
	unsigned num_signals;
ZEND_END_MODULE_GLOBALS(pcntl)

#if defined(ZTS) && defined(COMPILE_DL_PCNTL)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

ZEND_EXTERN_MODULE_GLOBALS(pcntl)
#define PCNTL_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(pcntl, v)

#endif /* PHP_PCNTL_WIN_H */
