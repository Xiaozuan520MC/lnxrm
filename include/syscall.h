#pragma once
#include <types.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linux-flavoured syscall numbers (int 0x80, args in rdi,rsi,rdx,r10,r8) */
#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_lseek       8
#define SYS_getdent     17
#define SYS_dup2        33
#define SYS_brk         12
#define SYS_fork        57
#define SYS_execve      59
#define SYS_exit        60
#define SYS_wait4       61
#define SYS_getpid      39
#define SYS_getppid     110
#define SYS_nanosleep   35
#define SYS_uname       63
#define SYS_ps          200
/* signal syscalls */
#define SYS_kill        62
#define SYS_sigaction   64
#define SYS_sigprocmask 65
#define SYS_sigreturn   201
/* SMP syscalls */
#define SYS_getcpu      202

struct lnxrm_utsname {
    char sysname[24];
    char nodename[24];
    char release[24];
    char version[32];
    char machine[16];
};

struct lnxrm_dirent {
    u64 d_ino;
    u8  d_type;
    char d_name[56];
};

/* sigaction for userspace (matches kernel struct sigaction) */
struct lnxrm_sigaction {
    void (*sa_handler)(int);
    u64 sa_mask;
    int sa_flags;
};

void syscall_entry(struct intr_frame *f);
long sys_open(const char *path, int flags);
long sys_read(int fd, void *buf, size_t n);
long sys_write(int fd, const void *buf, size_t n);

#ifdef __cplusplus
}
#endif
