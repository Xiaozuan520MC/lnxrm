#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

static inline long syscall1(long n, long a)
{
    long r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "D"(a)
                     : "rcx", "r11", "memory");
    return r;
}

/* careful: int 0x80 clobbers rcx/r11 only; use SysV arg regs */
static inline long sys_call3(long n, long a, long b, long c)
{
    long r;
    register long r10 __asm__("r10");
    (void)r10;
    __asm__ volatile("mov %4, %%r10\n\tint $0x80"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "r"((long)c)
                     : "rcx", "r11", "r10", "memory");
    return r;
}

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_lseek 4
#define SYS_brk 5
#define SYS_getdent 6
#define SYS_dup2 7
#define SYS_nanosleep 8
#define SYS_getpid 9
#define SYS_fork 10
#define SYS_execve 11
#define SYS_exit 12
#define SYS_wait4 13
#define SYS_kill 14
#define SYS_uname 15
#define SYS_sigaction 16
#define SYS_sigprocmask 17
#define SYS_getppid 18
#define SYS_ps 19
#define SYS_sigreturn 20
#define SYS_getcpu 21
#define SYS_diskinfo 22

static inline long kread(int fd, void *buf, size_t n)
{
    return sys_call3(SYS_read, fd, (long)buf, n);
}
static inline long kwrite(int fd, const void *buf, size_t n)
{
    return sys_call3(SYS_write, fd, (long)buf, n);
}
static inline long kopen(const char *p, int fl)
{
    return sys_call3(SYS_open, (long)p, fl, 0);
}
static inline long kclose(int fd)
{
    return syscall1(SYS_close, fd);
}
static inline long klseek(int fd, long off, int w)
{
    return sys_call3(SYS_lseek, fd, off, w);
}
static inline long kfork(void)
{
    return syscall1(SYS_fork, 0);
}
static inline long kexecve(const char *p, char **argv, char **envp)
{
    return sys_call3(SYS_execve, (long)p, (long)argv, (long)envp);
}
static inline void kexit(int c)
{
    syscall1(SYS_exit, c);
    for (;;)
        ;
}
static inline long kwaitpid(int pid, int *st, int fl)
{
    return sys_call3(SYS_wait4, pid, (long)st, fl);
}
static inline long kgetpid(void)
{
    return syscall1(SYS_getpid, 0);
}
static inline long kgetppid(void)
{
    return syscall1(SYS_getppid, 0);
}
static inline long ksleep_ms(long ms)
{
    return sys_call3(SYS_nanosleep, ms * 1000000L, 0, 0);
}
static inline long kps(void)
{
    return syscall1(SYS_ps, 0);
}
static inline long kbrk(long addr)
{
    return sys_call3(SYS_brk, addr, 0, 0);
}
static inline long kdup2(int a, int b)
{
    return sys_call3(SYS_dup2, a, b, 0);
}
struct lnxrm_dirent {
    u64 d_ino;
    u8 d_type;
    char d_name[56];
};
struct lnxrm_sigaction {
    void (*sa_handler)(int);
    u64 sa_mask;
    int sa_flags;
};
static inline long kgetdent(int fd, struct lnxrm_dirent *e, size_t len)
{
    return sys_call3(SYS_getdent, fd, (long)e, len);
}
static inline long ksigaction(int sig, const struct lnxrm_sigaction *act,
                              struct lnxrm_sigaction *oldact)
{
    return sys_call3(SYS_sigaction, sig, (long)act, (long)oldact);
}
static inline long ksigprocmask(int how, const u64 *set, u64 *oldset)
{
    return sys_call3(SYS_sigprocmask, how, (long)set, (long)oldset);
}
static inline long kgetcpu(void)
{
    return syscall1(SYS_getcpu, 0);
}
static inline long kdiskinfo(void *buf, int max)
{
    return sys_call3(SYS_diskinfo, (long)buf, max, 0);
}

/* ---- mini libc ---- */
size_t xstrlen(const char *);
char *xstrcpy(char *, const char *);
void *xmemcpy(void *, const void *, size_t);
void xputs(const char *);
void xprinti(long v);
void xphex(u64 v);

#define printf(...) do { } while (0)

/* formatted-ish print: supports %s %d %u %x %% via varargs */
void xprintf(const char *fmt, ...);

static inline void putc_(char c)
{
    kwrite(1, &c, 1);
}
