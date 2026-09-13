/* /bin/uname -- print system information. */
#include "ulib.h"

struct lnxrm_utsname {
    char sysname[24];
    char nodename[24];
    char release[24];
    char version[32];
    char machine[16];
};

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct lnxrm_utsname un;
    long r = syscall1(SYS_uname, (long)&un);
    if (r == 0) {
        xputs(un.sysname);
        putc_(' ');
        xputs(un.nodename);
        putc_(' ');
        xputs(un.release);
        putc_(' ');
        xputs(un.version);
        putc_(' ');
        xputs(un.machine);
        putc_('\n');
    }
    return 0;
}
