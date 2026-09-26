/* Virtual filesystem: path routing to a FAT32 root mounted at /,
 * plus synthesised device nodes under /dev and the fd table & surface. */
#include <sys/vfs.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <io.h>
#include <sys/cpu.h>
#include <disk.h>

/* ---- fat32 backend (fs/fat32.c) ---- */
extern struct fs_ops fat32_ops;
extern void *fat_mount(struct blkdev *b);

#define DISK_PREFIX "/"

/* Mounted filesystem info */
struct mounted_fs {
    struct fs_ops *ops;
    void *priv;
    char prefix[32];
    int prefix_len;
    bool active;
};

#define MAX_MOUNTS 4
static struct mounted_fs mounts[MAX_MOUNTS];
static int mount_count;
static bool disk_ready;

long console_read(struct file *f, void *buf, size_t n);
long console_write(struct file *f, const void *buf, size_t n);
int fat_dir_iter(struct dir_iter *it, struct dirent_out *d);

int copy_from_user(void *, const void *, size_t);
int copy_to_user(void *, const void *, size_t);
bool user_ptr_ok(u64 p, u64 n);

/* ---------------- routing ---------------- */
static struct fs_ops *fs_for_path(const char **path)
{
    /* Check each mounted filesystem */
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active) continue;

        if (!strncmp(*path, mounts[i].prefix, mounts[i].prefix_len)) {
            const char *rest = *path + mounts[i].prefix_len;
            /* root mount "/" matches everything */
            if (mounts[i].prefix_len == 1 && mounts[i].prefix[0] == '/') {
                while (*rest == '/') rest++;
                *path = rest;
                return mounts[i].ops;
            }
            if (*rest == '/' || *rest == '\0') {
                while (*rest == '/') rest++;
                *path = rest;
                return mounts[i].ops;
            }
        }
        /* Also match without leading '/' (e.g. "mnt" matches "/mnt") */
        if (mounts[i].prefix[0] == '/' &&
            !strncmp(*path, mounts[i].prefix + 1, mounts[i].prefix_len - 1)) {
            const char *rest = *path + mounts[i].prefix_len - 1;
            if (*rest == '/' || *rest == '\0') {
                while (*rest == '/') rest++;
                *path = rest;
                return mounts[i].ops;
            }
        }
    }
    return NULL; /* no mount owns this path */
}

static void *mount_priv(void *ops)
{
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active) continue;
        if (mounts[i].ops == ops) return mounts[i].priv;
    }
    return NULL;
}

/* Register a filesystem mount */
static int register_mount(struct fs_ops *ops, void *priv, const char *prefix)
{
    if (mount_count >= MAX_MOUNTS) return LNXRM_EFAIL;

    mounts[mount_count].ops = ops;
    mounts[mount_count].priv = priv;
    strncpy(mounts[mount_count].prefix, prefix, sizeof(mounts[mount_count].prefix) - 1);
    mounts[mount_count].prefix[sizeof(mounts[mount_count].prefix) - 1] = 0;
    mounts[mount_count].prefix_len = strlen(mounts[mount_count].prefix);
    mounts[mount_count].active = true;
    mount_count++;

    return 0;
}

static const char *abs_path(const char *path, char *buf, size_t bufsz)
{
    if (path[0] == '/') return path;
    buf[0] = '/';
    size_t i;
    for (i = 1; i < bufsz - 2 && path[i - 1]; i++) buf[i] = path[i - 1];
    buf[i] = 0;
    return buf;
}

/* ---- synthesised device nodes ----
 * FAT32 has no device files, so the nodes under /dev are built here instead
 * of living in a whole in-memory filesystem.  Elsewhere returns ENOENT. */
static int dev_getdent(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d)
{
    (void)mnt;
    (void)dirnode;
    if (*cookie) return LNXRM_EFAIL; /* exhausted */
    *cookie = 1;
    strncpy(d->name, "console", sizeof(d->name) - 1);
    d->name[sizeof(d->name) - 1] = 0;
    d->type = 3; /* DT_CHR */
    return 0;
}

static struct fs_ops dev_ops = {
    .getdent = dev_getdent,
};

static int dev_lookup(const char *path, struct vnode *vn)
{
    char p[32];
    strncpy(p, path, sizeof(p) - 1);
    p[sizeof(p) - 1] = 0;
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;

    if (!strcmp(p, "/dev")) {
        vn->type = V_DIR;
        vn->fs_data = NULL;
        vn->ops = &dev_ops;
        return 0;
    }
    if (!strcmp(p, "/dev/console")) {
        vn->type = V_CHR;
        vn->fs_data = NULL;
        return 0;
    }
    return LNXRM_ENOENT;
}

/* Resolve a path to a vnode, honouring O_CREAT and O_TRUNC. */
static long vfs_lookup(const char *path, int flags, struct vnode *vn)
{
    const char *p = path;
    memset(vn, 0, sizeof(*vn));
    if (!strcmp(path, "/")) {
        /* disk root when mounted at "/", otherwise nothing to serve */
        if (disk_ready) {
            struct fs_ops *ops = fs_for_path(&p);
            void *mnt = mount_priv(ops);
            if (ops && ops->lookup(mnt, p, vn) == 0) return 0;
        }
        return LNXRM_ENOENT;
    }

    struct fs_ops *ops = fs_for_path(&p);
    if (!ops) return dev_lookup(path, vn); /* unmounted: virtual nodes only */
    void *mnt = mount_priv(ops);
    /* defensive: a corrupted ops table must not kill the system */
    extern bool kern_text_ptr(u64 p);
    if (!kern_text_ptr((u64)ops->lookup)) return LNXRM_ENOENT;
    if (ops->lookup(mnt, p, vn) < 0) {
        if (dev_lookup(path, vn) == 0) return 0; /* e.g. /dev/console */
        if (!(flags & O_CREAT)) return LNXRM_ENOENT;      /* ENOENT */
        if (ops->create(mnt, p) < 0) return LNXRM_EACCES; /* EACCES-ish */
        if (ops->lookup(mnt, p, vn) < 0) return LNXRM_ENOENT;
        vn->size = 0;
    } else if ((flags & O_TRUNC) && vn->type == V_REG) {
        ops->write(mnt, vn->fs_data, 0, NULL, 0); /* resize to 0 */
        vn->size = 0;
    }
    return 0;
}

/* Wrap a resolved vnode in a struct file of the right flavour. */
static struct file *file_alloc(const struct vnode *vn, int flags)
{
    struct file *f = kmalloc(sizeof(*f));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->flags = flags;
    f->refcnt = 1;

    if (vn->type == V_CHR) {
        extern struct file_ops console_fops;
        f->ops = &console_fops;
        f->is_dir = false;
    } else if (vn->type == V_DIR) {
        extern struct file_ops dir_fops;
        f->ops = &dir_fops;
        f->is_dir = true;
        struct dir_iter *it = kmalloc(sizeof(*it));
        memset(it, 0, sizeof(*it));
        it->node = vn->fs_data;
        it->ops = vn->ops;
        it->mnt_data = mount_priv(vn->ops);
        f->priv = it;
    } else {
        extern struct file_ops reg_fops;
        f->ops = &reg_fops;
        f->priv = kmalloc(sizeof(*vn));
        memcpy(f->priv, vn, sizeof(*vn));
    }
    return f;
}

long vfs_open_file(const char *path, int flags, struct file **out)
{
    char abuf[128];
    struct vnode vn;
    long err = vfs_lookup(abs_path(path, abuf, sizeof(abuf)), flags, &vn);
    if (err < 0) return err;

    struct file *f = file_alloc(&vn, flags);
    if (!f) return LNXRM_ENOMEM;
    *out = f;
    return 0;
}

size_t vfs_file_size(struct file *f)
{
    struct vnode *vn = f->priv;
    return f->is_dir ? 0 : vn->size;
}

long vfs_read_file(struct file *f, void *buf, size_t n)
{ return f->ops->read(f, buf, n); }

void vfs_close_file(struct file *f)
{
    if (__sync_fetch_and_sub(&f->refcnt, 1) > 1) return;
    if (f->ops && f->ops->close) f->ops->close(f);
    kfree(f);
}

/* ---------------- fd-level operations ---------------- */
static struct file *fd_get(int fd)
{
    if (fd < 0 || fd >= NR_FDS || !current->fds[fd]) return NULL;
    return current->fds[fd];
}

long sys_open(const char *path, int flags)
{
    struct file *f = NULL;
    long err = vfs_open_file(path, flags, &f);
    if (err < 0) return err;
    for (int i = 0; i < NR_FDS; i++) {
        if (!current->fds[i]) {
            current->fds[i] = f;
            return i;
        }
    }
    vfs_close_file(f);
    return LNXRM_EMFILE;
}

long sys_close(int fd)
{
    struct file *f = fd_get(fd);
    if (!f) return LNXRM_EBADF;
    current->fds[fd] = NULL;
    vfs_close_file(f);
    return 0;
}

long sys_dup2(int oldfd, int newfd)
{
    if (oldfd == newfd) return fd_get(oldfd) ? newfd : -9;
    struct file *f = fd_get(oldfd);
    if (!f || newfd < 0 || newfd >= NR_FDS) return LNXRM_EBADF;
    if (current->fds[newfd]) sys_close(newfd);
    current->fds[newfd] = f;
    __sync_fetch_and_add(&f->refcnt, 1);
    return newfd;
}

long sys_mkdir(const char *path)
{
    char abuf[128];
    path = abs_path(path, abuf, sizeof(abuf));
    const char *p = path;
    struct fs_ops *ops = fs_for_path(&p);
    void *mnt = mount_priv(ops);
    if (!ops || !ops->mkdir) return LNXRM_ENOSYS;
    return ops->mkdir(mnt, p);
}

long sys_unlink(const char *path)
{
    char abuf[128];
    path = abs_path(path, abuf, sizeof(abuf));
    const char *p = path;
    struct fs_ops *ops = fs_for_path(&p);
    void *mnt = mount_priv(ops);
    if (!ops || !ops->unlink) return LNXRM_ENOSYS;
    return ops->unlink(mnt, p);
}

long sys_rmdir(const char *path)
{
    char abuf[128];
    path = abs_path(path, abuf, sizeof(abuf));
    const char *p = path;
    struct fs_ops *ops = fs_for_path(&p);
    void *mnt = mount_priv(ops);
    if (!ops || !ops->rmdir) return LNXRM_ENOSYS;
    return ops->rmdir(mnt, p);
}

/* Route a two-path namespace operation to the filesystem owning both ends. */
static long vfs_rename_path(const char *oldpath, const char *newpath)
{
    char abuf1[128], abuf2[128];
    oldpath = abs_path(oldpath, abuf1, sizeof(abuf1));
    newpath = abs_path(newpath, abuf2, sizeof(abuf2));
    const char *p1 = oldpath, *p2 = newpath;
    struct fs_ops *o1 = fs_for_path(&p1);
    struct fs_ops *o2 = fs_for_path(&p2);
    if (!o1 || !o2) return LNXRM_ENOENT;
    if (o1 != o2) return LNXRM_EXDEV; /* rename(2) across filesystems */
    if (!o1->rename) return LNXRM_ENOSYS;
    return o1->rename(mount_priv(o1), p1, p2);
}

long sys_rename(const char *oldpath, const char *newpath)
{ return vfs_rename_path(oldpath, newpath); }

/* Trailing slashes are meaningless for a move source/destination. */
static void strip_trailing_slashes(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
}

/* move(src, destdir) -> rename(src, destdir/<basename(src)>), the classic
 * "mv into a directory" semantics; the fs layer enforces that destdir
 * exists and is a directory. */
long sys_move(const char *src, const char *destdir)
{
    char sbuf[128], dbuf[128], out[256];
    strncpy(sbuf, src, sizeof(sbuf) - 1);
    sbuf[sizeof(sbuf) - 1] = 0;
    strncpy(dbuf, destdir, sizeof(dbuf) - 1);
    dbuf[sizeof(dbuf) - 1] = 0;
    strip_trailing_slashes(sbuf);
    strip_trailing_slashes(dbuf);

    const char *base = strrchr(sbuf, '/');
    base = base ? base + 1 : sbuf;
    if (!base[0]) return LNXRM_EINVAL; /* refused to move the root */

    size_t dl = strlen(dbuf), bl = strlen(base);
    if (dl + bl + 2 > sizeof(out)) return LNXRM_EINVAL;
    memcpy(out, dbuf, dl);
    if (dl == 0 || dbuf[dl - 1] != '/') out[dl++] = '/';
    memcpy(out + dl, base, bl + 1);
    return vfs_rename_path(sbuf, out);
}

long sys_read(int fd, void *ubuf, size_t n)
{
    struct file *f = fd_get(fd);
    if (!f) return LNXRM_EBADF;
    if (f->is_dir) return LNXRM_EFAIL;
    /* Per-syscall stack buffer: the old static kbuf was shared by all
     * tasks and broke under concurrent reads (SMP + preemption). */
    char kbuf[4096];
    if (n > sizeof(kbuf)) n = sizeof(kbuf);
    long r = f->ops->read(f, kbuf, n);
    if (r > 0 && copy_to_user(ubuf, kbuf, r) < 0) return LNXRM_EFAULT;
    return r;
}

long sys_write(int fd, const void *ubuf, size_t n)
{
    struct file *f = fd_get(fd);
    if (!f) return LNXRM_EBADF;
    char kbuf[4096];
    if (n > sizeof(kbuf)) n = sizeof(kbuf);
    if (copy_from_user(kbuf, ubuf, n) < 0) return LNXRM_EFAULT;
    return f->ops->write(f, kbuf, n);
}

long sys_lseek(int fd, long off, int whence)
{
    struct file *f = fd_get(fd);
    if (!f) return LNXRM_EBADF;
    if (!f->ops->lseek) return LNXRM_ENOSYS;
    return f->ops->lseek(f, off, whence);
}

long sys_getdent(int fd, void *ubuf, size_t len)
{
    struct file *f = fd_get(fd);
    if (!f || !f->is_dir) return LNXRM_EBADF;
    char kbuf[1024];
    if (len > sizeof(kbuf)) len = sizeof(kbuf);
    long r = f->ops->getdent(f, kbuf, len);
    if (r > 0 && copy_to_user(ubuf, kbuf, r) < 0) return LNXRM_EFAULT;
    return r;
}

/* ---------------- generic file/dir fops bridging to fs_ops -------------- */

static long reg_read(struct file *f, void *buf, size_t n)
{
    struct vnode *vn = f->priv;
    long r = vn->ops->read(vn->mnt_data, vn->fs_data, f->pos, buf, n);
    if (r > 0) f->pos += r;
    return r;
}

static long reg_write(struct file *f, const void *buf, size_t n)
{
    struct vnode *vn = f->priv;
    long r = vn->ops->write(vn->mnt_data, vn->fs_data, f->pos, buf, n);
    if (r > 0) f->pos += r;
    return r;
}

static long reg_lseek(struct file *f, long off, int whence)
{
    struct vnode *vn = f->priv;
    u64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->pos : vn->size;
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
        int (*fn)(void *, void *, u64 *, struct dirent_out *) =
            it->ops ? it->ops->getdent : NULL;

        if (!fn) break;

        if (fn(it->mnt_data, it->node, &it->cookie, &d) < 0) break;
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

static int noop_close(struct file *f)
{ return 0; }
static long noop_read(struct file *f, void *buf, size_t n)
{
    (void)f;
    (void)buf;
    (void)n;
    return LNXRM_EFAIL;
}
static long noop_write(struct file *f, const void *buf, size_t n)
{
    (void)f;
    (void)buf;
    (void)n;
    return LNXRM_EFAIL;
}

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
    .read = noop_read,
    .write = noop_write,
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
        if (c < 0) c = lnxrm_uart_trygetc(); /* polled fallback */
        if (c >= 0) {
            p[got++] = (char)c;
            if (c == '\n') break;
            continue;
        }

        /* No input available.  A signal (EINTR) beats sleeping. */
        if (current && current->signal_pending) return got ? (long)got : -4; /* -EINTR */

        /* Sleep until the next tick (1 jiffy safety net) or until
         * input_push() wakes us via console_waiter.  Arm the waiter
         * AFTER setting SLEEPING so a racing push either sees
         * T_SLEEPING and wakes us, or leaves a char that the recheck
         * below / the next loop iteration will pick up. */
        if (!current) return got ? (long)got : -5; /* -EIO, no task context */

        current->state = T_SLEEPING;
        current->sleep_until = jiffies + 1;
        console_waiter_arm(current);
        /* Recheck once more with the waiter armed: an input_push that
         * landed between the empty check and T_SLEEPING would not have
         * woken us (state wasn't SLEEPING yet) — but the char is now
         * in the ring. */
        c = input_pop();
        if (c < 0) c = lnxrm_uart_trygetc();
        if (c >= 0) {
            current->state = T_RUNNING;
            console_waiter_disarm(current);
            p[got++] = (char)c;
            if (c == '\n') break;
            continue;
        }

        runqueue_remove(current); /* off-queue invariant before sleep */
        schedule();
        console_waiter_disarm(current);
        current->state = T_RUNNING;
    }
    return got;
}

long console_write(struct file *f, const void *buf, size_t n)
{
    const char *p = buf;
    for (size_t i = 0; i < n; i++) console_putc(p[i]);
    return n;
}

/* ---------------- boot-time mounting ---------------- */
void vfs_init(void)
{
    blk_cache_init();
}

int vfs_mount_root(void)
{ return 0; }

struct blkdev *blk_first;
static struct blkdev *blk_list[8];
static int blk_count;

void blk_register(struct blkdev *b)
{
    if (blk_count < 8) blk_list[blk_count++] = b;
    if (!blk_first) blk_first = b;
    kprintf("[blk] registered %s (%llu sectors, %llu MiB)\n", b->name, b->num_sectors,
            b->num_sectors / 2048);
}

int blk_list_all(void *ubuf, int max)
{
    int n = blk_count < max ? blk_count : max;
    for (int i = 0; i < n; i++) {
        struct blkdev *b = blk_list[i];
        /* pack: name[16] + sector_size(u32) + num_sectors(u64) = 28 bytes */
        char tmp[28];
        memset(tmp, 0, sizeof(tmp));
        int len = 0;
        while (b->name[len] && len < 15) tmp[len] = b->name[len], len++;
        *(u32 *)(tmp + 16) = b->sector_size;
        *(u64 *)(tmp + 20) = b->num_sectors;
        if (copy_to_user((char *)ubuf + i * 28, tmp, 28) < 0) return i;
    }
    return n;
}

int vfs_try_mount_disk(void)
{
    extern struct blkdev *blk_first;
    if (!blk_first) return LNXRM_EFAIL;

    kprintf("[vfs] attempting disk mount on %s (%llu sectors, %llu MB)\n", blk_first->name,
            blk_first->num_sectors, blk_first->num_sectors / 2048);

    /* Flush cache before mounting to ensure consistent state */
    blk_cache_flush(blk_first);

    /* 1. Try whole disk as FAT32 (most common: no MBR partition table) */
    kprintf("[vfs] trying FAT32 (whole disk)...\n");
    void *m = fat_mount(blk_first);
    if (m) {
        register_mount(&fat32_ops, m, DISK_PREFIX);
        disk_ready = true;
        kprintf("[vfs] FAT32 mounted at %s (whole disk)\n", DISK_PREFIX);
        blk_cache_flush(blk_first);
        return 0;
    }

    /* 2. Check for MBR partition table */
    kprintf("[vfs] trying MBR partition table...\n");
    struct mbr_info mbr;
    if (mbr_parse(blk_first, &mbr) == 0 && mbr.part_count > 0) {
        kprintf("[vfs] MBR detected, %d partitions\n", mbr.part_count);

        for (int i = 0; i < mbr.part_count; i++) {
            if (mbr.parts[i].type == PART_TYPE_NONE) continue;

            kprintf("[vfs] partition %d: type=0x%02X (%s)\n", i, mbr.parts[i].type,
                    mbr_type_name(mbr.parts[i].type));

            /* Heap-allocate: fat_mount stores dev pointer in fat_priv */
            struct blkdev *part_dev = kmalloc(sizeof(*part_dev));
            if (!part_dev) continue;

            if (mbr_get_partition(&mbr, i, part_dev, blk_first) == 0) {
                void *pm = NULL;

                if (mbr.parts[i].type == PART_TYPE_FAT32 ||
                    mbr.parts[i].type == PART_TYPE_FAT32_LBA) {
                    kprintf("[vfs]   trying FAT32 on partition %d...\n", i);
                    pm = fat_mount(part_dev);
                    if (pm) {
                        register_mount(&fat32_ops, pm, DISK_PREFIX);
                        disk_ready = true;
                        kprintf("[vfs] FAT32 mounted at %s (partition %d)\n", DISK_PREFIX, i);
                        blk_cache_flush(blk_first);
                        return 0;
                    }
                }
            }
            kfree(part_dev);
        }

        if (disk_ready) {
            blk_cache_flush(blk_first);
            return 0;
        }
    }

    return LNXRM_EFAIL;
}
