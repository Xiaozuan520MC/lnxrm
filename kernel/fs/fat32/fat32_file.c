/* FAT32 file layer: path lookup plus read / write-grow-truncate of
 * regular files (cluster chains are extended through alloc_chain). */
#include "fat32_priv.h"

int fat_lookup_impl(void *mnt, const char *path, struct vnode *out)
{
    struct fat_mount *m = fat_priv;
    struct resolve r;
    while (*path == '/') path++;
    if (!*path) {
        /* mount root directory */
        out->type = V_DIR;
        out->size = 0;
        out->ops = &fat32_ops;
        out->mnt_data = (void *)1;
        out->fs_data = kmalloc(sizeof(struct resolve));
        memset(out->fs_data, 0, sizeof(struct resolve));
        ((struct resolve *)out->fs_data)->dir_cluster = m->root_cluster;
        ((struct resolve *)out->fs_data)->de.attr = ATTR_DIR;
        ((struct resolve *)out->fs_data)->de.fstcluslo = m->root_cluster & 0xFFFF;
        ((struct resolve *)out->fs_data)->de.fstclushi = m->root_cluster >> 16;
        return 0;
    }
    if (!fat_resolve_path(m, path, &r) || !r.found) return LNXRM_EFAIL;
    out->fs_data = kmalloc(sizeof(struct resolve));
    memcpy(out->fs_data, &r, sizeof(r));
    out->ops = &fat32_ops;
    out->mnt_data = (void *)1;
    if (r.de.attr & ATTR_DIR) {
        out->type = V_DIR;
        out->size = 0;
    } else {
        out->type = V_REG;
        out->size = r.de.filesize;
    }
    return 0;
}

int fat_read_impl(void *mnt, void *node, u64 off, void *ubuf, size_t n)
{
    struct fat_mount *m = fat_priv;
    struct resolve *r = node;
    if (r->de.attr & ATTR_DIR) return LNXRM_EFAIL;

    /* Validate buffer pointer */
    if (!ubuf && n > 0) return LNXRM_EFAIL;

    u32 clus = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    u64 size = r->de.filesize;
    if (off >= size) return 0;
    if (off + n > size) n = size - off;

    /* Limit read size to prevent buffer overflow */
    if (n > 1024 * 1024) /* Max 1MB per read */
        n = 1024 * 1024;

    u8 tmp[512];
    u64 done = 0;
    /* skip to offset */
    u64 skip = off;
    while (skip >= m->cluster_size && !cluster_is_eoc(clus) && clus >= 2) {
        skip -= m->cluster_size;
        clus = fat_next_cluster(m, clus);
    }
    u32 in_clus = skip % m->cluster_size;
    while (done < n && !cluster_is_eoc(clus) && clus >= 2) {
        u64 sec_off = in_clus % m->bytes_per_sector;
        u64 chunk = MIN(n - done, m->bytes_per_sector - sec_off);

        if (m->dev->read(m->dev, cluster_lba(m, clus) + in_clus / m->bytes_per_sector, 1, tmp))
            break; /* I/O error: return what we got */
        memcpy((u8 *)ubuf + done, tmp + sec_off, chunk);
        done += chunk;
        in_clus += chunk;
        /* advance to the next cluster only after the current one is
         * fully consumed -- never skip its remaining sectors */
        if (in_clus >= m->cluster_size) {
            in_clus = 0;
            clus = fat_next_cluster(m, clus);
        }
    }
    return done;
}

/* grow `first` so it covers need_bytes; returns (possibly new) head */
static u32 alloc_chain(struct fat_mount *m, u32 first, u64 need_bytes)
{
    u32 nclus = (need_bytes + m->cluster_size - 1) / m->cluster_size;
    u32 head = first, prev = 0, cnt = 0;

    if (first && cluster_is_eoc(first))
        ;
    else if (first) {
        u32 c = first;
        while (!cluster_is_eoc(c) && c >= 2) {
            prev = c;
            c = fat_next_cluster(m, c);
            cnt++;
        }
    } else
        cnt = 0; /* fresh chain */

    while (cnt < nclus) {
        u32 nc = fat_find_free_cluster(m);
        if (!nc) return 0; /* disk full */
        fat_set_entry(m, nc, 0x0FFFFFFF);
        if (prev)
            fat_set_entry(m, prev, nc);
        else
            head = nc;
        prev = nc;
        cnt++;
    }
    return head;
}

int fat_write_impl(void *mnt, void *node, u64 off, const void *buf, size_t n)
{
    struct fat_mount *m = fat_priv;
    struct resolve *r = node;
    if (r->de.attr & ATTR_DIR) return LNXRM_EFAIL;
    if (!buf && !n) { /* truncate request */
        r->de.filesize = 0;
        return 0;
    }

    /* Validate buffer pointer */
    if (!buf && n > 0) return LNXRM_EFAIL;

    /* Limit write size to prevent buffer overflow */
    if (n > 1024 * 1024) /* Max 1MB per write */
        n = 1024 * 1024;

    /* Start a transaction for atomic updates */
    fat32_journal_begin();

    u32 clus = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    u64 need = MAX(off + n, r->de.filesize);
    clus = alloc_chain(m, clus, need);
    if (!clus) {
        fat32_journal_abort();
        return LNXRM_EIO;
    }
    r->de.fstclushi = clus >> 16;
    r->de.fstcluslo = clus & 0xFFFF;

    u64 done = 0;
    u64 skip = off;
    u32 c = clus;
    while (skip >= m->cluster_size && !cluster_is_eoc(c) && c >= 2) {
        skip -= m->cluster_size;
        c = fat_next_cluster(m, c);
    }
    u32 in_clus = skip % m->cluster_size;
    u8 tmp[512];
    while (done < n && !cluster_is_eoc(c) && c >= 2) {
        u64 lba = cluster_lba(m, c) + in_clus / m->bytes_per_sector;
        u64 sec_off = in_clus % m->bytes_per_sector;
        u64 chunk = MIN(n - done, m->bytes_per_sector - sec_off);
        fat_read_sector(m, lba, tmp);
        memcpy(tmp + sec_off, (const u8 *)buf + done, chunk);

        /* Add to journal before writing */
        fat32_journal_add(lba, tmp);
        fat_write_sector(m, lba, tmp);

        done += chunk;
        in_clus += chunk;
        if (in_clus >= m->cluster_size) {
            in_clus = 0;
            c = fat_next_cluster(m, c);
        }
    }

    r->de.filesize = need > off + n ? r->de.filesize : off + n;

    /* update the on-disk dirent: rewrite parent dir sector */
    u8 dbuf[512];
    u32 dc = r->dir_cluster;
    for (u32 cc = dc; !cluster_is_eoc(cc) && cc >= 2; cc = fat_next_cluster(m, cc)) {
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u64 lba = cluster_lba(m, cc) + s;
            fat_read_sector(m, lba, dbuf);
            for (u32 doff = 0; doff <= m->bytes_per_sector - DIRENT_SIZE; doff += DIRENT_SIZE) {
                struct fat_dirent *e = (struct fat_dirent *)(dbuf + doff);
                if (e->name[0] == ENT_END) goto done_write;
                if (!entry_used(e)) continue;
                if (!memcmp(e->name, r->de.name, 11)) {
                    e->filesize = r->de.filesize;
                    e->fstclushi = r->de.fstclushi;
                    e->fstcluslo = r->de.fstcluslo;
                    e->attr |= ATTR_ARCHIVE;

                    fat32_journal_add(lba, dbuf);
                    fat_write_sector(m, lba, dbuf);

                    fat32_journal_commit();
                    return done;
                }
            }
        }
    }

done_write:
    fat32_journal_commit();
    return done;
}
