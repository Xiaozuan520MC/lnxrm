/* POSIX-like signal support. */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Signal handler function type */
typedef void (*sighandler_t)(int);

/* Standard signal numbers */
#define SIG_DFL     ((sighandler_t)0)
#define SIG_IGN     ((sighandler_t)1)

#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGSTKFLT   16
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19
#define SIGTSTP     20
#define SIGTTIN     21
#define SIGTTOU     22
#define SIGURG      23
#define SIGXCPU     24
#define SIGXFSZ     25
#define SIGVTALRM   26
#define SIGPROF     27
#define SIGWINCH    28

#define _NSIG       32

/* sigprocmask how */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* sa_flags */
#define SA_RESTART  0x10000000

struct sigaction {
    sighandler_t sa_handler;
    u64   sa_mask;
    int   sa_flags;
};

/* Signal trampoline code (user态, arch/signal_trampoline.S) */
extern u8 signal_trampoline_code[];
extern u8 signal_trampoline_end[];

#define SIG_TRAMPOLINE_VA  0x7fffff800000UL

/* Called from kernel/syscall.c on syscall return to check pending signals. */
struct task;
void do_signal_check(struct task *t);

/* Send signal `sig` to task `t`. */
void send_signal(struct task *t, int sig);

/* Install signal trampoline mapping in a process address space. */
void signal_init_trampoline(u64 pml4);

/* sys_sigreturn: restore from signal frame. */
long sys_sigreturn(void);

#ifdef __cplusplus
}
#endif
