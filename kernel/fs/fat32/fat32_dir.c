/* FAT32 directory layer: 8.3/LFN name handling, directory scanning,
 * path resolution and readdir (fat_dir_iter / getdent). */
#include "fat32_priv.h"

void fat_short_to_name(const u8 *sn, char out[13])
{
    int o = 0;
    for (int i = 0; i < 8 && sn[i] != ' '; i++) out[o++] = ascii_tolower((char)sn[i]);
    for (int i = 8; i < 11 && sn[i] != ' '; i++) {
        if (i == 8) out[o++] = '.';
        out[o++] = ascii_tolower((char)sn[i]);
    }
    out[o] = 0;
}

bool fat_name_eq_short(const char *want, const struct fat_dirent *e)
{
    u8 s[11];
    name_to_short(want, s);
    for (int i = 0; i < 11; i++)
        if (ascii_toupper((char)e->name[i]) != s[i]) return false;
    return true;
}

/* iterate dirents of a directory chain with LFN reconstruction.
 * fn returns nonzero to stop. ctx passed through. */
struct lfn_state {
    char longname[256];
    int seq_max;
    int name_end; /* highest byte written: the real name length */
};

/* Forget a partially assembled long name (entries that were skipped). */
static void lfn_reset(struct lfn_state *lf)
{
    lf->longname[0] = 0;
    lf->seq_max = 0;
    lf->name_end = 0;
}

int fat_scan_dir(struct fat_mount *m, u32 dirclus, dirent_cb cb, void *ctx)
{
    u8 *buf = cluster_buf_alloc(m);
    struct lfn_state lf;
    lfn_reset(&lf);

    for (u32 c = dirclus; !cluster_is_eoc(c) && c >= 2; c = fat_next_cluster(m, c)) {
        m->dev->read(m->dev, cluster_lba(m, c), m->sectors_per_cluster, buf);
        for (u32 off = 0; off < m->cluster_size; off += DIRENT_SIZE) {
            struct fat_dirent *e = (struct fat_dirent *)(buf + off);
            if (e->name[0] == ENT_END) goto done;
            if (!entry_used(e)) {
                /* Deleted entry: any LFN run around it is orphaned. */
                lfn_reset(&lf);
                continue;
            }
            if ((e->attr & ATTR_LFN) == ATTR_LFN) {
                /* LFN entry: assemble UTF16 pieces (stored in reverse
                 * order); access through a raw byte pointer because the
                 * pieces span past the 11-byte short-name array */
                const u8 *raw = (const u8 *)e;
                int seq = raw[0] & 0x1F;
                if (seq >= 1 && seq <= 20) {
                    char piece[14];
                    int pi = 0;
                    static const int off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (int k = 0; k < 13; k++) {
                        u16 ch = (u16)(raw[off[k]] | (raw[off[k] + 1] << 8));
                        /* 0xFFFF (and 0x0000) pad the final piece. */
                        if (ch == 0xFFFF || ch == 0) break;
                        piece[pi++] = (char)ch;
                    }
                    piece[pi] = 0;
                    size_t base = (size_t)(seq - 1) * 13;
                    if (base + pi < sizeof(lf.longname)) {
                        memcpy(&lf.longname[base], piece, pi);
                        /* Only the highest-numbered piece can be short, so
                         * keep the largest end offset seen so far. */
                        if ((int)(base + pi) > lf.name_end) lf.name_end = (int)(base + pi);
                    }
                    if (seq > lf.seq_max) lf.seq_max = seq;
                }
                continue;
            }
            if (e->attr & ATTR_VOLUME) continue;

            char shortn[13];
            fat_short_to_name(e->name, shortn);
            const char *nm = shortn;
            char assembled[256];
            if (lf.seq_max > 0 && lf.name_end > 0) {
                size_t ln = (size_t)lf.name_end;
                if (ln >= sizeof(lf.longname)) ln = sizeof(lf.longname) - 1;
                memcpy(assembled, lf.longname, ln);
                assembled[ln] = 0;
                nm = assembled;
            }
            lfn_reset(&lf);
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

int fat_scan_find_cb(const char *name, const struct fat_dirent *e, void *ctx)
{
    struct find_ctx *f = ctx;
    if (!strcasecmp(name, f->want) || fat_name_eq_short(f->want, e)) {
        f->found = *e;
        return 1;
    }
    return 0;
}

/* resolve a path to {parent_dir_cluster, dirent copy, name}. */
bool fat_resolve_path(struct fat_mount *m, const char *path, struct resolve *r)
{
    r->found = false;
    r->dir_cluster = m->root_cluster;
    while (*path == '/') path++;
    if (!*path) return true; /* root itself */

    char comp[56];
    const char *p = path;
    u32 cur = m->root_cluster;
    for (;;) {
        const char *sl = strchr(p, '/');
        size_t len = sl ? (size_t)(sl - p) : strlen(p);
        if (len >= sizeof(comp)) return false;
        memcpy(comp, p, len);
        comp[len] = 0;
        bool last = !sl;

        struct find_ctx fc = {.want = comp};
        if (!fat_scan_dir(m, cur, fat_scan_find_cb, &fc)) return false;
        if (last) {
            r->de = fc.found;
            r->dir_cluster = cur;
            strncpy(r->name, comp, sizeof(r->name) - 1);
            r->found = true;
            return true;
        }
        if (!(fc.found.attr & ATTR_DIR)) return false;
        cur = ((u32)fc.found.fstclushi << 16) | fc.found.fstcluslo;
        p = sl + 1;
        while (*p == '/') p++;
        if (!*p) { /* trailing slash: it's this dir */
            r->de = fc.found;
            r->dir_cluster = cur;
            r->found = true;
            strncpy(r->name, comp, sizeof(r->name) - 1);
            return true;
        }
    }
}

struct iter_ctx {
    u64 target, seen;
    char name[56];
    u8 type;
    bool hit;
};

static int scan_iter_cb(const char *name, const struct fat_dirent *e, void *v)
{
    struct iter_ctx *c = v;
    if (name[0] == '.') return 0;
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
    struct iter_ctx ctx = {.target = want_idx};

    if (seen_exhausted) return LNXRM_EFAIL;
    fat_scan_dir(m, dc, scan_iter_cb, &ctx);
    if (!ctx.hit) {
        it->cookie |= 1;
        return LNXRM_EFAIL;
    }
    it->cookie = ((want_idx + 1) << 8) | seen_exhausted;
    strncpy(d->name, ctx.name, sizeof(d->name) - 1);
    d->type = ctx.type;
    d->name[sizeof(d->name) - 1] = 0;
    return 0;
}

/* 4-arg wrapper matching fs_ops->getdent signature */
int fat32_getdent_impl(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d)
{
    (void)mnt;
    struct dir_iter it = {.node = dirnode, .cookie = *cookie};
    int r = fat_dir_iter(&it, d);
    *cookie = it.cookie;
    return r;
}
