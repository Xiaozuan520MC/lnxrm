/* Round-robin preemptive scheduler with per-CPU run queues. */
#include <sched.h>
#include <percpu.h>
#include <console.h>
#include <mm.h>
#include <cpu.h>
#include <spinlock.h>

static struct task task_table[NR_TASKS];

/* In SMP mode, `current` is a function reading from per-CPU data. */
#ifndef CONFIG_SMP
struct task *current;
#else
struct task *get_current(void) {
    return this_cpu_data()->_current;
}
#endif

static void switch_to(struct task *next);

/* Per-CPU pick_next: find runnable task from this CPU's queue. */
static struct task *pick_next(void)
{
    struct cpu_info *cpu = this_cpu_data();
    struct task *head = cpu->runq_head.rq_next;
    if (head == &cpu->runq_head)
        return NULL;
    return head;
}

/* Add task to current CPU's run queue. */
void runqueue_add(struct task *t)
{
    struct cpu_info *cpu = this_cpu_data();
    spin_lock(&cpu->task_lock);
    t->rq_next = cpu->runq_head.rq_next;
    cpu->runq_head.rq_next = t;
    spin_unlock(&cpu->task_lock);
}

/* Remove task from its run queue. */
void runqueue_remove(struct task *t)
{
    struct cpu_info *cpu = this_cpu_data();
    spin_lock(&cpu->task_lock);
    struct task *p = &cpu->runq_head;
    while (p->rq_next && p->rq_next != &cpu->runq_head && p->rq_next != t)
        p = p->rq_next;
    if (p->rq_next == t)
        p->rq_next = t->rq_next;
    t->rq_next = NULL;
    spin_unlock(&cpu->task_lock);
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

void sched_init(void)
{
    memset(task_table, 0, sizeof(task_table));

    /* init BSP per-cpu data */
    struct cpu_info *bsp = this_cpu_data();
    bsp->id = 0;
    bsp->bsp = true;
    bsp->started = true;
    bsp->need_resched = false;

    /* set up idle task (pid 0) in task_table */
    struct task *idle = &task_table[0];
    idle->pid = 0;
    strcpy(idle->name, "idle");
    idle->state = T_RUNNING;
    idle->cpu_id = 0;

    /* BSP current = idle */
    bsp->_current = idle;

    /* set GS base for BSP */
    cpu_set_gs_base((u64)bsp);
}

void sched_maybe_preempt(struct intr_frame *f)
{
    /* Per-CPU need_resched: no cross-CPU race. */
    if (this_cpu_data()->need_resched && current &&
        current->state == T_RUNNING && (f->cs & 3) == 3) {
        this_cpu_data()->need_resched = false;
        schedule();
    }
}

void sched_tick(void)
{
    if (!current || current->pid == 0)
        return;

    if (current->quantum > 0) {
        current->quantum--;
        if (current->quantum == 0 && current->state == T_RUNNING)
            this_cpu_data()->need_resched = true;
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
    schedule();
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

    if (!next || next == current) {
        if (current->state == T_RUNNING && current->pid != 0)
            current->quantum = 6;
        else if (current->state == T_SLEEPING)
            current->state = T_RUNNABLE;
        if (cli_state)
            __asm__ volatile("sti");
        return;
    }

    if (current->state == T_RUNNING && current->pid != 0) {
        current->state = T_RUNNABLE;
        runqueue_add(current);
    }
    runqueue_remove(next);
    switch_to(next);

    if (cli_state)
        __asm__ volatile("sti");
}

static void switch_to(struct task *next)
{
    struct task *prev = current;
    next->state = T_RUNNING;
    next->quantum = 6;
    next->cpu_id = this_cpu_data()->id;
    if (prev)
        prev->cpu_id = -1;

    tss_set_rsp0((u64)next->kstack_top);
    if (prev != next && prev && prev->pml4 != next->pml4)
        vmm_switch_to(next->pml4);

    this_cpu_data()->_current = next;
    swtch((u64 *)&prev->ctx, &next->ctx.sp);
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
    t->cpu_id = -1;
}

struct task *find_task(u32 pid)
{
    for (int i = 0; i < NR_TASKS; i++)
        if (task_table[i].state != T_UNUSED && task_table[i].pid == pid)
            return &task_table[i];
    return NULL;
}

int task_count(void)
{
    int n = 0;
    for (int i = 1; i < NR_TASKS; i++)
        if (task_table[i].state != T_UNUSED)
            n++;
    return n;
}

void task_set_cpu(struct task *t, int cpu)
{
    if (t)
        t->cpu_id = cpu;
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
    case T_STOPPED:  return "STOPPED";
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
