/* FAT32 driver core: cached FAT table access, mount, and the locked
 * fs_ops table.  Directory scanning lives in fat32_dir.c, file data in
 * fat32_file.c and namespace changes in fat32_meta.c; shared types are
 * in fat32_priv.h. */
#include "fat32_priv.h"

void *fat_priv; /* active mount or NULL */

/* Global FAT32 lock: serialises cluster allocation, journal transactions,
 * directory scans and FAT updates across CPUs.  Without it two tasks on
 * different CPUs can allocate the same free cluster (cross-linking) or
 * interleave journal begin/commit.  Lock order: fat -> cache -> driver. */
spinlock_t fat_fs_lock = SPINLOCK_INIT;

u32 fat_next_cluster(struct fat_mount *m, u32 clus)
{
    if (clus >= m->max_cluster) return 0x0FFFFFFF;
    return *(u32 *)&m->fat[clus * 4] & 0x0FFFFFFF;
}

void fat_set_entry(struct fat_mount *m, u32 clus, u32 val)
{
    if (clus >= m->max_cluster) return;
    u32 old = *(u32 *)&m->fat[clus * 4];
    *(u32 *)&m->fat[clus * 4] = (old & 0xF0000000) | (val & 0x0FFFFFFF);
    /* write back the touched sector */
    u64 off = clus * 4;
    m->dev->write(m->dev, m->reserved_sectors + off / m->bytes_per_sector, 1,
                  &m->fat[off & ~(u64)(m->bytes_per_sector - 1)]);
}

u32 fat_find_free_cluster(struct fat_mount *m)
{
    for (u32 i = 2; i < m->max_cluster; i++)
        if ((*(u32 *)&m->fat[i * 4] & 0x0FFFFFFF) == 0) return i;
    return 0;
}

static int fat32_getdent_locked(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d)
{
    FAT_ENTER();
    int r = fat32_getdent_impl(mnt, dirnode, cookie, d);
    FAT_LEAVE();
    return r;
}

static u64 fat_freespace_impl(void *mnt)
{
    struct fat_mount *m = fat_priv;
    u64 free_clusters = 0;
    for (u32 i = 2; i < m->max_cluster; i++)
        if ((*(u32 *)&m->fat[i * 4] & 0x0FFFFFFF) == 0) free_clusters++;
    return free_clusters * m->cluster_size;
}

/* --- locked wrappers: single global FS lock serialises all FAT32 ops --- */
static int fat_lookup(void *mnt, const char *path, struct vnode *out)
{
    FAT_ENTER();
    int r = fat_lookup_impl(mnt, path, out);
    FAT_LEAVE();
    return r;
}

static int fat_read(void *mnt, void *node, u64 off, void *ubuf, size_t n)
{
    FAT_ENTER();
    int r = fat_read_impl(mnt, node, off, ubuf, n);
    FAT_LEAVE();
    return r;
}

static int fat_write(void *mnt, void *node, u64 off, const void *buf, size_t n)
{
    FAT_ENTER();
    int r = fat_write_impl(mnt, node, off, buf, n);
    FAT_LEAVE();
    return r;
}

static int fat_create(void *mnt, const char *path)
{
    FAT_ENTER();
    int r = fat_create_impl(mnt, path);
    FAT_LEAVE();
    return r;
}

static int fat_mkdir(void *mnt, const char *path)
{
    FAT_ENTER();
    int r = fat_mkdir_impl(mnt, path);
    FAT_LEAVE();
    return r;
}

static int fat_unlink(void *mnt, const char *path)
{
    FAT_ENTER();
    int r = fat_unlink_impl(mnt, path);
    FAT_LEAVE();
    return r;
}

static int fat_rmdir(void *mnt, const char *path)
{
    FAT_ENTER();
    int r = fat_rmdir_impl(mnt, path);
    FAT_LEAVE();
    return r;
}

static int fat_rename(void *mnt, const char *oldpath, const char *newpath)
{
    FAT_ENTER();
    int r = fat_rename_impl(mnt, oldpath, newpath);
    FAT_LEAVE();
    return r;
}

static u64 fat_freespace(void *mnt)
{
    FAT_ENTER();
    u64 r = fat_freespace_impl(mnt);
    FAT_LEAVE();
    return r;
}

struct fs_ops fat32_ops = {
    .lookup = fat_lookup,
    .getdent = fat32_getdent_locked,
    .read = fat_read,
    .write = fat_write,
    .create = fat_create,
    .mkdir = fat_mkdir,
    .unlink = fat_unlink,
    .rmdir = fat_rmdir,
    .rename = fat_rename,
    .freespace = fat_freespace,
};

/* ---------------- mount ---------------- */
void *fat_mount(struct blkdev *dev)
{
    u8 bpb[512];
    if (dev->read(dev, 0, 1, bpb)) return NULL;

    struct fat_mount *m = kmalloc(sizeof(*m));
    memset(m, 0, sizeof(*m));
    m->dev = dev;
    m->bytes_per_sector = *(u16 *)&bpb[11];
    m->sectors_per_cluster = bpb[13];
    m->reserved_sectors = *(u16 *)&bpb[14];
    m->num_fats = bpb[16];
    u32 fatsz16 = *(u16 *)&bpb[22];
    u32 fatsz32 = *(u32 *)&bpb[36];
    m->fatsz = fatsz16 ? fatsz16 : fatsz32;
    m->root_cluster = *(u32 *)&bpb[44];
    u32 totsec16 = *(u16 *)&bpb[19];
    u32 totsec32 = *(u32 *)&bpb[32];
    u32 totsec = totsec16 ? totsec16 : totsec32;

    if (m->bytes_per_sector != dev->sector_size || m->bytes_per_sector > 4096 ||
        !m->sectors_per_cluster || !m->fatsz || !m->reserved_sectors ||
        (m->num_fats != 1 && m->num_fats != 2) || m->root_cluster < 2 ||
        (*(u16 *)&bpb[22] && bpb[13])) {
        /* Not a valid FAT32 filesystem */
        kfree(m);
        return NULL;
    }
    u16 rootents = *(u16 *)&bpb[17];

    /* totsec must not exceed the real device; a forged BPB could make
     * cluster->LBA math run off the end of the disk. */
    if (totsec && dev->num_sectors && totsec > dev->num_sectors) totsec = (u32)dev->num_sectors;

    u32 fat_start = m->reserved_sectors;
    u64 data_start = fat_start + (u64)m->num_fats * m->fatsz +
                     (rootents * 32 + m->bytes_per_sector - 1) / m->bytes_per_sector;
    if (data_start >= totsec) {
        kfree(m);
        return NULL;
    }
    m->data_start_lba = data_start;
    m->max_cluster = totsec > data_start ? (totsec - data_start) / m->sectors_per_cluster + 2 : 2;
    m->cluster_size = m->bytes_per_sector * m->sectors_per_cluster;

    /* CRITICAL: clamp the cluster range to what the FAT table can hold.
     * Without this, chain walks / allocation scans read & WRITE past the
     * end of the cached FAT heap buffer, corrupting neighbouring objects
     * (this was the source of the "mystery" function-pointer corruption). */
    u32 fat_entries = m->fatsz * m->bytes_per_sector / 4;
    if (m->max_cluster >= fat_entries) m->max_cluster = fat_entries ? fat_entries - 1 : 2;

    m->fat_bytes = (u64)m->fatsz * m->bytes_per_sector;
    m->fat = kmalloc(m->fat_bytes);
    if (!m->fat || dev->read(dev, fat_start, m->fatsz, m->fat)) {
        kfree(m->fat);
        kfree(m);
        return NULL;
    }

    kprintf("[fat32] mounted %s: %u MiB, %u B/sector x %u/cluster, "
            "root=%u\n",
            dev->name, totsec * m->bytes_per_sector >> 20, m->bytes_per_sector,
            m->sectors_per_cluster, m->root_cluster);

    {
        u8 probe[512];
        memset(probe, 0xAB, sizeof(probe));
        int rc1 = dev->read(dev, 0, 1, probe);
        u32 w0 = *(u32 *)probe;
        memset(probe, 0xAB, sizeof(probe));
        int rc2 = dev->read(dev, data_start, 1, probe);
        u32 w2 = *(u32 *)probe;
        kprintf("[fatprobe] lba0 rc=%d w=%08x | lba%lu rc=%d w=%08x\n", rc1, w0, data_start, rc2,
                w2);
    }

    /* Initialize journal for transaction support */
    fat32_journal_init(dev);

    fat_priv = m;
    return m;
}
