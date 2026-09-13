/* /bin/kill -- send signal to a process. */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        xputs("kill: missing pid operand\n");
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
    long r = syscall1(SYS_kill, pid);
    if (r < 0) {
        xputs("kill: failed\n");
        return 1;
    }
    return 0;
}
