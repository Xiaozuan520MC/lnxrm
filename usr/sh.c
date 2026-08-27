/* /bin/sh -- userspace terminal for lnxrm.
 * Everything goes through syscalls: console I/O via fd 0/1/2,
 * program execution via fork+execve, file listing via getdent. */
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
            if (c == '\b') {
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
            xputs("builtins: help echo ls cat ps uname clear\n"
                  "programs: /bin/hello   files: > file redirects\n");
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
