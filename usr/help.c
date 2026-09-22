/* /bin/help -- list available commands. */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    xputs("cat mkdir clear cpu echo fdisk help kill ls ps sh touch rm\n");
    return 0;
}
