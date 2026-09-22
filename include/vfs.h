#pragma once
#include <types.h>


#ifdef __cplusplus
extern "C" {
#endif

enum vnode_type { V_REG, V_DIR, V_CHR };

struct file;

struct vnode {
    enum vnode_type type;
    u64 size;
    void *fs_data;                  /* fs-private node */
    struct fs_ops *ops;
    void *mount;                    /* owning mount */
};

struct dirent_out {
    char name[56];
    u8  type;
};

/* Filesystem-specific operations. All paths are relative to mount root. */
struct fs_ops {
    int  (*lookup)(void *mnt, const char *path, struct vnode *out);
    int  (*getdent)(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d);
    int  (*read)(void *mnt, void *node, u64 off, void *buf, size_t n);
    int  (*write)(void *mnt, void *node, u64 off, const void *buf, size_t n);
    int  (*create)(void *mnt, const char *path);
    int  (*mkdir)(void *mnt, const char *path);
    int  (*unlink)(void *mnt, const char *path);
    int  (*rmdir)(void *mnt, const char *path);
    u64  (*freespace)(void *mnt);
};

struct file_ops {
    long (*read)(struct file *, void *buf, size_t n);
    long (*write)(struct file *, const void *buf, size_t n);
    long (*lseek)(struct file *, long off, int whence);
    long (*getdent)(struct file *, void *ubuf, size_t n);
    int  (*close)(struct file *);
};

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_APPEND 02000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct dir_iter {
    void *node;
    void *mount;
    u64 cookie;
    void *extra;                    /* mount private data */
};

struct file {
    struct file_ops *ops;
    u64 pos;
    void *priv;                     /* vnode / device private */
    bool is_dir;
    int flags;
    int refcnt;
};

void vfs_init(void);
int  vfs_mount_root(void);          /* initramfs ramfs */
int  vfs_try_mount_disk(void);      /* FAT32 from first block dev -> / */
long vfs_open_file(const char *path, int flags, struct file **out);
size_t vfs_file_size(struct file *f);
long vfs_read_file(struct file *f, void *buf, size_t n);
void vfs_close_file(struct file *f);
extern struct blkdev *blk_first;

long sys_read(int fd, void *buf, size_t n);
long sys_write(int fd, const void *buf, size_t n);
long sys_close(int fd);
long sys_lseek(int fd, long off, int whence);
long sys_getdent(int fd, void *ubuf, size_t len);
long sys_dup2(int oldfd, int newfd);
long sys_mkdir(const char *upath);
long sys_unlink(const char *upath);

/* block devices (drivers/ahci.c registers; fs/fat32.c consumes) */
struct blkdev {
    const char *name;
    u32 sector_size;                /* bytes */
    u64 num_sectors;
    int (*read)(struct blkdev *, u64 lba, u32 count, void *buf);
    int (*write)(struct blkdev *, u64 lba, u32 count, const void *buf);
    void *drv;
};
void blk_register(struct blkdev *b);


#ifdef __cplusplus
}
#endif
