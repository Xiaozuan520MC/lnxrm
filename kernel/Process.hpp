#pragma once
#include <sched.h>

/* C++ wrapper around struct task with RAII-ish helpers. The kernel core
 * (task.c) stays plain C; this exposes an OO face for kernel components. */
class Process {
public:
    explicit Process(struct task *t) : t_(t) {}

    u32 pid() const { return t_->pid; }
    const char *name() const { return t_->name; }
    bool alive() const
    {
        return t_ && (t_->state == T_RUNNABLE || t_->state == T_RUNNING ||
                      t_->state == T_SLEEPING);
    }
    int exit_code() const { return t_->exit_code; }

private:
    struct task *t_;
};

/* spawn a user ELF as a new process (used by main.c for /init) */
int kernel_spawn(const char *path);
