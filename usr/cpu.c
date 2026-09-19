/* usr/cpu.c -- 显示当前进程运行在哪个 CPU 上 */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    long c = kgetcpu();
    xprintf("getcpu = %d\n", (int)c);

    /* 打印几次, 看是否被调度到不同 CPU */
    for (int i = 0; i < 8; i++) {
        xprintf("  sample %d: cpu %d\n", i, (int)kgetcpu());
    }

    return 0;
}
