/* System call dispatch (int 0x80, nr in rax, args rdi rsi rdx r10 r8). */
#include <syscall.h>
#include <sched.h>
#include <console.h>
#include <mm.h>
#include <vfs.h>
#include <cpu.h>
#include <signal.h>

int copy_from_user(void *, const void *, size_t);
int copy_to_user(void *, const void *, size_t);
bool user_ptr_ok(u64 p, u64 n);
long sys_getdent(int fd, void *ubuf, size_t len);

static long sys_brk(u64 newbrk)
{
    if (!newbrk)
        return (long)current->brk_cur;
    if ((u64)newbrk < (u64)current->brk_base ||
        newbrk > USER_STACK_TOP - (1 << 20))
        return -12;
    u64 old = ALIGN_UP((u64)current->brk_cur, PAGE_SIZE);
    u64 tgt = ALIGN_UP(newbrk, PAGE_SIZE);
    if (tgt > old) {
        for (u64 va = old; va < tgt; va += PAGE_SIZE) {
            u64 pa = pmm_alloc();
            if (!pa)
                return -12;
            memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
            vmm_map_user(current->pml4, va, pa, true, true);
        }
    } else {
        for (u64 va = tgt; va < old; va += PAGE_SIZE) {
            u64 pa = vmm_unmap_user(current->pml4, va);
            if (pa)
                pmm_free(pa);
        }
    }
    current->brk_cur = (void *)newbrk;
    return (long)current->brk_cur;
}

/* ---- kill(pid, sig) ---- */
static long sys_kill(int pid, int sig)
{
    if (sig < 0 || sig >= NR_SIGNALS)
        return -22;  /* EINVAL */

    if (pid > 0) {
        /* send to specific process */
        struct task *t = find_task(pid);
        if (!t)
            return -3;  /* ESRCH */
        send_signal(t, sig);
        return 0;
    } else if (pid == -1) {
        /* send to all processes (except idle) */
        int i = 0;
        for (struct task *t = task_iter(&i); t; t = task_iter(&i)) {
            if (t->pid > 0)
                send_signal(t, sig);
        }
        return 0;
    }
    return -22;
}

/* ---- sigaction(sig, act, oldact) ---- */
static long sys_sigaction(int sig, const struct lnxrm_sigaction *uact,
                          struct lnxrm_sigaction *uoldact)
{
    if (sig < 1 || sig >= NR_SIGNALS)
        return -22;

    /* save old handler */
    if (uoldact && user_ptr_ok((u64)uoldact, sizeof(*uoldact))) {
        struct lnxrm_sigaction old;
        old.sa_handler = current->sa[sig].sa_handler;
        old.sa_mask = current->sa[sig].sa_mask;
        old.sa_flags = current->sa[sig].sa_flags;
        copy_to_user(uoldact, &old, sizeof(old));
    }

    /* set new handler */
    if (uact && user_ptr_ok((u64)uact, sizeof(*uact))) {
        struct lnxrm_sigaction act;
        copy_from_user(&act, uact, sizeof(act));
        current->sa[sig].sa_handler = act.sa_handler;
        current->sa[sig].sa_mask = act.sa_mask;
        current->sa[sig].sa_flags = act.sa_flags;
    }

    return 0;
}

/* ---- sigprocmask(how, set, oldset) ---- */
static long sys_sigprocmask(int how, const u64 *uset, u64 *uoldset)
{
    u64 old_mask = current->signal_mask;

    if (uoldset && user_ptr_ok((u64)uoldset, sizeof(u64)))
        copy_to_user(uoldset, &old_mask, sizeof(u64));

    if (uset && user_ptr_ok((u64)uset, sizeof(u64))) {
        u64 new_set;
        copy_from_user(&new_set, uset, sizeof(u64));

        switch (how) {
        case 0: /* SIG_BLOCK */
            current->signal_mask |= new_set;
            break;
        case 1: /* SIG_UNBLOCK */
            current->signal_mask &= ~new_set;
            break;
        case 2: /* SIG_SETMASK */
            current->signal_mask = new_set;
            break;
        default:
            return -22;
        }
        /* cannot block SIGKILL or SIGSTOP */
        current->signal_mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    }

    return 0;
}

/* ---- getcpu() ---- */
static long sys_getcpu(void)
{
    return this_cpu_data()->id;
}

void syscall_entry(struct intr_frame *f)
{
    if (current)
        current->tf = f;
    u64 nr = f->rax;
    long ret = -38;
    u64 a3 = f->r10;

    switch (nr) {
    case SYS_read:
        ret = sys_read((int)f->rdi, (void *)f->rsi, a3);
        break;
    case SYS_write:
        ret = sys_write((int)f->rdi, (const void *)f->rsi, a3);
        break;
    case SYS_open: {
        char path[128];
        if (user_ptr_ok(f->rdi, 2)) {
            strncpy(path, (const char *)f->rdi, 127);
            path[127] = 0;
            ret = sys_open(path, (int)f->rsi);
        } else
            ret = -14;
        break;
    }
    case SYS_close:
        ret = sys_close((int)f->rdi);
        break;
    case SYS_lseek:
        ret = sys_lseek((int)f->rdi, (long)f->rsi, (int)a3);
        break;
    case SYS_getdent:
        ret = sys_getdent((int)f->rdi, (void *)f->rsi, a3);
        break;
    case SYS_dup2:
        ret = sys_dup2((int)f->rdi, (int)f->rsi);
        break;
    case SYS_brk:
        ret = sys_brk(f->rdi);
        break;
    case SYS_fork:
        ret = sys_fork();
        break;
    case SYS_execve:
        ret = sys_execve((const char *)f->rdi, (char *const *)f->rsi,
                         (char *const *)a3);
        break;
    case SYS_exit:
        sys_exit((int)f->rdi);
        break;
    case SYS_getpid:
        ret = current->pid;
        break;
    case SYS_getppid:
        ret = current->parent ? current->parent->pid : 0;
        break;
    case SYS_wait4:
        ret = sys_waitpid((int)f->rdi, (int *)f->rsi, (int)a3);
        break;
    case SYS_nanosleep:
        ret = sys_nanosleep(f->rdi / 1000000 ? f->rdi / 1000000 : 1);
        break;
    case SYS_uname: {
        static struct lnxrm_utsname un = {
            "LNXRM", "lnxrm", "1.0.0", "lnxrm #1", "x86_64"
        };
        if (copy_to_user((void *)f->rdi, &un, sizeof(un)) < 0)
            ret = -14;
        else
            ret = 0;
        break;
    }
    case SYS_ps: {
        extern void ps_dump(void);
        ps_dump();
        ret = 0;
        break;
    }
    /* signal syscalls */
    case SYS_kill:
        ret = sys_kill((int)f->rdi, (int)f->rsi);
        break;
    case SYS_sigaction:
        ret = sys_sigaction((int)f->rdi, (const struct lnxrm_sigaction *)f->rsi,
                            (struct lnxrm_sigaction *)a3);
        break;
    case SYS_sigprocmask:
        ret = sys_sigprocmask((int)f->rdi, (const u64 *)f->rsi, (u64 *)a3);
        break;
    case SYS_sigreturn:
        ret = sys_sigreturn();
        break;
    /* SMP syscalls */
    case SYS_getcpu:
        ret = sys_getcpu();
        break;
    default:
        kprintf("[sys] unknown syscall %d from pid %u\n", nr, current->pid);
        ret = -38;
    }
    f->rax = (u64)(long)ret;
    f->rflags |= 0x200;
}
