#pragma once

#include <stddef.h>
#include <stdint.h>

enum lnxrm_errno {
    LNXRM_EFAIL = -1,
    LNXRM_ENOENT = -2,
    LNXRM_ESRCH = -3,
    LNXRM_EINTR = -4,
    LNXRM_EIO = -5,
    LNXRM_ENOEXEC = -8,
    LNXRM_EBADF = -9,
    LNXRM_ECHILD = -10,
    LNXRM_EAGAIN = -11,
    LNXRM_ENOMEM = -12,
    LNXRM_EACCES = -13,
    LNXRM_EFAULT = -14,
    LNXRM_EEXIST = -17,
    LNXRM_EXDEV = -18,
    LNXRM_ENOTDIR = -20,
    LNXRM_EINVAL = -22,
    LNXRM_EMFILE = -24,
    LNXRM_ENOSPC = -28,
    LNXRM_ENOSYS = -38,
    LNXRM_ENOTEMPTY = -39
};

#if defined(__cplusplus)
#define LNXRM_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define LNXRM_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

enum lnxrm_syscall_number {
#define LNXRM_SYS(name, nr) SYS_##name = nr,
#include "lnxrm_syscalls.def"
#undef LNXRM_SYS
};

#define LNXRM_SYSCALL_FIRST 0
#define LNXRM_SYSCALL_LAST  33

/* SYS_getkey(flags) */
#define LNXRM_KBD_BLOCK   0 /* default: sleep until a key is pressed */
#define LNXRM_KBD_NONBLOCK 1 /* return -EAGAIN instead of sleeping */

/* SYS_getkey return value: ascii | scancode<<8 | extended<<16 */
#define LNXRM_KEY_ASCII(ev)     ((unsigned)(ev) & 0xFFu)
#define LNXRM_KEY_SCANCODE(ev)  (((unsigned)(ev) >> 8) & 0xFFu)
#define LNXRM_KEY_EXTENDED(ev)  (((unsigned)(ev) >> 16) & 1u)

struct lnxrm_utsname {
    char sysname[24];
    char nodename[24];
    char release[24];
    char version[32];
    char machine[16];
};

struct lnxrm_dirent {
    uint64_t d_ino;
    uint8_t d_type;
    char d_name[56];
};

struct lnxrm_sigaction {
    void (*sa_handler)(int);
    uint64_t sa_mask;
    int sa_flags;
};

struct lnxrm_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
};

struct lnxrm_fb_rect {
    uint32_t x, y, w, h, color;
};

struct lnxrm_fb_char {
    uint32_t x, y, ch, fg, bg;
};

struct lnxrm_fb_str {
    uint32_t x, y, fg, bg;
    const char *str;
};

LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_utsname) == 120, "utsname ABI");
LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_dirent) == 72, "dirent ABI");
LNXRM_STATIC_ASSERT(offsetof(struct lnxrm_dirent, d_type) == 8, "dirent ABI");
LNXRM_STATIC_ASSERT(offsetof(struct lnxrm_dirent, d_name) == 9, "dirent ABI");
LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_sigaction) == 24, "sigaction ABI");
LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_fb_info) == 16, "fb_info ABI");
LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_fb_rect) == 20, "fb_rect ABI");
LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_fb_char) == 20, "fb_char ABI");
LNXRM_STATIC_ASSERT(sizeof(struct lnxrm_fb_str) == 24, "fb_str ABI");
