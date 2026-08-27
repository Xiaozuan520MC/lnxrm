/* Process management: fork, execve, exit, waitpid + the syscall layer. */
#include <sched.h>
#include <io.h>
#include <syscall.h>
#include <console.h>
#include <mm.h>
#include <vfs.h>
#include <elf.h>
#include <cpu.h>


extern void glue_first(void);
struct task *task_alloc_slot(void);
void task_free_slot(struct task *t);
void runqueue_add(struct task *);
void runqueue_remove(struct task *);
int  task_count(void);

static u32 pid_counter;

static u32 pid_shadow = 1;   /* synced after boot spawns via pid_canary_sync */

void pid_canary_sync(void)
{
    extern u32 next_pid(void);
    /* called once after boot-time spawns */
    pid_shadow = pid_counter;
}

void pid_canary_check(void)
{
    if (pid_counter != pid_shadow) {
        u64 *base = (u64 *)((uptr)&pid_counter & ~7UL);
        kprintf("[!!] pid_counter %u -> %u j=%lu addr=%p\n", pid_shadow,
                pid_counter, jiffies, (void *)&pid_counter);
        for (int q = -4; q <= 4; q++)
            kprintf("  [%+d] %p = %#lx\n", q, (void *)&base[q], base[q]);
        pid_shadow = pid_counter;
    }
}

u32 next_pid(void)
{
    pid_shadow = ++pid_counter;   /* legitimate bump keeps the shadow in sync */
    return pid_counter;
}

/* ---- fd table helpers ---- */
static struct file *fd_get(int fd)
{
    if (fd < 0 || fd >= NR_FDS)
        return NULL;
    return current->fds[fd];
}

int fd_install(struct file *f)
{
    for (int i = 0; i < NR_FDS; i++) {
        if (!current->fds[i]) {
            current->fds[i] = f;
            f->refcnt++;
            return i;
        }
    }
    return -1;
}

bool user_ptr_ok(u64 p, u64 n)
{
    if (!n || !vmm_is_user_range(p, p + n))
        return false;
    u64 lo = p & ~4095ULL;
    u64 hi = (p + n - 1) & ~4095ULL;
    for (u64 pg = lo; pg <= hi; pg += 4096)
        if (!vmm_translate_in(current->pml4, pg))
            return false;
    return true;
}

#define UCP(p) ((p) ? user_ptr_ok((u64)(p), 8) : true)

/* ================= fork ================= */
int sys_fork(void)
{
    struct task *ch = task_alloc_slot();
    if (!ch)
        return -1;

    ch->kstack = kmalloc(KSTACK_SIZE);
    if (!ch->kstack) {
        task_free_slot(ch);
        return -1;
    }
    ch->kstack_top = ch->kstack + KSTACK_SIZE;
    ch->pid = next_pid();
    strcpy(ch->name, current->name);
    ch->parent = current;
    ch->state = T_EMBRYO;
    ch->pml4 = vmm_new_user_aspace();

    /* duplicate user pages: walk parent's slot-255 subtree */
    extern int dup_user_aspace(u64 src, u64 dst);
    if (dup_user_aspace(current->pml4, ch->pml4) < 0) {
        vmm_destroy_user_aspace(ch->pml4);
        kfree(ch->kstack);
        task_free_slot(ch);
        return -1;
    }

    /* clone fd table */
    memcpy(ch->fds, current->fds, sizeof(ch->fds));
    for (int i = 0; i < NR_FDS; i++)
        if (ch->fds[i])
            ch->fds[i]->refcnt++;
    ch->brk_base = current->brk_base;
    ch->brk_cur = current->brk_cur;

    /* fabricate the child's interrupt frame at the top of its kstack */
    struct intr_frame *tf =
        (struct intr_frame *)(ch->kstack_top - sizeof(struct intr_frame));
    memcpy(tf, current->tf, sizeof(*tf));
    tf->rax = 0;                    /* child sees fork() == 0 */
    tf->rflags |= 0x200;            /* interrupts ON in ring 3 (the copied
                                       kernel frame had IF cleared by the
                                       interrupt gate) */
    ch->tf = tf;

    /* register save area on the child kstack, right below its frame */
    u64 *area = (u64 *)((char *)tf - 7 * 8);
    memset(area, 0, 6 * 8);
    area[6] = (u64)glue_first;
    ch->ctx.sp = (u64)area;

    ch->state = T_RUNNABLE;
    runqueue_add(ch);
    return ch->pid;
}

/* ================= execve ================= */
long sys_execve(const char *upath, char *const uargv[], char *const uenvp[])
{
    char path[128];
    char argv[16][64];
    char envbuf[256];
    int argc = 0;

    if (!UCP(upath))
        return -14;
    strncpy(path, upath, sizeof(path) - 1);

    if (uargv && UCP((u64)uargv)) {
        for (int i = 0; i < 16; i++) {
            const char *a;
            if (copy_from_user(&a, &uargv[i], sizeof(a)) < 0 || !a)
                break;
            if (!user_ptr_ok((u64)a, 1))
                break;
            strncpy(argv[i], a, 63);
            argv[i][63] = 0;
            argc++;
        }
    }

    struct file *f = NULL;
    long err = vfs_open_file(path, O_RDONLY, &f);
    if (err < 0)
        return err;

    size_t sz = vfs_file_size(f);
    void *img = kmalloc(ALIGN_UP(sz + 1, PAGE_SIZE));
    if (!img) {
        vfs_close_file(f);
        return -12;
    }
    if (vfs_read_file(f, img, sz) != (long)sz) {
        kfree(img);
        vfs_close_file(f);
        return -5;
    }
    vfs_close_file(f);

    u64 old_pml4 = current->pml4;

    /* build the new address space and switch to it before loading:
     * elf_load writes through user VAs, so the new AS must be active */
    u64 new_pml4 = vmm_new_user_aspace();

    /* map a fresh user stack (16 KiB) below USER_STACK_TOP */
    for (u64 va = USER_STACK_TOP - 0x4000; va < USER_STACK_TOP;
         va += PAGE_SIZE) {
        u64 pa = pmm_alloc();
        if (!pa) {
            vmm_destroy_user_aspace(new_pml4);
            return -12;
        }
        memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
        vmm_map_user(new_pml4, va, pa, true, true);
    }

    u64 entry = 0, brk_end = 0;
    vmm_switch_to(new_pml4);
    entry = elf_load(new_pml4, img, sz, &brk_end);
    (void)0;
    if (!entry) {
        kfree(img);
        sys_exit(-8);               /* AS already switched; bail hard */
    }
    kfree(img);

    /* tear down the old address space (we no longer run in it) */
    vmm_destroy_user_aspace(old_pml4);
    current->pml4 = new_pml4;
    current->brk_base = (void *)ALIGN_UP(brk_end, PAGE_SIZE);
    current->brk_cur = current->brk_base;
    for (int i = 0; i < NR_FDS; i++)
        if (current->fds[i] && !(current->fds[i]->flags & 010000))
            ;                       /* keep fds simple: inherit all */
    strncpy(current->name, path, TASK_NAME_LEN - 1);

    /* user stack with argv/envp per SysV ABI */
    u64 sp = USER_STACK_TOP;
    u64 argv_ptrs[17];
    u64 envp_ptr = 0;
    sp -= 64;                       /* env placeholder */
    *(u64 *)(sp) = 0;
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        memcpy((void *)sp, argv[i], len);
        argv_ptrs[i] = sp;
    }
    sp &= ~15UL;
    sp -= 24;
    u64 ap = sp;
    *(u64 *)ap = 0;                 /* envp[0] = NULL */
    ap += 8;
    *(u64 *)ap = 0;                 /* auxv terminator */
    ap += 8;
    sp -= (argc + 2) * 8;
    u64 argvp = sp;
    *(u64 *)sp = (u64)argc;
    for (int i = 0; i < argc; i++)
        ((u64 *)sp)[1 + i] = argv_ptrs[i];
    ((u64 *)sp)[1 + argc] = 0;

    struct intr_frame *f2 = current->tf;
    f2->rip = entry;
    f2->ussp = sp;
    f2->cs = 0x18 | 3;
    f2->usss = 0x20 | 3;
    f2->rflags = 0x202;             /* IF | reserved bit 1 */
    (void)envp_ptr;
    (void)argvp;
    (void)envbuf;
    return 0;
}

/* ================= exit / wait ================= */
void sys_exit(int code)
{
    for (int i = 0; i < NR_FDS; i++)
        if (current->fds[i])
            sys_close(i);

    /* orphan children -> reparent to init (lowest live non-idle task) */
    struct task *init_t = NULL;
    int i = 0;
    for (struct task *t = task_iter(&i); t; t = task_iter(&i))
        if (t != current && t->parent == current)
            t->parent = init_t ? init_t : find_task(1);

    current->exit_code = code;
    current->state = T_ZOMBIE;
    runqueue_remove(current);
    if (current->parent && current->parent->state == T_SLEEPING &&
        current->parent->pid != 0) {
        current->parent->state = T_RUNNABLE;
        runqueue_add(current->parent);
    }
    schedule();
    /* Nothing else runnable: park as a zombie. The reaper does NOT free our
     * kernel stack, so it is safe to stay resident on it. */
    for (;;)
        __asm__ volatile("hlt");
}

int sys_waitpid(int wpid, int *ustatus, int opts)
{
    for (;;) {
        bool have_kids = false;
        int i = 0;
        for (struct task *t = task_iter(&i); t; t = task_iter(&i)) {
            if (t == current || t->parent != current)
                continue;
            have_kids = true;
            if (wpid > 0 && t->pid != wpid)
                continue;
            if (t->state == T_ZOMBIE) {
                int code = t->exit_code;
                if (ustatus && UCP(ustatus))
                    copy_to_user(ustatus, &code, sizeof(code));
                u32 pid = t->pid;
                vmm_destroy_user_aspace(t->pml4);
                /* Safe to free: the zombie is parked in its final hlt loop,
                 * will never be scheduled again, and this reaper code runs
                 * on the PARENT's stack -- the dying stack is dormant. */
                kfree(t->kstack);
                t->kstack = NULL;
                task_free_slot(t);
                return pid;
            }
        }
        if (!have_kids)
            return -10;             /* ECHILD-ish */
        if (opts & 1)               /* WNOHANG */
            return 0;
        current->state = T_SLEEPING;
        runqueue_remove(current);
        schedule();
    }
}

/* ================= user<->kernel copies ================= */
int copy_from_user(void *kdst, const void *usrc, size_t n)
{
    if (!user_ptr_ok((u64)usrc, n))
        return -1;
    memcpy(kdst, usrc, n);
    return 0;
}

int copy_to_user(void *udst, const void *ksrc, size_t n)
{
    if (!n || !vmm_is_user_range((u64)udst, (u64)udst + n))
        return -1;
    {
        u64 lo = (u64)udst & ~4095ULL;
        u64 hi = ((u64)udst + n - 1) & ~4095ULL;
        for (u64 pg = lo; pg <= hi; pg += 4096)
            if (!vmm_translate_in(current->pml4, pg))
                return -1;
    }
    memcpy(udst, ksrc, n);
    return 0;
}

/* ================= first user process ================= */
/* Build a runnable ring-3 task around `path` before any scheduler exists. */
int kernel_spawn(const char *path)
{
    struct task *t = task_alloc_slot();
    if (!t)
        return -1;

    t->kstack = kmalloc(KSTACK_SIZE);
    t->kstack_top = t->kstack + KSTACK_SIZE;
    t->pid = next_pid();
    strncpy(t->name, path, TASK_NAME_LEN - 1);
    t->state = T_EMBRYO;
    t->pml4 = vmm_new_user_aspace();

    struct file *f = NULL;
    if (vfs_open_file(path, O_RDONLY, &f) < 0) {
        kprintf("[init] cannot open %s\n", path);
        return -2;
    }
    size_t sz = vfs_file_size(f);
    void *img = kmalloc(ALIGN_UP(sz + 1, PAGE_SIZE));
    vfs_read_file(f, img, sz);
    vfs_close_file(f);

    for (u64 va = USER_STACK_TOP - 0x4000; va < USER_STACK_TOP;
         va += PAGE_SIZE) {
        u64 pa = pmm_alloc();
        memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
        vmm_map_user(t->pml4, va, pa, true, true);
    }

    vmm_switch_to(t->pml4);
    u64 entry = elf_load(t->pml4, img, sz, (u64 *)&t->brk_base);
    kfree(img);
    if (!entry)
        panic("cannot load %s", path);
    /* argv = {"path", NULL} on the fresh stack -- child AS still active! */
    u64 sp = USER_STACK_TOP - 256;
    strcpy((char *)sp, path);
    u64 argp[2] = { sp, 0 };
    u64 ap = sp - 32;
    ((u64 *)ap)[0] = 1;             /* argc */
    ((u64 *)ap)[1] = argp[0];       /* argv[0] */
    ((u64 *)ap)[2] = 0;             /* argv NULL */
    ((u64 *)ap)[3] = 0;             /* envp NULL */

    vmm_switch_to(master_pml4_phys());
    t->brk_base = (void *)ALIGN_UP((uptr)t->brk_base, PAGE_SIZE);
    t->brk_cur = t->brk_base;

    struct intr_frame *tf =
        (struct intr_frame *)(t->kstack_top - sizeof(struct intr_frame));
    memset(tf, 0, sizeof(*tf));
    tf->rip = entry;
    tf->ussp = ap;
    tf->cs = 0x18 | 3;
    tf->usss = 0x20 | 3;
    tf->rflags = 0x202;
    t->tf = tf;

    u64 *area = (u64 *)((char *)tf - 7 * 8);
    memset(area, 0, 6 * 8);
    area[6] = (u64)glue_first;
    t->ctx.sp = (u64)area;

    t->state = T_RUNNABLE;
    runqueue_add(t);
    kprintf("[task] spawned pid=%u (%s) entry=%#lx sp=%#lx\n", t->pid, path,
            entry, ap);
    return t->pid;
}

void ps_dump(void)
{
    int i = 0;
    kprintf("  PID STATE     NAME            PARENT\n");
    for (struct task *t = task_iter(&i); t; t = task_iter(&i)) {
        kprintf("%5u %-9s %-15s %u\n", t->pid, task_state_name(t->state),
                t->name, t->parent ? t->parent->pid : 0);
    }
}
