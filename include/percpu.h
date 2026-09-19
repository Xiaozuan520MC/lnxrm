/* Per-CPU data: accessed via GS segment base. */
#pragma once
#include <types.h>
#include <sched.h>
#include <spinlock.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CPUS    8

struct cpu_info {
    u32 id;             /* logical CPU id */
    u32 apic_id;        /* APIC ID */
    bool started;       /* true once AP is online */
    bool bsp;           /* true for the bootstrap processor */

    struct task *_current;  /* current running task on this CPU */
    struct task runq_head;  /* per-CPU run queue sentinel */

    u64 kstack_top;     /* kernel stack top for this CPU */
    struct cpu_ctx idle_ctx; /* idle task context */
    u64 idle_stack[512];     /* 4 KiB idle stack */

    spinlock_t task_lock;   /* protects runq + task state */

    volatile bool need_resched; /* per-CPU reschedule flag */
};

/* GS-relative per-CPU struct. Size must be a power of 2 for easy addressing. */
#define CPU_DATA_SIZE   4096

/* Read/write the per-CPU pointer via GS. */
struct cpu_info *this_cpu_data(void);
void cpu_set_gs_base(u64 base);
void cpu_init_percpu(u32 cpu_id, u32 apic_id);

/* Convenience macros for per-CPU access. */
#define this_cpu     (this_cpu_data()->_current)
#define this_cpu_id  (this_cpu_data()->id)

/* All CPU info (indexed by logical id). */
extern struct cpu_info cpu_table[MAX_CPUS];
extern u32             cpu_count;  /* number of started CPUs */

#ifdef __cplusplus
}
#endif
