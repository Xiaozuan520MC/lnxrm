/* ramfs: in-memory filesystem seeded from the embedded cpio initramfs. */
#include <vfs.h>
#include <console.h>
#include <mm.h>

struct rnode {
    char name[56];
    int type;
    struct rnode *child, *sibling, *parent;
    u8 *data;
    u64 size, cap;
};

static struct rnode root_node;

void ramfs_init(void)
{
    memset(&root_node, 0, sizeof(root_node));
    strcpy(root_node.name, "");
    root_node.type = V_DIR;
}

static int ramfs_lookup(void *mnt, const char *path, struct vnode *out);
static int ramfs_read(void *mnt, void *node, u64 off, void *buf, size_t n);
static int ramfs_write(void *mnt, void *node, u64 off, const void *buf, size_t n);
static int ramfs_create(void *mnt, const char *path);

struct fs_ops ramfs_ops = {
    .lookup = ramfs_lookup,
    .getdent = NULL,
    .read = ramfs_read,
    .write = ramfs_write,
    .create = ramfs_create,
    .mkdir = NULL,
    .unlink = NULL,
};

int ramfs_getdent_raw(struct dir_iter *it, struct dirent_out *d);

static struct rnode *find_child(struct rnode *dir, const char *name)
{
    for (struct rnode *c = dir->child; c; c = c->sibling)
        if (!strcmp(c->name, name))
            return c;
    return NULL;
}

static int lookup_internal(const char *path, struct rnode **out)
{
    if (*path == '/')
        path++;
    if (!*path) {
        *out = &root_node;
        return 0;
    }
    char comp[56];
    struct rnode *cur = &root_node;
    while (*path) {
        const char *sl = strchr(path, '/');
        size_t len = sl ? (size_t)(sl - path) : strlen(path);
        if (len >= sizeof(comp))
            return -1;
        memcpy(comp, path, len);
        comp[len] = 0;
        cur = find_child(cur, comp);
        if (!cur)
            return -1;
        path = sl ? sl + 1 : path + len;
        while (*path == '/')
            path++;
    }
    *out = cur;
    return 0;
}

static void node_grow(struct rnode *n, u64 need)
{
    if (need <= n->cap)
        return;
    u64 cap = n->cap ? n->cap : 512;
    while (cap < need)
        cap *= 2;
    u8 *nd = kmalloc(cap);
    if (n->data) {
        memcpy(nd, n->data, n->size);
        kfree(n->data);
    }
    n->data = nd;
    n->cap = cap;
}

static int ramfs_lookup(void *mnt, const char *path, struct vnode *out)
{
    struct rnode *n;
    if (lookup_internal(path, &n) < 0)
        return -1;
    out->type = n->type;
    out->size = n->size;
    out->fs_data = n;
    out->ops = &ramfs_ops;
    return 0;
}

static int ramfs_read(void *mnt, void *node, u64 off, void *buf, size_t n)
{
    struct rnode *r = node;
    if (off >= r->size)
        return 0;
    u64 avail = r->size - off;
    if (n > avail)
        n = avail;
    memcpy(buf, r->data + off, n);
    return n;
}

static int ramfs_write(void *mnt, void *node, u64 off, const void *buf, size_t n)
{
    struct rnode *r = node;
    if (!buf && !n) {               /* truncate */
        r->size = 0;
        return 0;
    }
    node_grow(r, off + n);
    memcpy(r->data + off, buf, n);
    if (off + n > r->size)
        r->size = off + n;
    return n;
}

static int ramfs_mkdirp(const char *path);

static int ramfs_create(void *mnt, const char *path)
{
    /* split into parent dir + name */
    char full[128], parent[128], name[56];
    strncpy(full, path, sizeof(full) - 1);
    char *slash = strrchr(full, '/');
    const char *base = slash ? slash + 1 : full;
    strncpy(name, base, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    parent[0] = 0;
    if (slash && slash != full) {
        size_t pl = slash - full;
        memcpy(parent, full, pl);
        parent[pl] = 0;
    }

    struct rnode *dir = &root_node;
    if (parent[0]) {
        if (lookup_internal(parent, &dir) < 0)
            return -1;
    }
    if (dir->type != V_DIR || find_child(dir, name))
        return -1;

    struct rnode *n = kmalloc(sizeof(*n));
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->type = V_REG;
    n->parent = dir;
    n->sibling = dir->child;
    dir->child = n;
    return 0;
}

int ramfs_getdent_raw(struct dir_iter *it, struct dirent_out *d)
{
    struct rnode *dir = it->node;
    u64 idx = it->cookie;
    if (idx >> 48)
        return -1;                  /* exhausted */
    struct rnode *c = dir->child;
    for (u64 i = 0; c && i < idx; i++)
        c = c->sibling;
    if (!c) {
        it->cookie |= 1ULL << 48;
        return -1;
    }
    it->cookie++;
    strncpy(d->name, c->name, sizeof(d->name) - 1);
    d->name[sizeof(d->name) - 1] = 0;
    d->type = c->type == V_DIR ? 4 : c->type == V_CHR ? 3 : 8;
    return 0;
}


/* ---------------- cpio (newc) extraction ---------------- */
static inline u32 hex4(const char *p)
{
    u32 v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= c - '0';
        else if (c >= 'a' && c <= 'f')
            v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v |= c - 'A' + 10;
    }
    return v;
}

static struct rnode *ensure_dir(const char *comp_path)
{
    struct rnode *n;
    if (!lookup_internal(comp_path, &n)) {
        if (n->type == V_DIR)
            return n;
        return NULL;
    }
    /* create intermediate directory */
    char full[128], parent[128];
    strncpy(full, comp_path, sizeof(full) - 1);
    full[127] = 0;
    char *slash = strrchr(full, '/');
    const char *base = slash ? slash + 1 : full;
    char name[56];
    strncpy(name, base, sizeof(name) - 1);
    parent[0] = 0;
    if (slash && slash != full) {
        size_t pl = slash - full;
        memcpy(parent, full, pl);
        parent[pl] = 0;
    }
    struct rnode *dir = &root_node;
    if (parent[0] && !(lookup_internal(parent, &dir) == 0 && dir->type == V_DIR))
        return NULL;
    struct rnode *nd = kmalloc(sizeof(*nd));
    memset(nd, 0, sizeof(*nd));
    strncpy(nd->name, name, sizeof(nd->name) - 1);
    nd->type = V_DIR;
    nd->parent = dir;
    nd->sibling = dir->child;
    dir->child = nd;
    return nd;
}

void ramfs_add_from_cpio(const void *cpio, size_t len)
{
    const u8 *p = cpio, *end = p + len;
    int added = 0;
    while (p + 110 <= end) {
        if (memcmp(p, "070701", 6))
            break;
        u32 namesize = hex4((const char *)p + 94);
        u32 filesize = hex4((const char *)p + 54);
        u32 mode = hex4((const char *)p + 14);
        const char *name = (const char *)p + 110;
        if (!strcmp(name, "TRAILER!!!"))
            break;
        u64 data_off = 110 + namesize;
        data_off = (data_off + 3) & ~3ULL;
        const u8 *data = p + data_off;

        bool is_dir = (mode & 0170000) == 0040000;
        bool is_chr = (mode & 0170000) == 0020000;
        char path[128];
        strncpy(path, name, sizeof(path) - 1);
        path[127] = 0;
        /* strip trailing slash from dir entries */
        size_t pl = strlen(path);
        while (pl && path[pl - 1] == '/')
            path[--pl] = 0;

        struct rnode *n;
        if (is_dir) {
            n = ensure_dir(path);
        } else {
            /* make sure parents exist */
            char parent[128] = "";
            char *slash = strrchr(path, '/');
            if (slash && slash != path) {
                size_t l = slash - path;
                memcpy(parent, path, l);
                parent[l] = 0;
                ensure_dir(parent);
            }
            n = NULL;
            ramfs_create(NULL, path);
            lookup_internal(path, &n);
            if (n) {
                node_grow(n, filesize);
                memcpy(n->data, data, filesize);
                n->size = filesize;
                if (is_chr)
                    n->type = V_CHR;
            }
        }
        u64 adv = data_off + filesize;
        adv = (adv + 3) & ~3ULL;
        p += adv;
        added++;
    }
    kprintf("[cpio] extracted %d entries\n", added);
}
