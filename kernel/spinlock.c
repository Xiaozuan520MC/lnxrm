/* Spinlock: busy-wait mutual exclusion for SMP.
 * Reference: Linux kernel/locking/spinlock.c */
#include <sys/spinlock.h>
#include <sys/cpu.h>

void spin_init(spinlock_t *l)
{ l->locked = 0; }

void spin_lock(spinlock_t *l)
{
    while (__sync_lock_test_and_set(&l->locked, 1));
    __sync_synchronize();
}

void spin_unlock(spinlock_t *l)
{
    __sync_synchronize();
    __sync_lock_release(&l->locked);
}

void spin_lock_irqsave(spinlock_t *l, u64 *flags)
{
    u64 fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl));
    *flags = fl;
    while (__sync_lock_test_and_set(&l->locked, 1));
    __sync_synchronize();
}

void spin_unlock_irqrestore(spinlock_t *l, u64 flags)
{
    __sync_synchronize();
    __sync_lock_release(&l->locked);
    __asm__ volatile("push %0; popfq" ::"r"(flags) : "memory");
}
