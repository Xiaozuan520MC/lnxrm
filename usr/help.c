/* /bin/help -- list available commands. */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    xputs("cat echo fdisk kill ls ps touch uname\n");
    return 0;
}
