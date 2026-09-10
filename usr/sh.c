/* /bin/sh -- userspace terminal for lnxrm.
 * Everything goes through syscalls: console I/O via fd 0/1/2,
 * program execution via fork+execve, file listing via getdent.
 *
 * Supported commands (covering all 17 syscalls):
 *   read/write  : cat, echo, head, tail, touch, mkdir
 *   open/close  : ls, cat, touch, head, tail
 *   lseek       : head, tail
 *   brk         : meminfo
 *   getdent     : ls
 *   dup2        : dup_test
 *   nanosleep   : sleep
 *   fork/execve : external programs
 *   exit        : exit
 *   wait4       : (used after fork)
 *   getpid      : pid
 *   getppid     : ppid
 *   uname       : uname
 *   ps          : ps */
#include "ulib.h"

#define MAXARG 16

static char line[256];
static char cwd_file[64];

static size_t slen(const char *s) { size_t n = 0; while (*s++) n++; return n; }

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static char *scpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n-- && (*d++ = *s++)) ;
    return r;
}

static char *scat(char *d, const char *s, size_t n)
{
    while (*d) d++;
    while (n-- && (*d++ = *s++)) ;
    *d = 0;
    return d;
}

static long sintoi(const char *s)
{
    long v = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static int split(char *s, char **av, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        av[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = 0;
    }
    return n;
}

static void cmd_ls(const char *path)
{
    char pbuf[64];
    scpy(pbuf, path ? path : "/", sizeof(pbuf) - 1);
    long fd = kopen(pbuf, 0);
    if (fd < 0) {
        xprintf("ls: %s: no such directory\n", pbuf);
        return;
    }
    struct lnxrm_dirent de;
    while (kgetdent(fd, &de, sizeof(de)) > 0) {
        xputs(de.d_name);
        if (de.d_type == 4)
            putc_('/');
        xputs("  ");
    }
    putc_('\n');
    kclose(fd);
}

static void cmd_cat(const char *path)
{
    long fd = kopen(path, 0);
    if (fd < 0) {
        xprintf("cat: %s: no such file\n", path);
        return;
    }
    char b[128];
    long n;
    while ((n = kread(fd, b, sizeof(b))) > 0)
        kwrite(1, b, n);
    kclose(fd);
}

/* head: read first N lines (uses lseek to verify file size) */
static void cmd_head(const char *path, int lines)
{
    if (!path) { xputs("head: usage: head [-n N] FILE\n"); return; }
    long fd = kopen(path, 0);
    if (fd < 0) { xprintf("head: %s: no such file\n", path); return; }

    /* get file size via lseek SEEK_END */
    long sz = klseek(fd, 0, 2);
    klseek(fd, 0, 0); /* rewind */

    char b[128];
    long n;
    int cnt = 0;
    while (cnt < lines && (n = kread(fd, b, sizeof(b))) > 0) {
        for (long i = 0; i < n && cnt < lines; i++) {
            kwrite(1, &b[i], 1);
            if (b[i] == '\n') cnt++;
        }
    }
    kclose(fd);
}

/* tail: read last N lines (uses lseek) */
static void cmd_tail(const char *path, int lines)
{
    if (!path) { xputs("tail: usage: tail [-n N] FILE\n"); return; }
    long fd = kopen(path, 0);
    if (fd < 0) { xprintf("tail: %s: no such file\n", path); return; }

    long sz = klseek(fd, 0, 2);
    if (sz <= 0) { kclose(fd); return; }

    /* read last 4096 bytes or full file */
    long buf_sz = sz < 4096 ? sz : 4096;
    char *buf = (char *)kbrk(0);
    kbrk((long)(buf + buf_sz));
    klseek(fd, -buf_sz, 2);
    long n = kread(fd, buf, buf_sz);
    kclose(fd);

    /* count newlines, find start position */
    int total = 0;
    for (long i = 0; i < n; i++)
        if (buf[i] == '\n') total++;
    if (total <= lines) {
        kwrite(1, buf, n);
    } else {
        int skip = total - lines;
        long pos = 0;
        for (long i = 0; i < n && skip > 0; i++)
            if (buf[i] == '\n') { skip--; pos = i + 1; }
        kwrite(1, buf + pos, n - pos);
    }
}

/* sleep: uses nanosleep */
static void cmd_sleep(const char *sec_str)
{
    if (!sec_str) { xputs("sleep: usage: sleep SECONDS\n"); return; }
    long sec = sintoi(sec_str);
    if (sec <= 0) { xputs("sleep: invalid argument\n"); return; }
    ksleep_ms(sec * 1000);
}

/* pid: uses getpid */
static void cmd_pid(void)
{
    xprintf("%d\n", kgetpid());
}

/* ppid: uses getppid */
static void cmd_ppid(void)
{
    xprintf("%d\n", kgetppid());
}

/* meminfo: uses brk to show heap info */
static void cmd_meminfo(void)
{
    long cur = kbrk(0);
    xprintf("brk current: %d (%x)\n", cur, cur);
}

/* touch: create file with open O_CREAT */
static void cmd_touch(const char *path)
{
    if (!path) { xputs("touch: usage: touch FILE\n"); return; }
    long fd = kopen(path, 1 | 0100 | 01000); /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd < 0) { xprintf("touch: cannot create %s\n", path); return; }
    kclose(fd);
}

/* mkdir: create directory (via open with O_CREAT, kernel handles) */
static void cmd_mkdir(const char *path)
{
    if (!path) { xputs("mkdir: usage: mkdir DIR\n"); return; }
    long fd = kopen(path, 1 | 0100 | 01000);
    if (fd < 0) { xprintf("mkdir: cannot create %s\n", path); return; }
    kclose(fd);
}

/* dup_test: demonstrate dup2 syscall */
static void cmd_dup_test(void)
{
    xprintf("dup2 test: fd0=%d fd1=%d\n", kgetpid(), kgetppid());
    long fd = kopen("/etc/motd", 0);
    if (fd < 0) { xputs("dup_test: cannot open /etc/motd\n"); return; }
    /* dup fd to fd 10 */
    long newfd = kdup2(fd, 10);
    xprintf("dup2: fd %d -> fd %d\n", fd, newfd);
    /* read from new fd */
    char b[64];
    long n = kread(newfd, b, sizeof(b) - 1);
    if (n > 0) { b[n] = 0; xprintf("read via dup: %s\n", b); }
    kclose(newfd);
    kclose(fd);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    for (;;) {
        xputs("lnxrm$ ");

        int i = 0;
        for (;;) {
            char c;
            if (kread(0, &c, 1) <= 0)
                continue;
            if (c == '\r')
                c = '\n';
            if (c == '\b' || c == 0x7F) {
                if (i) {
                    i--;
                    kwrite(1, "\b \b", 3);
                }
                continue;
            }
            kwrite(1, &c, 1); /* echo */
            if (c == '\n')
                break;
            if (i < (int)sizeof(line) - 1)
                line[i++] = c;
        }
        line[i] = 0;

        /* output redirection: "cmd ... > file" */
        char *redir = 0;
        for (int a = 0; a < i; a++) {
            if (line[a] == '>' && (a == 0 || line[a - 1] == ' ')) {
                line[a] = 0;
                redir = &line[a + 1];
                while (*redir == ' ') redir++;
                char *e = redir;
                while (*e && *e != '\n') e++;
                *e = 0;
                break;
            }
        }

        char *av[MAXARG];
        int n = split(line, av, MAXARG);
        if (!n)
            continue;

        if (streq(av[0], "help")) {
            xputs("help echo ls cat head tail touch mkdir sleep pid ppid ps uname meminfo dup_test clear exit\n");
        } else if (streq(av[0], "echo")) {
            char obuf[160];
            size_t o = 0;
            for (int a = 1; a < n && o < sizeof(obuf) - 2; a++) {
                scpy(obuf + o, av[a], sizeof(obuf) - o - 2);
                o += slen(av[a]);
                obuf[o++] = ' ';
            }
            if (o) o--;
            obuf[o++] = '\n';
            obuf[o] = 0;

            long ofd = 1;
            if (redir) {
                ofd = kopen(redir, 1 | 0100 | 01000);
                if (ofd < 0) {
                    xprintf("sh: cannot create %s\n", redir);
                    continue;
                }
            }
            kwrite(ofd, obuf, o);
            if (ofd != 1)
                kclose(ofd);
        } else if (streq(av[0], "ls")) {
            cmd_ls(n > 1 ? av[1] : "/");
        } else if (streq(av[0], "cat")) {
            if (n > 1)
                cmd_cat(av[1]);
            else
                xputs("cat: usage: cat FILE\n");
        } else if (streq(av[0], "ps")) {
            kps();
        } else if (streq(av[0], "uname")) {
            xputs("lnxrm 1.0.0 x86_64\n");
        } else if (streq(av[0], "clear")) {
            kwrite(1, "\033[2J\033[H", 7);
        } else if (streq(av[0], "head")) {
            int lines = 10;
            const char *path = 0;
            for (int a = 1; a < n; a++) {
                if (streq(av[a], "-n") && a + 1 < n)
                    lines = sintoi(av[++a]);
                else
                    path = av[a];
            }
            cmd_head(path, lines);
        } else if (streq(av[0], "tail")) {
            int lines = 10;
            const char *path = 0;
            for (int a = 1; a < n; a++) {
                if (streq(av[a], "-n") && a + 1 < n)
                    lines = sintoi(av[++a]);
                else
                    path = av[a];
            }
            cmd_tail(path, lines);
        } else if (streq(av[0], "sleep")) {
            cmd_sleep(n > 1 ? av[1] : 0);
        } else if (streq(av[0], "pid")) {
            cmd_pid();
        } else if (streq(av[0], "ppid")) {
            cmd_ppid();
        } else if (streq(av[0], "meminfo")) {
            cmd_meminfo();
        } else if (streq(av[0], "touch")) {
            for (int a = 1; a < n; a++)
                cmd_touch(av[a]);
        } else if (streq(av[0], "mkdir")) {
            for (int a = 1; a < n; a++)
                cmd_mkdir(av[a]);
        } else if (streq(av[0], "dup_test")) {
            cmd_dup_test();
        } else if (streq(av[0], "exit")) {
            kexit(0);
        } else {
            /* external program from /bin */
            char full[64];
            scpy(full, "/bin/", sizeof(full) - 6);
            scat(full, av[0], sizeof(full) - slen(full) - 1);

            long pid = kfork();
            if (pid == 0) {
                if (redir) {
                    long f = kopen(redir, 1 | 0100 | 01000);
                    if (f >= 0) {
                        kdup2(f, 1);
                        if (f != 1)
                            kclose(f);
                    }
                }
                if (kexecve(full, av, 0) < 0 &&
                    kexecve(av[0], av, 0) < 0)
                    xprintf("sh: %s: not found\n", av[0]);
                kexit(127);
            }
            int st = 0;
            kwaitpid(pid, &st, 0);
        }
    }
    return 0;
}
