/* Scheduler and task definitions (includes POSIX-like signal support). */
#pragma once
#include <types.h>
#include <sys/spinlock.h>

/* Forward declarations for percpu.h (breaks circular dependency) */
struct cpu_info;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- signals (kernel/signal.c) ---- */
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28

#define _NSIG 32

/* sigprocmask how */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* sa_flags */
#define SA_RESTART 0x10000000

struct sigaction {
    sighandler_t sa_handler;
    u64 sa_mask;
    int sa_flags;
};

#define SIG_TRAMPOLINE_VA 0x7fffff800000UL

/* ---- interrupt frame & task context ---- */
/* Interrupt frame pushed by the stubs in entry64.S (ascending addresses). */
struct intr_frame {
    u64 r15, r14, r13, r12;
    u64 r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rax;
    u64 rbx; /* callee-saved: must survive the C handler */
    u64 intno;
    u64 err;
    u64 rip, cs, rflags, ussp, usss;
};

/* Callee-saved context for swtch(): holds the saved-kernel-stack pointer.
 * The register save area itself lives on the task's kernel stack. */
struct cpu_ctx {
    u64 sp;
};

enum task_state {
    T_UNUSED,
    T_EMBRYO,
    T_RUNNABLE,
    T_RUNNING,
    T_SLEEPING,
    T_ZOMBIE,
    T_STOPPED,
};

#define TASK_NAME_LEN 16

struct task {
    u32 pid;
    u32 cpu_id; /* CPU running this task (-1 if none) */
    enum task_state state;
    char name[TASK_NAME_LEN];

    struct intr_frame *tf; /* points into kstack while in kernel */
    struct cpu_ctx ctx;
    char *kstack; /* kmalloc'd KSTACK_SIZE */
    char *kstack_top;

    u64 pml4;       /* CR3 physical */
    void *brk_base; /* user heap start (= end of segments) */
    void *brk_cur;

    struct file *fds[NR_FDS];

    int exit_code;
    u64 sleep_until;
    int quantum;

    struct task *parent;
    struct task *next;    /* all-tasks list */
    struct task *rq_next; /* runqueue link */
    int rq_cpu;           /* CPU whose runqueue holds this task; -1 = not queued */

    /* signal support */
    u64 signal_pending;              /* bitmask of pending signals */
    u64 signal_mask;                 /* bitmask of blocked signals */
    struct sigaction sa[NR_SIGNALS]; /* per-signal handlers */
};

/* current task macro: SMP mode reads from per-CPU data via GS. */
#ifdef CONFIG_SMP
#include <sys/percpu.h>
struct task *get_current(void);
#define current (get_current())
#else
extern struct task *current;
#endif

#define KSTACK_SIZE 32768

int sys_fork(void);
long sys_execve(const char *path, char *const argv[], char *const envp[]);
void sys_exit(int code) __attribute__((noreturn));
int sys_waitpid(int pid, int *status, int opts);
u32 next_pid(void);
struct task *find_task(u32 pid);
const char *task_state_name(enum task_state s);

int copy_from_user(void *kdst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *ksrc, size_t n);
/* True iff [p, p+n) lies in user space and every page is mapped. */
bool user_ptr_ok(u64 p, u64 n);
/* Copy a NUL-terminated string from user space (byte-wise validated).
 * Always NUL-terminates kdst. Returns 0 on success, -1 if any byte of the
 * source is outside the user range or not mapped. */
int copy_user_str(char *kdst, u64 usrc, size_t max);

/* scheduler (kernel/sched.c) */
void sched_init(void);
void schedule(void);
void sched_tick(void); /* PIT hook: quantum expiry */
void sched_maybe_preempt(struct intr_frame *f);
int sys_nanosleep(u64 ms);
struct task *task_iter(int *i);

/* signal helpers (kernel/signal.c) */
void send_signal(struct task *t, int sig);
void do_signal_check(struct task *t);
void signal_init_trampoline(u64 pml4);
long sys_sigreturn(void);
void runqueue_add(struct task *t);void runqueue_remove(struct task *t);
void idle_loop(void) __attribute__((noreturn));
struct task *task_alloc_slot(void);
void task_free_slot(struct task *t);
struct task *sched_create_idle(int cpu_id);

#ifdef __cplusplus
}
#endif
