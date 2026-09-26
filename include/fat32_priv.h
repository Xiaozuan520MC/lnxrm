/* Shared internals of the FAT32 driver: on-disk structures, mount state,
 * path/cluster helpers and the cross-file entry points of
 * fat32.c / fat32_dir.c / fat32_file.c / fat32_meta.c.  Module-private:
 * nothing outside kernel/fs/ includes this header. */
#ifndef LNXRM_FAT32_PRIV_H
#define LNXRM_FAT32_PRIV_H

#include <sys/vfs.h>
#include <console.h>
#include <mm/mm.h>
#include <disk.h>
#include <sys/spinlock.h>
extern void *fat_priv;          /* active mount or NULL */
extern struct fs_ops fat32_ops; /* defined in fat32.c */

/* Global FAT32 lock: serialises cluster allocation, journal transactions,
 * directory scans and FAT updates across CPUs.  Without it two tasks on
 * different CPUs can allocate the same free cluster (cross-linking) or
 * interleave journal begin/commit.  Lock order: fat -> cache -> driver. */
extern spinlock_t fat_fs_lock;

#define FAT_ENTER()                                                                                \
    u64 _fat_fl;                                                                                   \
    spin_lock_irqsave(&fat_fs_lock, &_fat_fl)
#define FAT_LEAVE() spin_unlock_irqrestore(&fat_fs_lock, _fat_fl)

static inline char ascii_toupper(char c)
{ return c >= 'a' && c <= 'z' ? c - 32 : c; }

static inline char ascii_tolower(char c)
{ return c >= 'A' && c <= 'Z' ? c + 32 : c; }

struct fat_mount {
    struct blkdev *dev;
    u32 bytes_per_sector;
    u32 sectors_per_cluster;
    u32 cluster_size;
    u32 reserved_sectors;
    u32 num_fats;
    u32 fatsz; /* sectors per FAT */
    u32 root_cluster;
    u64 data_start_lba; /* cluster 2 */
    u32 max_cluster;
    u8 *fat; /* cached FAT table */
    u64 fat_bytes;
};

#define DIRENT_SIZE 32

struct fat_dirent {
    u8 name[11];
    u8 attr;
    u8 ntres;
    u8 crttenth;
    u16 crttime, crtdate, lstaccdate;
    u16 fstclushi;
    u16 wrttime, wrtdate;
    u16 fstcluslo;
    u32 filesize;
} __attribute__((packed));

#define ATTR_RO      0x01
#define ATTR_HIDDEN  0x02
#define ATTR_SYSTEM  0x04
#define ATTR_VOLUME  0x08
#define ATTR_DIR     0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN     0x0F
#define ENT_FREE     0x00
#define ENT_E5       0xE5
#define ENT_END      0x05

static inline int fat_read_sector(struct fat_mount *m, u64 lba, void *buf)
{
    /* Use block cache for better performance */
    return blk_cache_read(m->dev, lba, buf);
}

static inline int fat_write_sector(struct fat_mount *m, u64 lba, const void *buf)
{
    /* Use block cache for better performance */
    return blk_cache_write(m->dev, lba, buf);
}

static inline void name_to_short(const char *in, u8 out[11])
{
    memset(out, ' ', 11);
    int i = 0, o = 0;
    while (in[i] && in[i] != '.' && o < 8) out[o++] = ascii_toupper(in[i++]);
    if (in[i] == '.') {
        i++;
        o = 8;
        while (in[i] && o < 11) out[o++] = ascii_toupper(in[i++]);
    }
}

/* 8.3 short name -> display name ("FOO     TXT" -> "foo.txt") */
void fat_short_to_name(const u8 *sn, char out[13]);
/* does `want` match the stored short name (after 8.3 truncation)? */
bool fat_name_eq_short(const char *want, const struct fat_dirent *e);

static inline int cluster_is_eoc(u32 c)
{ return c >= 0x0FFFFFF8; }

static inline u64 cluster_lba(struct fat_mount *m, u32 clus)
{ return m->data_start_lba + (u64)(clus - 2) * m->sectors_per_cluster; }

static inline void *cluster_buf_alloc(struct fat_mount *m)
{ return kmalloc(m->cluster_size ? m->cluster_size : 4096); }

static inline bool entry_used(const struct fat_dirent *e)
{ return e->name[0] != ENT_FREE && e->name[0] != ENT_E5; }

/* Cluster / FAT table access (fat32.c). */
u32 fat_next_cluster(struct fat_mount *m, u32 clus);
void fat_set_entry(struct fat_mount *m, u32 clus, u32 val);
u32 fat_find_free_cluster(struct fat_mount *m);

/* Directory scan + path resolution (fat32_dir.c). */
typedef int (*dirent_cb)(const char *name, const struct fat_dirent *e, void *ctx);
struct find_ctx {
    const char *want;
    struct fat_dirent found;
};
struct resolve {
    u32 dir_cluster; /* parent's cluster */
    char name[56];
    struct fat_dirent de; /* valid if found */
    bool found;
};
int fat_scan_dir(struct fat_mount *m, u32 dirclus, dirent_cb cb, void *ctx);
int fat_scan_find_cb(const char *name, const struct fat_dirent *e, void *ctx);
bool fat_resolve_path(struct fat_mount *m, const char *path, struct resolve *r);
int fat_dir_iter(struct dir_iter *it, struct dirent_out *d);
int fat32_getdent_impl(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d);

/* Locked fs_ops implementations (fat32_file.c / fat32_meta.c). */
int fat_lookup_impl(void *mnt, const char *path, struct vnode *out);
int fat_read_impl(void *mnt, void *node, u64 off, void *ubuf, size_t n);
int fat_write_impl(void *mnt, void *node, u64 off, const void *buf, size_t n);
int fat_create_impl(void *mnt, const char *path);
int fat_mkdir_impl(void *mnt, const char *path);
int fat_rmdir_impl(void *mnt, const char *path);
int fat_unlink_impl(void *mnt, const char *path);
int fat_rename_impl(void *mnt, const char *oldpath, const char *newpath);

#endif /* LNXRM_FAT32_PRIV_H */
