/* Round-robin preemptive scheduler.
 * Tasks are switched via swtch(); user entry/exit goes through interrupt
 * frames on each task's kernel stack. */
#include <sched.h>
#include <console.h>
#include <mm.h>
#include <cpu.h>

static struct task task_table[NR_TASKS];
static struct task runq_head;       /* sentinel ring */
struct task *current;

static struct task *pick_next(void)
{
    if (runq_head.rq_next == &runq_head)
        return NULL;
    return runq_head.rq_next;
}

void runqueue_add(struct task *t)
{
    t->rq_next = runq_head.rq_next;
    runq_head.rq_next = t;
}

void runqueue_remove(struct task *t)
{
    struct task *p = &runq_head;
    /* stop at the sentinel so removing a not-queued task cannot spin */
    while (p->rq_next && p->rq_next != &runq_head && p->rq_next != t)
        p = p->rq_next;
    if (p->rq_next == t)
        p->rq_next = t->rq_next;
    t->rq_next = NULL;
}

extern void swtch(u64 *old_slot, u64 *new_slot);
extern void tf_exit(struct intr_frame *f);
void tss_set_rsp0(u64 rsp);

/* idle context: entered when the runqueue is empty */
static void idle_task_body(void)
{
    for (;;) {
        __asm__ volatile("sti; hlt");
        schedule();
    }
}
static u8 idle_stack[4096] __attribute__((aligned(16)));
static struct cpu_ctx idle_ctx;

void sched_init(void)
{
    runq_head.rq_next = &runq_head;
    memset(task_table, 0, sizeof(task_table));

    /* pid 0: idle task, never scheduled through the queue */
    struct task *idle = &task_table[0];
    idle->pid = 0;
    strcpy(idle->name, "idle");
    idle->state = T_RUNNING;
    idle->kstack = (char *)idle_stack + sizeof(idle_stack);
    idle->pml4 = master_pml4_phys();
    current = idle;
}

/* Called from IRQ0 exit path when the current quantum ran out and we
 * interrupted a user frame (kernel frames finish their work first). */
static volatile bool need_resched;

void sched_maybe_preempt(struct intr_frame *f)
{
    if (need_resched && current && current->state == T_RUNNING &&
        (f->cs & 3) == 3) {
        need_resched = false;
        schedule();
    }
}

void sched_tick(void)
{
    if (!current || current->pid == 0)
        return;
    /* cooperative for now: no forced preemption */
    {
        extern void pid_canary_check(void);
        pid_canary_check();
    }
    /* wake sleepers */
    for (int i = 1; i < NR_TASKS; i++) {
        struct task *t = &task_table[i];
        if (t->state == T_SLEEPING && jiffies >= t->sleep_until) {
            t->state = T_RUNNABLE;
            runqueue_add(t);
        }
    }
}

void yield(void)
{
    need_resched = false;
    schedule();
}

static void switch_to(struct task *next)
{
    struct task *prev = current;
    next->state = T_RUNNING;
    next->quantum = 6;              /* ~60 ms slices */
    tss_set_rsp0((u64)next->kstack_top);
    if (prev != next && prev && prev->pml4 != next->pml4)
        vmm_switch_to(next->pml4);
    current = next;
    swtch((u64 *)&prev->ctx, &next->ctx.sp);
}

void schedule(void)
{
    if (!current)
        panic("schedule without current");
    bool cli_state = false;
    u64 fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl));
    cli_state = !!(fl & 0x200);

    struct task *next = pick_next();

    /* NOTE: we may run inside an interrupt handler where IF==0 -- never
     * re-enable interrupts here. */
    if (!next || next == current) {
        /* sole runner: refill its quantum and keep going */
        if (current->state == T_RUNNING && current->pid != 0)
            current->quantum = 6;
        else if (current->state == T_SLEEPING)
            current->state = T_RUNNABLE;   /* spurious wake: retry the wait */
        if (cli_state)
            __asm__ volatile("cli");
        return;
    }

    if (current->state == T_RUNNING && current->pid != 0) {
        current->state = T_RUNNABLE;
        runqueue_add(current);
    }
    runqueue_remove(next);
    switch_to(next);

    if (cli_state)
        __asm__ volatile("cli");    /* resumed with IF as caller expects off */
}

int sys_nanosleep(u64 ms)
{
    u64 until = jiffies + ms * HZ / 1000 + 1;
    for (;;) {
        if ((i64)(jiffies - until) >= 0)
            break;
        current->sleep_until = until;
        current->state = T_SLEEPING;
        runqueue_remove(current);
        schedule();
    }
    return 0;
}

/* ---- task table helpers used by task.c ---- */
struct task *task_alloc_slot(void)
{
    for (int i = 1; i < NR_TASKS; i++)
        if (task_table[i].state == T_UNUSED)
            return &task_table[i];
    return NULL;
}

void task_free_slot(struct task *t)
{
    t->state = T_UNUSED;
}

struct task *find_task(u32 pid)
{
    for (int i = 0; i < NR_TASKS; i++)
        if (task_table[i].state != T_UNUSED && task_table[i].pid == pid)
            return &task_table[i];
    return NULL;
}

const char *task_state_name(enum task_state s)
{
    switch (s) {
    case T_UNUSED:   return "UNUSED";
    case T_EMBRYO:   return "EMBRYO";
    case T_RUNNABLE: return "RUNNABLE";
    case T_RUNNING:  return "RUNNING";
    case T_SLEEPING: return "SLEEPING";
    case T_ZOMBIE:   return "ZOMBIE";
    }
    return "?";
}

struct task *task_iter(int *i)
{
    while (*i < NR_TASKS) {
        struct task *t = &task_table[(*i)++];
        if (t->state != T_UNUSED)
            return t;
    }
    return NULL;
}
