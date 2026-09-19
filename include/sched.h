/* Scheduler and task definitions. */
#pragma once
#include <types.h>
#include <spinlock.h>
#include <signal.h>

/* Forward declarations for percpu.h (breaks circular dependency) */
struct cpu_info;

#ifdef __cplusplus
extern "C" {
#endif

/* Interrupt frame pushed by the stubs in entry64.S (ascending addresses). */
struct intr_frame {
    u64 r15, r14, r13, r12;
    u64 r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rax;
    u64 rbx;                        /* callee-saved: must survive the C handler */
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
    u32 cpu_id;                     /* CPU running this task (-1 if none) */
    enum task_state state;
    char name[TASK_NAME_LEN];

    struct intr_frame *tf;          /* points into kstack while in kernel */
    struct cpu_ctx ctx;
    char *kstack;                   /* kmalloc'd KSTACK_SIZE */
    char *kstack_top;

    u64 pml4;                       /* CR3 physical */
    void *brk_base;                 /* user heap start (= end of segments) */
    void *brk_cur;

    struct file *fds[NR_FDS];

    int exit_code;
    u64 sleep_until;
    int quantum;

    struct task *parent;
    struct task *next;              /* all-tasks list */
    struct task *rq_next;           /* runqueue link */

    /* signal support */
    u64 signal_pending;             /* bitmask of pending signals */
    u64 signal_mask;                /* bitmask of blocked signals */
    struct sigaction sa[NR_SIGNALS]; /* per-signal handlers */
};

/* current task macro: SMP mode reads from per-CPU data via GS. */
#ifdef CONFIG_SMP
#include <percpu.h>
struct task *get_current(void);
#define current (get_current())
#else
extern struct task *current;
#endif

#define KSTACK_SIZE 32768

void task_init(void);
int  sys_fork(void);
long sys_execve(const char *path, char *const argv[], char *const envp[]);
void sys_exit(int code) __attribute__((noreturn));
int  sys_waitpid(int pid, int *status, int opts);
u32  next_pid(void);
struct task *find_task(u32 pid);
struct task *task_by_pid(u32 pid);
void task_exit_notify(void);
const char *task_state_name(enum task_state s);

int copy_from_user(void *kdst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *ksrc, size_t n);

/* scheduler (kernel/sched.c) */
void sched_init(void);
void schedule(void);
void sched_tick(void);              /* PIT hook: quantum expiry */
void sched_maybe_preempt(struct intr_frame *f);
void yield(void);
int  sys_nanosleep(u64 ms);
struct task *task_iter(int *i);
void task_set_cpu(struct task *t, int cpu);
int  task_count(void);

/* signal helpers (kernel/signal.c) */
void send_signal(struct task *t, int sig);
void do_signal_check(struct task *t);
void signal_init_trampoline(u64 pml4);
long sys_sigreturn(void);
void runqueue_add(struct task *t);
void runqueue_remove(struct task *t);
void idle_loop(void) __attribute__((noreturn));


#ifdef __cplusplus
}
#endif
