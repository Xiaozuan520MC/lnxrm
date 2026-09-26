#pragma once
#include <stdint.h>
#include <stddef.h>

#include <abi/lnxrm_abi.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

static inline long syscall1(long n, long a)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
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

static inline long kread(int fd, void *buf, size_t n)
{ return sys_call3(SYS_read, fd, (long)buf, n); }
static inline long kwrite(int fd, const void *buf, size_t n)
{ return sys_call3(SYS_write, fd, (long)buf, n); }
static inline long kopen(const char *p, int fl)
{ return sys_call3(SYS_open, (long)p, fl, 0); }
static inline long kclose(int fd)
{ return syscall1(SYS_close, fd); }
static inline long klseek(int fd, long off, int w)
{ return sys_call3(SYS_lseek, fd, off, w); }
static inline long kfork(void)
{ return syscall1(SYS_fork, 0); }
static inline long kexecve(const char *p, char **argv, char **envp)
{ return sys_call3(SYS_execve, (long)p, (long)argv, (long)envp); }
static inline void kexit(int c)
{
    syscall1(SYS_exit, c);
    for (;;);
}
static inline long kwaitpid(int pid, int *st, int fl)
{ return sys_call3(SYS_wait4, pid, (long)st, fl); }
static inline long kgetpid(void)
{ return syscall1(SYS_getpid, 0); }
static inline long kgetppid(void)
{ return syscall1(SYS_getppid, 0); }
static inline long ksleep_ms(long ms)
{ return sys_call3(SYS_nanosleep, ms * 1000000L, 0, 0); }
static inline long kps(void)
{ return syscall1(SYS_ps, 0); }
static inline long kbrk(long addr)
{ return sys_call3(SYS_brk, addr, 0, 0); }
static inline long kdup2(int a, int b)
{ return sys_call3(SYS_dup2, a, b, 0); }
static inline long kgetdent(int fd, struct lnxrm_dirent *e, size_t len)
{ return sys_call3(SYS_getdent, fd, (long)e, len); }
static inline long ksigaction(int sig, const struct lnxrm_sigaction *act,
                              struct lnxrm_sigaction *oldact)
{ return sys_call3(SYS_sigaction, sig, (long)act, (long)oldact); }
static inline long ksigprocmask(int how, const u64 *set, u64 *oldset)
{ return sys_call3(SYS_sigprocmask, how, (long)set, (long)oldset); }
static inline long kgetcpu(void)
{ return syscall1(SYS_getcpu, 0); }
static inline long kdiskinfo(void *buf, int max)
{ return sys_call3(SYS_diskinfo, (long)buf, max, 0); }
static inline long kmkdir(const char *path)
{ return syscall1(SYS_mkdir, (long)path); }
static inline long kunlink(const char *path)
{ return syscall1(SYS_unlink, (long)path); }
static inline long krmdir(const char *path)
{ return syscall1(SYS_rmdir, (long)path); }
static inline long krename(const char *oldpath, const char *newpath)
{ return sys_call3(SYS_rename, (long)oldpath, (long)newpath, 0); }
static inline long kmove(const char *src, const char *destdir)
{ return sys_call3(SYS_move, (long)src, (long)destdir, 0); }
/* one raw key event: ascii | scancode<<8 | extended<<16 */
static inline long kgetkey(int flags)
{ return syscall1(SYS_getkey, flags); }

static inline long kfb_info(struct lnxrm_fb_info *info)
{ return syscall1(SYS_fb_info, (long)info); }
static inline long kfb_clear(u32 color)
{ return syscall1(SYS_fb_clear, (long)color); }
static inline long kfb_fill(struct lnxrm_fb_rect *r)
{ return syscall1(SYS_fb_fill, (long)r); }
static inline long kfb_char(struct lnxrm_fb_char *c)
{ return syscall1(SYS_fb_char, (long)c); }
static inline long kfb_puts(struct lnxrm_fb_str *s)
{ return syscall1(SYS_fb_puts, (long)s); }

/* ---- mini libc ---- */
size_t xstrlen(const char *);
char *xstrcpy(char *, const char *);
void *xmemcpy(void *, const void *, size_t);
void xputs(const char *);
void xprinti(long v);
void xphex(u64 v);

#define printf(...)                                                                                \
    do {                                                                                           \
    } while (0)

/* formatted-ish print: supports %s %d %u %x %% via varargs */
void xprintf(const char *fmt, ...);

static inline void putc_(char c)
{ kwrite(1, &c, 1); }
