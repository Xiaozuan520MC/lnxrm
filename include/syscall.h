#pragma once
#include <types.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lnxrm syscall numbers (int 0x80, args in rdi,rsi,rdx,r10,r8) */
#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_lseek       4
#define SYS_brk         5
#define SYS_getdent     6
#define SYS_dup2        7
#define SYS_nanosleep   8
#define SYS_getpid      9
#define SYS_fork        10
#define SYS_execve      11
#define SYS_exit        12
#define SYS_wait4       13
#define SYS_kill        14
#define SYS_uname       15
#define SYS_sigaction   16
#define SYS_sigprocmask 17
#define SYS_getppid     18
#define SYS_ps          19
#define SYS_sigreturn   20
#define SYS_getcpu      21
#define SYS_diskinfo    22

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
