/* /bin/mv -- move a file or directory into another directory
 * (SYS_move), or to a new name when the destination is not a directory
 * (SYS_rename).  This is the usual mv behaviour. */
#include "ulib.h"

/* Does `path` exist and is it a directory?  sys_getdent() rejects
 * non-directories with -EBADF, so a successful call proves it is one. */
static int is_dir(const char *path)
{
    long fd = kopen(path, 0);
    if (fd < 0) return 0;
    struct lnxrm_dirent e;
    long r = kgetdent(fd, &e, sizeof(e));
    kclose(fd);
    return r >= 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        xputs("usage: mv <source> <dest-dir | new-name>\n");
        return 1;
    }
    const char *src = argv[1];
    const char *dst = argv[2];
    size_t n = xstrlen(dst);
    int into_dir = (n > 0 && dst[n - 1] == '/') || is_dir(dst);

    long r = into_dir ? kmove(src, dst) : krename(src, dst);
    if (r < 0) {
        xputs("mv: cannot move '");
        xputs(src);
        xputs("' to '");
        xputs(dst);
        xputs("' (errno ");
        xprinti(-r);
        xputs(")\n");
        return 1;
    }
    return 0;
}
