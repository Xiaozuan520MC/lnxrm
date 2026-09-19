/* FAT32 filesystem driver over the generic block-device layer.
 * Supports: mount, lookup (with LFN), read, write/grow/truncate, create,
 * unlink, directory listing. */
#include <vfs.h>
#include <console.h>
#include <mm.h>
#include <blk_cache.h>
#include <fat32_journal.h>

void *fat_priv;                     /* active mount or NULL */
extern struct fs_ops fat32_ops;     /* defined at the bottom of this file */

static char toupper_(char c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

struct fat_mount {
    struct blkdev *dev;
    u32 bytes_per_sector;
    u32 sectors_per_cluster;
    u32 cluster_size;
    u32 reserved_sectors;
    u32 num_fats;
    u32 fatsz;                      /* sectors per FAT */
    u32 root_cluster;
    u64 data_start_lba;             /* cluster 2 */
    u32 max_cluster;
    u8 *fat;                        /* cached FAT table */
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

static int rd_sec(struct fat_mount *m, u64 lba, void *buf)
{
    /* Use block cache for better performance */
    return blk_cache_read(m->dev, lba, buf);
}

static int wr_sec(struct fat_mount *m, u64 lba, const void *buf)
{
    /* Use block cache for better performance */
    return blk_cache_write(m->dev, lba, buf);
}

static u32 fat_next(struct fat_mount *m, u32 clus)
{
    if (clus >= m->max_cluster)
        return 0x0FFFFFFF;
    return *(u32 *)&m->fat[clus * 4] & 0x0FFFFFFF;
}

static void fat_set(struct fat_mount *m, u32 clus, u32 val)
{
    if (clus >= m->max_cluster)
        return;
    u32 old = *(u32 *)&m->fat[clus * 4];
    *(u32 *)&m->fat[clus * 4] =
        (old & 0xF0000000) | (val & 0x0FFFFFFF);
    /* write back the touched sector */
    u64 off = clus * 4;
    m->dev->write(m->dev, m->reserved_sectors + off / m->bytes_per_sector,
                  1, &m->fat[off & ~(u64)(m->bytes_per_sector - 1)]);
}

static int clus_is_eoc(u32 c)
{
    return c >= 0x0FFFFFF8;
}

static u64 clus_lba(struct fat_mount *m, u32 clus)
{
    return m->data_start_lba + (u64)(clus - 2) * m->sectors_per_cluster;
}

static void *clus_buf_alloc(struct fat_mount *m)
{
    return kmalloc(m->cluster_size ? m->cluster_size : 4096);
}

/* read whole cluster chain into a kmalloc'd buffer; returns length */
static u64 chain_read(struct fat_mount *m, u32 start, u8 **out)
{
    if (!start || clus_is_eoc(start)) {
        *out = NULL;
        return 0;
    }
    u64 total = 0;
    for (u32 c = start; !clus_is_eoc(c) && c >= 2; c = fat_next(m, c))
        total += m->cluster_size;
    u8 *buf = kmalloc(total);
    u64 off = 0;
    for (u32 c = start; !clus_is_eoc(c) && c >= 2 && off < total;
         c = fat_next(m, c)) {
        m->dev->read(m->dev, clus_lba(m, c), m->sectors_per_cluster,
                     buf + off);
        off += m->cluster_size;
    }
    *out = buf;
    return total;
}

static bool ent_used(const struct fat_dirent *e)
{
    return e->name[0] != ENT_FREE && e->name[0] != ENT_E5;
}

static void short_to_name(const u8 *sn, char out[13])
{
    int o = 0;
    for (int i = 0; i < 8 && sn[i] != ' '; i++)
        out[o++] = sn[i];
    for (int i = 8; i < 11 && sn[i] != ' '; i++) {
        if (i == 8)
            out[o++] = '.';
        out[o++] = sn[i];
    }
    out[o] = 0;
}

static void name_to_short(const char *in, u8 out[11])
{
    memset(out, ' ', 11);
    int i = 0, o = 0;
    while (in[i] && in[i] != '.' && o < 8)
        out[o++] = toupper_(in[i++]);
    if (in[i] == '.') {
        i++;
        o = 8;
        while (in[i] && o < 11)
            out[o++] = toupper_(in[i++]);
    }
}

static bool name_eq_short(const char *want, const struct fat_dirent *e)
{
    u8 s[11];
    name_to_short(want, s);
    for (int i = 0; i < 11; i++)
        if (toupper_((char)e->name[i]) != s[i])
            return false;
    return true;
}

/* iterate dirents of a directory chain with LFN reconstruction.
 * fn returns nonzero to stop. ctx passed through. */
struct lfn_state {
    char longname[256];
    int seq_max;
};

typedef int (*dirent_cb)(const char *name, const struct fat_dirent *e,
                         void *ctx);

static int scan_dir(struct fat_mount *m, u32 dirclus, dirent_cb cb, void *ctx)
{
    u8 *buf = clus_buf_alloc(m);
    struct lfn_state lf;
    lf.longname[0] = 0;

    for (u32 c = dirclus; !clus_is_eoc(c) && c >= 2; c = fat_next(m, c)) {
        m->dev->read(m->dev, clus_lba(m, c), m->sectors_per_cluster, buf);
        for (u32 off = 0; off < m->cluster_size; off += DIRENT_SIZE) {
            struct fat_dirent *e = (struct fat_dirent *)(buf + off);
            if (e->name[0] == ENT_END)
                goto done;
            if (!ent_used(e))
                continue;
            if ((e->attr & ATTR_LFN) == ATTR_LFN) {
                /* LFN entry: assemble UTF16 pieces (stored in reverse
                 * order); access through a raw byte pointer because the
                 * pieces span past the 11-byte short-name array */
                const u8 *raw = (const u8 *)e;
                int seq = raw[0] & 0x1F;
                char piece[14];
                int pi = 0;
                static const int off[13] = { 1,3,5,7,9, 14,16,18,20,22, 28,30 };
                for (int k = 0; k < 13 && pi < 13; k++)
                    piece[pi++] = (char)raw[off[k]];
                piece[pi] = 0;
                size_t base = (size_t)(seq > 0 ? seq - 1 : 0) * 13;
                if (base + pi < sizeof(lf.longname))
                    memcpy(&lf.longname[base], piece, pi);
                if (seq > lf.seq_max)
                    lf.seq_max = seq;
                continue;
            }
            if (e->attr & ATTR_VOLUME)
                continue;

            char shortn[13];
            short_to_name(e->name, shortn);
            const char *nm = shortn;
            char assembled[256];
            if (lf.seq_max > 1 && lf.longname[0]) {
                memcpy(assembled, lf.longname, sizeof(assembled));
                nm = assembled;
            }
            lf.longname[0] = 0;
            lf.seq_max = 0;
            if (cb(nm, e, ctx)) {
                kfree(buf);
                return 1;
            }
        }
    }
done:
    kfree(buf);
    return 0;
}

struct find_ctx {
    const char *want;
    struct fat_dirent found;
};

static int dbg_scan;
static int cb_find(const char *name, const struct fat_dirent *e, void *ctx)
{
    struct find_ctx *f = ctx;
    if (dbg_scan < 8) {
        dbg_scan++;
        }
    if (!strcmp(name, f->want) || name_eq_short(f->want, e)) {
        f->found = *e;
        return 1;
    }
    return 0;
}

struct list_ctx {
    struct dir_iter *it;
    int emitted;
};

/* resolve a path to {parent_dir_cluster, dirent copy, name}. */
struct resolve {
    u32 dir_cluster;                /* parent's cluster */
    char name[56];
    struct fat_dirent de;           /* valid if found */
    bool found;
};

static bool resolve_path(struct fat_mount *m, const char *path,
                         struct resolve *r)
{
    r->found = false;
    r->dir_cluster = m->root_cluster;
    while (*path == '/')
        path++;
    if (!*path)
        return true;                /* root itself */

    char comp[56];
    const char *p = path;
    u32 cur = m->root_cluster;
    for (;;) {
        const char *sl = strchr(p, '/');
        size_t len = sl ? (size_t)(sl - p) : strlen(p);
        if (len >= sizeof(comp))
            return false;
        memcpy(comp, p, len);
        comp[len] = 0;
        bool last = !sl;

        struct find_ctx fc = { .want = comp };
        if (!scan_dir(m, cur, cb_find, &fc))
            return false;
        if (last) {
            r->de = fc.found;
            r->dir_cluster = cur;
            strncpy(r->name, comp, sizeof(r->name) - 1);
            r->found = true;
            return true;
        }
        if (!(fc.found.attr & ATTR_DIR))
            return false;
        cur = ((u32)fc.found.fstclushi << 16) | fc.found.fstcluslo;
        p = sl + 1;
        while (*p == '/')
            p++;
        if (!*p) {                  /* trailing slash: it's this dir */
            r->de = fc.found;
            r->dir_cluster = cur;
            r->found = true;
            strncpy(r->name, comp, sizeof(r->name) - 1);
            return true;
        }
    }
}

/* ---------------- fs_ops ---------------- */

static int fat_lookup(void *mnt, const char *path, struct vnode *out)
{
    struct fat_mount *m = fat_priv;
    struct resolve r;
    while (*path == '/')
        path++;
    if (!*path) {
        /* mount root directory */
        out->type = V_DIR;
        out->size = 0;
        out->ops = &fat32_ops;
        out->mount = (void *)1;
        out->fs_data = kmalloc(sizeof(struct resolve));
        memset(out->fs_data, 0, sizeof(struct resolve));
        ((struct resolve *)out->fs_data)->dir_cluster = m->root_cluster;
        ((struct resolve *)out->fs_data)->de.attr = ATTR_DIR;
        ((struct resolve *)out->fs_data)->de.fstcluslo =
            m->root_cluster & 0xFFFF;
        ((struct resolve *)out->fs_data)->de.fstclushi =
            m->root_cluster >> 16;
        return 0;
    }
    if (!resolve_path(m, path, &r) || !r.found)
        return -1;
    out->fs_data = kmalloc(sizeof(struct resolve));
    memcpy(out->fs_data, &r, sizeof(r));
    out->ops = &fat32_ops;
    out->mount = (void *)1;
    if (r.de.attr & ATTR_DIR) {
        out->type = V_DIR;
        out->size = 0;
    } else {
        out->type = V_REG;
        out->size = r.de.filesize;
    }
    return 0;
}

static int fat_read(void *mnt, void *node, u64 off, void *ubuf, size_t n)
{
    struct fat_mount *m = fat_priv;
    struct resolve *r = node;
    if (r->de.attr & ATTR_DIR)
        return -1;
    
    /* Validate buffer pointer */
    if (!ubuf && n > 0)
        return -1;
    
    u32 clus = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    u64 size = r->de.filesize;
    if (off >= size)
        return 0;
    if (off + n > size)
        n = size - off;

    /* Limit read size to prevent buffer overflow */
    if (n > 1024 * 1024)  /* Max 1MB per read */
        n = 1024 * 1024;

    u8 tmp[512];
    u64 done = 0;
    /* skip to offset */
    u64 skip = off;
    while (skip >= m->cluster_size && !clus_is_eoc(clus) && clus >= 2) {
        skip -= m->cluster_size;
        clus = fat_next(m, clus);
    }
    u32 in_clus = skip % m->cluster_size;
    while (done < n && !clus_is_eoc(clus) && clus >= 2) {
        u64 chunk = MIN(n - done, m->cluster_size - in_clus);
        
        /* Ensure chunk doesn't exceed sector size for safety */
        if (chunk > m->bytes_per_sector)
            chunk = m->bytes_per_sector;
        
        m->dev->read(m->dev,
                     clus_lba(m, clus) + in_clus / m->bytes_per_sector,
                     (chunk + m->bytes_per_sector - 1) / m->bytes_per_sector,
                     tmp);
        memcpy((u8 *)ubuf + done, tmp, chunk);
        done += chunk;
        in_clus = 0;
        clus = fat_next(m, clus);
    }
    return done;
}

static u32 find_free_cluster(struct fat_mount *m)
{
    for (u32 i = 2; i < m->max_cluster; i++)
        if ((*(u32 *)&m->fat[i * 4] & 0x0FFFFFFF) == 0)
            return i;
    return 0;
}

/* grow `first` so it covers need_bytes; returns (possibly new) head */
static u32 alloc_chain(struct fat_mount *m, u32 first, u64 need_bytes)
{
    u32 nclus = (need_bytes + m->cluster_size - 1) / m->cluster_size;
    u32 head = first, prev = 0, cnt = 0;

    if (first && clus_is_eoc(first))
        ;
    else if (first) {
        u32 c = first;
        while (!clus_is_eoc(c) && c >= 2) {
            prev = c;
            c = fat_next(m, c);
            cnt++;
        }
    } else
        cnt = 0;                    /* fresh chain */

    while (cnt < nclus) {
        u32 nc = find_free_cluster(m);
        if (!nc)
            return 0;               /* disk full */
        fat_set(m, nc, 0x0FFFFFFF);
        if (prev)
            fat_set(m, prev, nc);
        else
            head = nc;
        prev = nc;
        cnt++;
    }
    return head;
}

static int fat_write(void *mnt, void *node, u64 off, const void *buf, size_t n)
{
    struct fat_mount *m = fat_priv;
    struct resolve *r = node;
    if (r->de.attr & ATTR_DIR)
        return -1;
    if (!buf && !n) {               /* truncate request */
        r->de.filesize = 0;
        return 0;
    }
    
    /* Validate buffer pointer */
    if (!buf && n > 0)
        return -1;
    
    /* Limit write size to prevent buffer overflow */
    if (n > 1024 * 1024)  /* Max 1MB per write */
        n = 1024 * 1024;

    /* Start a transaction for atomic updates */
    fat32_journal_begin();

    u32 clus = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    u64 need = MAX(off + n, r->de.filesize);
    clus = alloc_chain(m, clus, need);
    if (!clus) {
        fat32_journal_abort();
        return -5;
    }
    r->de.fstclushi = clus >> 16;
    r->de.fstcluslo = clus & 0xFFFF;

    u64 done = 0;
    u64 skip = off;
    u32 c = clus;
    while (skip >= m->cluster_size && !clus_is_eoc(c) && c >= 2) {
        skip -= m->cluster_size;
        c = fat_next(m, c);
    }
    u32 in_clus = skip % m->cluster_size;
    u8 tmp[512];
    while (done < n && !clus_is_eoc(c) && c >= 2) {
        u64 lba = clus_lba(m, c) + in_clus / m->bytes_per_sector;
        u64 sec_off = in_clus % m->bytes_per_sector;
        u64 chunk = MIN(n - done, m->bytes_per_sector - sec_off);
        rd_sec(m, lba, tmp);
        memcpy(tmp + sec_off, (const u8 *)buf + done, chunk);
        
        /* Add to journal before writing */
        fat32_journal_add(lba, tmp);
        wr_sec(m, lba, tmp);
        
        done += chunk;
        in_clus += chunk;
        if (in_clus >= m->cluster_size) {
            in_clus = 0;
            c = fat_next(m, c);
        }
    }

    r->de.filesize = need > off + n ? r->de.filesize : off + n;

    /* update the on-disk dirent: rewrite parent dir sector */
    u8 dbuf[512];
    u32 dc = r->dir_cluster;
    kprintf("<rescan dc=%u>", dc);
    for (u32 cc = dc; !clus_is_eoc(cc) && cc >= 2; cc = fat_next(m, cc)) {
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u64 lba = clus_lba(m, cc) + s;
            rd_sec(m, lba, dbuf);
            for (u32 doff = 0; doff <= m->bytes_per_sector - DIRENT_SIZE;
                 doff += DIRENT_SIZE) {
                struct fat_dirent *e = (struct fat_dirent *)(buf + doff);
                if (e->name[0] == ENT_END)
                    break;
                if (!ent_used(e))
                    continue;
                if (!memcmp(e->name, r->de.name, 11)) {
                    e->filesize = r->de.filesize;
                    e->fstclushi = r->de.fstclushi;
                    e->fstcluslo = r->de.fstcluslo;
                    e->attr |= ATTR_ARCHIVE;
                    
                    /* Add to journal before writing */
                    fat32_journal_add(lba, dbuf);
                    wr_sec(m, lba, dbuf);
                    
                    /* Commit the transaction */
                    fat32_journal_commit();
                    return done;
                }
            }
        }
    }
    
    /* Commit the transaction */
    fat32_journal_commit();
    return done;
}

static int fat_create(void *mnt, const char *path)
{
    struct fat_mount *m = fat_priv;
    struct resolve pr;
    char full[128];
    strncpy(full, path, sizeof(full) - 1);
    full[127] = 0;

    /* find parent directory cluster */
    char parent[128] = "";
    char *slash = strrchr(full, '/');
    const char *base = slash ? slash + 1 : full;
    if (slash) {
        size_t pl = slash - full;
        if (pl >= sizeof(parent))
            return -1;
        memcpy(parent, full, pl);
        parent[pl] = 0;
    }
    u32 pdir;
    if (!parent[0]) {
        pdir = m->root_cluster;
    } else {
        struct resolve rr;
        if (!resolve_path(m, parent, &rr) || !rr.found ||
            !(rr.de.attr & ATTR_DIR))
            return -1;
        pdir = ((u32)rr.de.fstclushi << 16) | rr.de.fstcluslo;
    }
    if (!base[0])
        return -1;

    /* refuse duplicates */
    struct find_ctx fc = { .want = base };
    if (scan_dir(m, pdir, cb_find, &fc))
        return -17;

    /* find a free slot in the parent dir chain (extend if needed) */
    u8 buf[512];
    for (u32 cc = pdir;; cc = fat_next(m, cc)) {
        if (clus_is_eoc(cc) || cc < 2)
            break;
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u64 lba = clus_lba(m, cc) + s;
            rd_sec(m, lba, buf);
            for (u32 doff = 0; doff <= m->bytes_per_sector - DIRENT_SIZE;
                 doff += DIRENT_SIZE) {
                struct fat_dirent *e = (struct fat_dirent *)(buf + doff);
                if (e->name[0] == ENT_END || e->name[0] == ENT_FREE ||
                    e->name[0] == ENT_E5) {
                    memset(e, 0, DIRENT_SIZE);
                    name_to_short(base, e->name);
                    e->attr = ATTR_ARCHIVE;
                    e->crttime = 0x6000; e->crtdate = 0x5A00;
                    e->wrttime = 0x6000; e->wrtdate = 0x5A00;
                    wr_sec(m, lba, buf);
                    return 0;
                }
            }
        }
    }
    kprintf("[fat32] no free dirent slot for '%s'\n", base);
    return -1;
}

static int fat_unlink(void *mnt, const char *path)
{
    struct fat_mount *m = fat_priv;
    struct resolve r;
    if (!resolve_path(m, path, &r) || !r.found)
        return -2;
    if (r.de.attr & ATTR_DIR)
        return -1;
    
    /* Start a transaction for atomic updates */
    fat32_journal_begin();
    
    u32 clus = ((u32)r.de.fstclushi << 16) | r.de.fstcluslo;
    while (!clus_is_eoc(clus) && clus >= 2) {
        u32 nx = fat_next(m, clus);
        fat_set(m, clus, 0);
        clus = nx;
    }
    u8 buf[512];
    for (u32 cc = r.dir_cluster; !clus_is_eoc(cc) && cc >= 2;
         cc = fat_next(m, cc)) {
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u64 lba = clus_lba(m, cc) + s;
            rd_sec(m, lba, buf);
            for (u32 doff = 0; doff <= m->bytes_per_sector - DIRENT_SIZE;
                 doff += DIRENT_SIZE) {
                struct fat_dirent *e = (struct fat_dirent *)(buf + doff);
                if (e->name[0] == ENT_END) {
                    fat32_journal_abort();
                    return -2;
                }
                if (ent_used(e) && !memcmp(e->name, r.de.name, 11)) {
                    e->name[0] = ENT_E5;
                    
                    /* Add to journal before writing */
                    fat32_journal_add(lba, buf);
                    wr_sec(m, lba, buf);
                    
                    /* Commit the transaction */
                    fat32_journal_commit();
                    return 0;
                }
            }
        }
    }
    fat32_journal_abort();
    return -2;
}

struct iter_ctx {
    u64 target, seen;
    char name[56];
    u8 type;
    bool hit;
};

static int cb_iter(const char *name, const struct fat_dirent *e, void *v)
{
    struct iter_ctx *c = v;
    if (name[0] == '.')
        return 0;
    if (c->seen++ == c->target) {
        strncpy(c->name, name, sizeof(c->name) - 1);
        c->name[sizeof(c->name) - 1] = 0;
        c->type = (e->attr & ATTR_DIR) ? 4 : 8;
        c->hit = true;
        return 1;
    }
    return 0;
}

int fat_dir_iter(struct dir_iter *it, struct dirent_out *d)
{
    struct fat_mount *m = fat_priv;
    struct resolve *r = it->node;
    u32 dc = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    u64 want_idx = it->cookie >> 8;
    u8 seen_exhausted = it->cookie & 1;
    struct iter_ctx ctx = { .target = want_idx };

    if (seen_exhausted)
        return -1;
    scan_dir(m, dc, cb_iter, &ctx);
    if (!ctx.hit) {
        it->cookie |= 1;
        return -1;
    }
    it->cookie = ((want_idx + 1) << 8) | seen_exhausted;
    strncpy(d->name, ctx.name, sizeof(d->name) - 1);
    d->type = ctx.type;
    d->name[sizeof(d->name) - 1] = 0;
    return 0;
}

/* 4-arg wrapper matching fs_ops->getdent signature */
int fat32_getdent(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d)
{
    (void)mnt;
    struct dir_iter it = { .node = dirnode, .cookie = *cookie };
    int r = fat_dir_iter(&it, d);
    *cookie = it.cookie;
    return r;
}

static u64 fat_freespace(void *mnt)
{
    struct fat_mount *m = fat_priv;
    u64 free_clusters = 0;
    for (u32 i = 2; i < m->max_cluster; i++)
        if ((*(u32 *)&m->fat[i * 4] & 0x0FFFFFFF) == 0)
            free_clusters++;
    return free_clusters * m->cluster_size;
}

struct fs_ops fat32_ops = {
    .lookup = fat_lookup,
    .getdent = fat32_getdent,
    .read = fat_read,
    .write = fat_write,
    .create = fat_create,
    .mkdir = NULL,
    .unlink = fat_unlink,
};

/* ---------------- mount ---------------- */
void *fat_mount(struct blkdev *dev)
{
    u8 bpb[512];
    if (dev->read(dev, 0, 1, bpb))
        return NULL;

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

    if (m->bytes_per_sector != dev->sector_size ||
        !m->sectors_per_cluster || !m->fatsz ||
        (*(u16 *)&bpb[22] && bpb[13])) {
        /* Not a valid FAT32 filesystem */
        kfree(m);
        return NULL;
    }
    u16 rootents = *(u16 *)&bpb[17];

    u32 fat_start = m->reserved_sectors;
    u64 data_start = fat_start + (u64)m->num_fats * m->fatsz +
                     (rootents * 32 + m->bytes_per_sector - 1) /
                         m->bytes_per_sector;
    m->data_start_lba = data_start;
    m->max_cluster =
        totsec > data_start ? (totsec - data_start) / m->sectors_per_cluster + 2
                            : 2;
    m->cluster_size = m->bytes_per_sector * m->sectors_per_cluster;

    /* CRITICAL: clamp the cluster range to what the FAT table can hold.
     * Without this, chain walks / allocation scans read & WRITE past the
     * end of the cached FAT heap buffer, corrupting neighbouring objects
     * (this was the source of the "mystery" function-pointer corruption). */
    u32 fat_entries = m->fatsz * m->bytes_per_sector / 4;
    if (m->max_cluster >= fat_entries)
        m->max_cluster = fat_entries ? fat_entries - 1 : 2;

    m->fat_bytes = (u64)m->fatsz * m->bytes_per_sector;
    m->fat = kmalloc(m->fat_bytes);
    dev->read(dev, fat_start, m->fatsz, m->fat);

    kprintf("[fat32] mounted %s: %u MiB, %u B/sector x %u/cluster, "
            "root=%u\n",
            dev->name, totsec * m->bytes_per_sector >> 20,
            m->bytes_per_sector, m->sectors_per_cluster, m->root_cluster);

    {
        u8 probe[512];
        memset(probe, 0xAB, sizeof(probe));
        int rc1 = dev->read(dev, 0, 1, probe);
        u32 w0 = *(u32 *)probe;
        memset(probe, 0xAB, sizeof(probe));
        int rc2 = dev->read(dev, data_start, 1, probe);
        u32 w2 = *(u32 *)probe;
        kprintf("[fatprobe] lba0 rc=%d w=%08x | lba%lu rc=%d w=%08x\n",
                rc1, w0, data_start, rc2, w2);
    }

    /* Initialize journal for transaction support */
    fat32_journal_init(dev);

    fat_priv = m;
    return m;
}
