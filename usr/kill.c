/* /bin/kill -- send signal to a process. */
#include "ulib.h"

#define SIGTERM 15

int main(int argc, char **argv)
{
    if (argc < 2) {
        xputs("usage: kill <pid>\n");
        return 1;
    }

    long pid = 0;
    const char *s = argv[1];
    while (*s >= '0' && *s <= '9')
        pid = pid * 10 + (*s++ - '0');
    if (pid <= 0) {
        xputs("kill: invalid pid\n");
        return 1;
    }

    /* 两个参数都传: (pid, sig), 默认 SIGTERM */
    long r = sys_call3(SYS_kill, pid, SIGTERM, 0);
    if (r < 0) {
        xputs("kill: failed\n");
        return 1;
    }
    return 0;
}
