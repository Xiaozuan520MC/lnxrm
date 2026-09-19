/* ext2 filesystem driver (read-only) */
#include <ext2.h>
#include <vfs.h>
#include <console.h>
#include <mm.h>
#include <blk_cache.h>
#include <string.h>

struct ext2_mount *ext2_priv;
static struct fs_ops ext2_fs_ops;

/* Read a block from disk */
static int ext2_read_block(struct ext2_mount *m, u32 block, void *buf)
{
    u32 sectors_per_block = m->block_size / 512;
    u64 lba = (u64)block * sectors_per_block;
    for (u32 i = 0; i < sectors_per_block; i++) {
        if (blk_cache_read(m->dev, lba + i, (u8*)buf + i * 512))
            return -1;
    }
    return 0;
}

/* Get block group descriptor */
static struct ext2_bgd *ext2_get_bgd(struct ext2_mount *m, u32 group)
{
    return &m->bgd[group];
}

/* Get inode table block offset */
static u32 ext2_inode_table_block(struct ext2_mount *m, u32 group)
{
    return ext2_get_bgd(m, group)->inode_table;
}

/* Read an inode */
static int ext2_read_inode(struct ext2_mount *m, u32 inode_num, struct ext2_inode *out)
{
    if (inode_num == 0)
        return -1;
    
    u32 group = (inode_num - 1) / m->sb.inodes_per_group;
    u32 index = (inode_num - 1) % m->sb.inodes_per_group;
    u32 table_block = ext2_inode_table_block(m, group);
    u32 offset = index * m->inode_size;
    
    /* Calculate block and offset within block */
    u32 block_offset = offset / m->block_size;
    u32 byte_offset = offset % m->block_size;
    
    u8 *block_buf = kmalloc(m->block_size);
    if (!block_buf)
        return -1;
    
    if (ext2_read_block(m, table_block + block_offset, block_buf)) {
        kfree(block_buf);
        return -1;
    }
    
    memcpy(out, block_buf + byte_offset, sizeof(struct ext2_inode));
    kfree(block_buf);
    return 0;
}

/* Get block number for file offset */
static u32 ext2_get_block(struct ext2_mount *m, struct ext2_inode *inode, u32 file_block)
{
    if (file_block < 12) {
        return inode->block[file_block];
    }
    
    u32 ptrs_per_block = m->block_size / 4;
    file_block -= 12;
    
    /* Single indirect */
    if (file_block < ptrs_per_block) {
        u32 indirect_buf[m->block_size / 4];
        if (!inode->block[12])
            return 0;
        if (ext2_read_block(m, inode->block[12], indirect_buf))
            return 0;
        return indirect_buf[file_block];
    }
    file_block -= ptrs_per_block;
    
    /* Double indirect */
    if (file_block < ptrs_per_block * ptrs_per_block) {
        u32 double_buf[m->block_size / 4];
        if (!inode->block[13])
            return 0;
        if (ext2_read_block(m, inode->block[13], double_buf))
            return 0;
        u32 idx1 = file_block / ptrs_per_block;
        u32 idx2 = file_block % ptrs_per_block;
        if (!double_buf[idx1])
            return 0;
        u32 single_buf[m->block_size / 4];
        if (ext2_read_block(m, double_buf[idx1], single_buf))
            return 0;
        return single_buf[idx2];
    }
    
    return 0;
}

/* Read data from inode */
static u64 ext2_read_inode_data(struct ext2_mount *m, struct ext2_inode *inode,
                                u64 offset, void *buf, u64 count)
{
    u64 file_size = inode->size;
    if (inode->mode & EXT2_S_IFDIR) {
        /* For directories, use dir_acl for large files */
        file_size |= ((u64)inode->dir_acl) << 32;
    }
    
    if (offset >= file_size)
        return 0;
    if (offset + count > file_size)
        count = file_size - offset;
    
    u64 done = 0;
    while (done < count) {
        u32 file_block = (u32)((offset + done) / m->block_size);
        u32 block_offset = (offset + done) % m->block_size;
        u32 to_read = m->block_size - block_offset;
        if (to_read > count - done)
            to_read = (u32)(count - done);
        
        u32 disk_block = ext2_get_block(m, inode, file_block);
        if (!disk_block) {
            /* Sparse file - zero the buffer */
            memset((u8*)buf + done, 0, to_read);
            done += to_read;
            continue;
        }
        
        u8 *block_buf = kmalloc(m->block_size);
        if (!block_buf)
            return done;
        
        if (ext2_read_block(m, disk_block, block_buf)) {
            kfree(block_buf);
            return done;
        }
        
        memcpy((u8*)buf + done, block_buf + block_offset, to_read);
        kfree(block_buf);
        done += to_read;
    }
    return done;
}

/* Lookup helper - find entry in directory inode */
struct ext2_lookup_result {
    u32 inode_num;
    struct ext2_inode inode;
    bool found;
};

static int ext2_lookup_in_dir(struct ext2_mount *m, struct ext2_inode *dir_inode,
                              const char *name, struct ext2_lookup_result *out)
{
    u64 dir_size = dir_inode->size;
    u8 *dir_buf = kmalloc(dir_size);
    if (!dir_buf)
        return -1;
    
    u64 read = ext2_read_inode_data(m, dir_inode, 0, dir_buf, dir_size);
    if (read != dir_size) {
        kfree(dir_buf);
        return -1;
    }
    
    u64 offset = 0;
    while (offset < dir_size) {
        struct ext2_dirent *de = (struct ext2_dirent *)(dir_buf + offset);
        if (de->rec_len == 0)
            break;
        
        if (de->inode != 0 && de->name_len == strlen(name)) {
            if (memcmp(de->name, name, de->name_len) == 0) {
                out->inode_num = de->inode;
                out->found = true;
                kfree(dir_buf);
                return ext2_read_inode(m, de->inode, &out->inode);
            }
        }
        
        offset += de->rec_len;
    }
    
    kfree(dir_buf);
    out->found = false;
    return 0;
}

/* Resolve path to inode */
static int ext2_resolve_path(struct ext2_mount *m, const char *path, 
                             struct ext2_inode *out, u32 *out_ino)
{
    struct ext2_inode current;
    u32 current_ino = 2; /* root inode */
    
    if (ext2_read_inode(m, current_ino, &current))
        return -1;
    
    char buf[256];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    
    /* Skip leading slashes */
    char *p = buf;
    while (*p == '/') p++;
    
    if (!*p) {
        *out = current;
        if (out_ino) *out_ino = current_ino;
        return 0;
    }
    
    while (*p) {
        char *slash = strchr(p, '/');
        if (slash) *slash = 0;
        
        struct ext2_lookup_result lr;
        if (ext2_lookup_in_dir(m, &current, p, &lr) || !lr.found) {
            return -1;
        }
        
        current = lr.inode;
        current_ino = lr.inode_num;
        
        if (!slash) break;
        p = slash + 1;
        while (*p == '/') p++;
    }
    
    *out = current;
    if (out_ino) *out_ino = current_ino;
    return 0;
}

/* ---- VFS ops ---- */

/* VFS resolve result */
struct ext2_resolve {
    u32 inode_num;
    struct ext2_inode inode;
};

static int ext2_vfs_lookup(void *mnt, const char *path, struct vnode *out)
{
    struct ext2_mount *m = ext2_priv;
    struct ext2_inode inode;
    u32 ino;
    
    while (*path == '/') path++;
    if (!*path) {
        /* Root directory */
        if (ext2_read_inode(m, 2, &inode))
            return -1;
        ino = 2;
    } else {
        if (ext2_resolve_path(m, path, &inode, &ino))
            return -1;
    }
    
    out->fs_data = kmalloc(sizeof(struct ext2_resolve));
    if (!out->fs_data)
        return -1;
    
    struct ext2_resolve *r = out->fs_data;
    r->inode_num = ino;
    r->inode = inode;
    out->ops = &ext2_fs_ops;
    out->mount = (void*)1;
    
    if ((inode.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        out->type = V_DIR;
        out->size = 0;
    } else {
        out->type = V_REG;
        out->size = inode.size;
    }
    return 0;
}

static int ext2_vfs_read(void *mnt, void *node, u64 off, void *buf, size_t n)
{
    struct ext2_mount *m = ext2_priv;
    struct ext2_resolve *r = node;
    
    if (r->inode.mode & EXT2_S_IFDIR)
        return -1;
    
    return (int)ext2_read_inode_data(m, &r->inode, off, buf, n);
}

/* Directory iteration */
struct ext2_iter_ctx {
    struct dir_iter *it;
    struct dirent_out *d;
    int status;  /* 0=found, -1=not found */
};

static int ext2_dir_iterate(struct ext2_mount *m, struct ext2_inode *dir_inode,
                            u64 cookie, struct dirent_out *d)
{
    u64 dir_size = dir_inode->size;
    u8 *dir_buf = kmalloc(dir_size);
    if (!dir_buf)
        return -1;
    
    if (ext2_read_inode_data(m, dir_inode, 0, dir_buf, dir_size) != dir_size) {
        kfree(dir_buf);
        return -1;
    }
    
    u64 offset = 0;
    u64 idx = 0;
    int result = -1;
    
    while (offset < dir_size) {
        struct ext2_dirent *de = (struct ext2_dirent *)(dir_buf + offset);
        if (de->rec_len == 0)
            break;
        
        if (de->inode != 0 && de->name[0] != '.') {
            if (idx == cookie) {
                memcpy(d->name, de->name, de->name_len);
                d->name[de->name_len] = 0;
                d->type = de->file_type;
                result = 0;
                break;
            }
            idx++;
        }
        
        offset += de->rec_len;
    }
    
    kfree(dir_buf);
    return result;
}

static int ext2_vfs_getdent(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d)
{
    struct ext2_mount *m = ext2_priv;
    struct ext2_resolve *r = dirnode;
    
    if (ext2_dir_iterate(m, &r->inode, *cookie, d))
        return -1;
    
    (*cookie)++;
    return 0;
}

static u64 ext2_vfs_freespace(void *mnt)
{
    struct ext2_mount *m = ext2_priv;
    return (u64)m->sb.free_blocks_count * m->block_size;
}

/* ---- Mount ---- */
int ext2_mount(struct blkdev *dev)
{
    u8 buf[1024];
    
    /* Read superblock (at offset 1024) */
    for (int i = 0; i < 2; i++) {
        if (blk_cache_read(dev, 2 + i, (u8*)buf + i * 512))
            return -1;
    }
    
    struct ext2_superblock *sb = (struct ext2_superblock *)buf;
    
    /* Check magic */
    if (sb->magic != EXT2_SUPER_MAGIC) {
        kprintf("[ext2] invalid magic 0x%04X\n", sb->magic);
        return -1;
    }
    
    kprintf("[ext2] found superblock: inodes=%u blocks=%u\n",
            sb->inodes_count, sb->blocks_count);
    
    /* Allocate mount struct */
    struct ext2_mount *m = kmalloc(sizeof(*m));
    if (!m)
        return -1;
    memset(m, 0, sizeof(*m));
    
    m->dev = dev;
    memcpy(&m->sb, sb, sizeof(*sb));
    
    /* Calculate block size */
    m->block_size = 1024 << sb->log_block_size;
    m->inode_size = sb->inode_size ? sb->inode_size : 128;
    m->inodes_per_block = m->block_size / m->inode_size;
    m->blocks_per_group = sb->blocks_per_group;
    m->group_count = (sb->blocks_count - sb->first_data_block + 
                      sb->blocks_per_group - 1) / sb->blocks_per_group;
    
    kprintf("[ext2] block_size=%u inode_size=%u groups=%u\n",
            m->block_size, m->inode_size, m->group_count);
    
    /* Read block group descriptors */
    u32 bgd_block = sb->first_data_block + 1;
    u32 bgd_size = m->group_count * sizeof(struct ext2_bgd);
    u32 bgd_sectors = (bgd_size + 511) / 512;
    
    m->bgd = kmalloc(bgd_sectors * 512);
    if (!m->bgd) {
        kfree(m);
        return -1;
    }
    
    for (u32 i = 0; i < bgd_sectors; i++) {
        if (blk_cache_read(dev, bgd_block * (m->block_size / 512) + i,
                          (u8*)m->bgd + i * 512)) {
            kfree(m->bgd);
            kfree(m);
            return -1;
        }
    }
    
    ext2_priv = m;
    
    kprintf("[ext2] mounted: %u MiB, %u inodes, %u blocks\n",
            (u64)sb->blocks_count * m->block_size >> 20,
            sb->inodes_count, sb->blocks_count);
    
    return 0;
}

static struct fs_ops ext2_fs_ops = {
    .lookup = ext2_vfs_lookup,
    .getdent = ext2_vfs_getdent,
    .read = ext2_vfs_read,
    .write = NULL,  /* read-only for now */
    .create = NULL,
    .mkdir = NULL,
    .unlink = NULL,
    .freespace = ext2_vfs_freespace,
};

struct fs_ops ext2_ops = {
    .lookup = ext2_vfs_lookup,
    .getdent = ext2_vfs_getdent,
    .read = ext2_vfs_read,
    .write = NULL,
    .create = NULL,
    .mkdir = NULL,
    .unlink = NULL,
    .freespace = ext2_vfs_freespace,
};
