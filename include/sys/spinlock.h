/* Spinlock: busy-wait mutual exclusion for SMP.
 * Caller must ensure interrupts are disabled when holding the lock,
 * or use spin_lock_irqsave / spin_unlock_irqrestore. */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile u32 locked; /* 0=free, 1=held */
    u32 pad;
} spinlock_t;

#define SPINLOCK_INIT {0, 0}

void spin_init(spinlock_t *l);
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);

/* Disable interrupts, acquire lock, save old IF into *flags. */
void spin_lock_irqsave(spinlock_t *l, u64 *flags);
/* Restore old IF and release lock. */
void spin_unlock_irqrestore(spinlock_t *l, u64 flags);

#ifdef __cplusplus
}
#endif
