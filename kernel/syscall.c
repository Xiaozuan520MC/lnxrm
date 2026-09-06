/* System call dispatch (int 0x80, nr in rax, args rdi rsi rdx r10 r8). */
#include <syscall.h>
#include <sched.h>
#include <console.h>
#include <mm.h>
#include <vfs.h>
#include <cpu.h>


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

void syscall_entry(struct intr_frame *f)
{
    if (current)
        current->tf = f;        /* fork/exec need the live user frame */
    u64 nr = f->rax;
    long ret = -38;                 /* ENOSYS */
    u64 a3 = f->r10;                /* 3rd arg rides R10 (Linux-style) */

    switch (nr) {
    case SYS_read:
        ret = sys_read((int)f->rdi, (void *)f->rsi, a3);
        break;
    case SYS_write:
        ret = sys_write((int)f->rdi, (const void *)f->rsi, a3);
        break;
    case SYS_open: {
        /* path lives in user memory: validate then copy */
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
    case SYS_wait4 + 1000:          /* unreachable, silences enum warnings */
        sys_exit((int)f->rdi);
        break;
    case SYS_getpid:
        ret = current->pid;
        {
            static int jd;
            if (jd++ < 6)
                kprintf("<j%lu>", jiffies);
        }
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
    default:
        kprintf("[sys] unknown syscall %d from pid %u\n", nr, current->pid);
        ret = -38;
    }
    f->rax = (u64)(long)ret;
    f->rflags |= 0x200;             /* userland always runs with IF=1 */
}
