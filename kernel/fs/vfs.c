/* Virtual filesystem: path routing between the initramfs ramfs (root) and
 * an optional FAT32 mount at /mnt, plus the fd table & syscall surface. */
#include <vfs.h>
#include <console.h>
#include <mm.h>
#include <sched.h>
#include <io.h>
#include <syscall.h>
#include <cpu.h>

/* ---- ramfs backend (fs/ramfs.c) ---- */
extern struct fs_ops ramfs_ops;

/* ---- fat32 backend (fs/fat32.c) ---- */
extern struct fs_ops fat32_ops;
extern void *fat_mount(struct blkdev *b);
#define DISK_PREFIX "/mnt"
#define DISK_PREFIX_LEN 4

static bool disk_ready;
void *root_priv;                    /* ramfs private */

long console_read(struct file *f, void *buf, size_t n);
long console_write(struct file *f, const void *buf, size_t n);
int  fat_dir_iter(struct dir_iter *it, struct dirent_out *d);
int  ramfs_getdent_raw(struct dir_iter *it, struct dirent_out *d);

int copy_from_user(void *, const void *, size_t);
int copy_to_user(void *, const void *, size_t);

/* ---------------- routing ---------------- */
static struct fs_ops *route(const char **path)
{
    if (disk_ready && !strncmp(*path, DISK_PREFIX, DISK_PREFIX_LEN)) {
        const char *rest = *path + DISK_PREFIX_LEN;
        if (*rest == '/' || *rest == '\0') {
            while (*rest == '/')
                rest++;
            *path = rest;
            return &fat32_ops;
        }
    }
    return &ramfs_ops;
}

static void *route_mnt(void)
{
    extern void *fat_priv;
    return fat_priv;
}

long vfs_open_file(const char *path, int flags, struct file **out)
{
    const char *p = path;
    struct vnode vn;
    memset(&vn, 0, sizeof(vn));
    if (!strcmp(path, "/")) {
        /* resolve through ramfs so fs_data points at the root rnode */
        if (ramfs_ops.lookup(NULL, "/", &vn) < 0)
            return -2;
        vn.mount = NULL;
    } else {
        struct fs_ops *ops = route(&p);
        void *mnt = ops == &fat32_ops ? route_mnt() : NULL;
        /* defensive: a corrupted ops table must not kill the system */
        extern bool kern_text_ptr(u64 p);
        if (!kern_text_ptr((u64)ops->lookup))
            return -2;
        if (ops->lookup(mnt, p, &vn) < 0) {
            if (!(flags & O_CREAT))
                return -2;          /* ENOENT */
            if (ops->create(mnt, p) < 0)
                return -13;         /* EACCES-ish */
            if (ops->lookup(mnt, p, &vn) < 0)
                return -2;
            vn.size = 0;
        } else if ((flags & O_TRUNC) && vn.type == V_REG) {
            ops->write(mnt, vn.fs_data, 0, NULL, 0);   /* resize to 0 */
            vn.size = 0;
        }
    }

    struct file *f = kmalloc(sizeof(*f));
    if (!f)
        return -12;
    memset(f, 0, sizeof(*f));
    f->flags = flags;
    f->refcnt = 1;

    if (vn.type == V_CHR) {
        extern struct file_ops console_fops;
        f->ops = &console_fops;
        f->is_dir = false;
    } else if (vn.type == V_DIR) {
        extern struct file_ops dir_fops;
        f->ops = &dir_fops;
        f->is_dir = true;
        /* priv: {mount, node, cookie} packed */
        f->priv = kmalloc(4096);
        memset(f->priv, 0, 4096);
        memcpy(f->priv, &vn.fs_data, sizeof(void *));
        memcpy((char *)f->priv + 8, &vn.mount, sizeof(void *));
    } else {
        extern struct file_ops reg_fops;
        f->ops = &reg_fops;
        f->priv = kmalloc(sizeof(vn));
        memcpy(f->priv, &vn, sizeof(vn));
    }
    *out = f;
    return 0;
}

size_t vfs_file_size(struct file *f)
{
    struct vnode *vn = f->priv;
    return f->is_dir ? 0 : vn->size;
}

long vfs_read_file(struct file *f, void *buf, size_t n)
{
    return f->ops->read(f, buf, n);
}

void vfs_close_file(struct file *f)
{
    if (--f->refcnt > 0)
        return;
    if (f->ops && f->ops->close)
        f->ops->close(f);
    kfree(f);
}

/* ---------------- fd-level operations ---------------- */
static struct file *fd_get(int fd)
{
    if (fd < 0 || fd >= NR_FDS || !current->fds[fd])
        return NULL;
    return current->fds[fd];
}

long sys_open(const char *path, int flags)
{
    struct file *f = NULL;
    long err = vfs_open_file(path, flags, &f);
    if (err < 0)
        return err;
    for (int i = 0; i < NR_FDS; i++) {
        if (!current->fds[i]) {
            current->fds[i] = f;
            return i;
        }
    }
    vfs_close_file(f);
    return -24;
}

long sys_close(int fd)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -9;
    current->fds[fd] = NULL;
    vfs_close_file(f);
    return 0;
}

long sys_dup2(int oldfd, int newfd)
{
    struct file *f = fd_get(oldfd);
    if (!f || newfd < 0 || newfd >= NR_FDS)
        return -9;
    if (current->fds[newfd])
        sys_close(newfd);
    current->fds[newfd] = f;
    f->refcnt++;
    return newfd;
}

long sys_read(int fd, void *ubuf, size_t n)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -9;
    static char kbuf[4096];
    if (n > sizeof(kbuf))
        n = sizeof(kbuf);
    long r = f->ops->read(f, kbuf, n);
    if (r > 0 && copy_to_user(ubuf, kbuf, r) < 0)
        return -14;
    return r;
}

long sys_write(int fd, const void *ubuf, size_t n)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -9;
    static char kbuf[4096];
    if (n > sizeof(kbuf))
        n = sizeof(kbuf);
    if (copy_from_user(kbuf, ubuf, n) < 0)
        return -14;
    return f->ops->write(f, kbuf, n);
}

long sys_lseek(int fd, long off, int whence)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -9;
    return f->ops->lseek(f, off, whence);
}

long sys_getdent(int fd, void *ubuf, size_t len)
{
    struct file *f = fd_get(fd);
    if (!f || !f->is_dir)
        return -9;
    static char kbuf[1024];
    if (len > sizeof(kbuf))
        len = sizeof(kbuf);
    long r = f->ops->getdent(f, kbuf, len);
    if (r > 0 && copy_to_user(ubuf, kbuf, r) < 0)
        return -14;
    return r;
}

/* ---------------- generic file/dir fops bridging to fs_ops -------------- */

static long reg_read(struct file *f, void *buf, size_t n)
{
    struct vnode *vn = f->priv;
    long r = vn->ops->read(vn->mount, vn->fs_data, f->pos, buf, n);
    if (r > 0)
        f->pos += r;
    return r;
}

static long reg_write(struct file *f, const void *buf, size_t n)
{
    struct vnode *vn = f->priv;
    long r = vn->ops->write(vn->mount, vn->fs_data, f->pos, buf, n);
    if (r > 0)
        f->pos += r;
    return r;
}

static long reg_lseek(struct file *f, long off, int whence)
{
    struct vnode *vn = f->priv;
    u64 base = whence == SEEK_SET ? 0 :
               whence == SEEK_CUR ? f->pos : vn->size;
    f->pos = off < 0 ? 0 : base + off;
    return f->pos;
}

static long dir_getdent(struct file *f, void *buf, size_t n)
{
    struct dir_iter *it = f->priv;
    struct dirent_out d;
    long cnt = 0;
    char *p = buf;
    while ((size_t)(cnt + (long)sizeof(struct lnxrm_dirent)) <= n) {
        int (*fn)(struct dir_iter *, struct dirent_out *) =
            it->mount ? fat_dir_iter : ramfs_getdent_raw;
        if (fn(it, &d) < 0)
            break;
        struct lnxrm_dirent e;
        memset(&e, 0, sizeof(e));
        strncpy(e.d_name, d.name, 55);
        e.d_type = d.type;
        memcpy(p + cnt, &e, sizeof(e));
        cnt += sizeof(e);
    }
    return cnt;
}

int fat_dir_iter(struct dir_iter *it, struct dirent_out *d);

static int noop_close(struct file *f) { return 0; }

static int dir_close(struct file *f)
{
    kfree(f->priv);
    return 0;
}

struct file_ops console_fops = {
    .read = console_read,
    .write = console_write,
    .lseek = NULL,
    .close = noop_close,
};

struct file_ops reg_fops = {
    .read = reg_read,
    .write = reg_write,
    .lseek = reg_lseek,
    .close = noop_close,
};

struct file_ops dir_fops = {
    .getdent = dir_getdent,
    .close = dir_close,
};

/* console character device */
long console_read(struct file *f, void *buf, size_t n)
{
    char *p = buf;
    size_t got = 0;
    extern int lnxrm_uart_trygetc(void);
    while (got < n) {
        int c = input_pop();
        if (c < 0)
            c = lnxrm_uart_trygetc();   /* polled fallback */
        if (c >= 0) {
            p[got++] = (char)c;
            if (c == '\n')
                break;
        } else {
            __asm__ volatile("pause");  /* busy-poll: IF==0 inside syscalls */
        }
    }
    return got;
}

long console_write(struct file *f, const void *buf, size_t n)
{
    const char *p = buf;
    for (size_t i = 0; i < n; i++)
        console_putc(p[i]);
    return n;
}

/* ---------------- boot-time mounting ---------------- */
void vfs_init(void)
{
    extern void ramfs_init(void);
    ramfs_init();
}

void ramfs_add_from_cpio(const void *cpio, size_t len);

int vfs_mount_root(void)
{
    extern u8 __initramfs_start[], __initramfs_end[];
    ramfs_add_from_cpio(__initramfs_start, __initramfs_end - __initramfs_start);
    return 0;
}

struct blkdev *blk_first;

void blk_register(struct blkdev *b)
{
    if (!blk_first) {
        blk_first = b;
        kprintf("[blk] registered %s (%u sectors)\n", b->name,
                b->num_sectors);
    }
}

int vfs_try_mount_disk(void)
{
    extern struct blkdev *blk_first;
    if (!blk_first)
        return -1;
    void *m = fat_mount(blk_first);
    if (!m)
        return -1;
    disk_ready = true;
    return 0;
}
