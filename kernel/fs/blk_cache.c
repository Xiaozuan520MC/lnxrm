/* Block cache: LRU sector cache for improving disk I/O performance. */
#include <disk.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/spinlock.h>

static struct cache_entry cache[CACHE_ENTRIES];
static spinlock_t cache_lock = SPINLOCK_INIT;

void blk_cache_init(void)
{
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        cache[i].valid = false;
        cache[i].dirty = false;
        cache[i].used = false;
        cache[i].dev = NULL;
        cache[i].lba = 0;
    }
    kprintf("[blk_cache] initialized %d entries\n", CACHE_ENTRIES);
}

/* Find a cache entry for a given device and LBA.
 * Returns the index if found, -1 otherwise. */
static int cache_find(struct blkdev *dev, u64 lba)
{
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dev == dev && cache[i].lba == lba) {
            cache[i].used = true;
            return i;
        }
    }
    return LNXRM_EFAIL;
}

/* Find a victim entry for eviction using simple LRU.
 * Prefer invalid entries first, then unused entries. */
static int cache_find_victim(void)
{
    /* First pass: look for invalid entries */
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].valid) return i;
    }

    /* Second pass: look for unused entries */
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].used) return i;
    }

    /* Third pass: clear all used flags and try again */
    for (int i = 0; i < CACHE_ENTRIES; i++) { cache[i].used = false; }

    /* Try again after clearing flags */
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].used) return i;
    }

    /* Should not reach here, but fallback to first entry */
    return 0;
}

/* Writeback a dirty cache entry */
static int cache_writeback(int idx)
{
    if (!cache[idx].valid || !cache[idx].dirty) return 0;

    struct blkdev *dev = cache[idx].dev;
    u64 lba = cache[idx].lba;
    int ret = dev->write(dev, lba, 1, cache[idx].data);
    if (ret == 0) cache[idx].dirty = false;
    return ret;
}

/* all helpers below assume cache_lock is held */
static int cache_flush_locked(struct blkdev *dev)
{
    int ret = 0;
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dirty && cache[i].dev == dev) {
            int r = cache_writeback(i);
            if (r != 0) ret = r;
        }
    }
    return ret;
}

int blk_cache_read(struct blkdev *dev, u64 lba, void *buf)
{
    /* Validate buffer pointer */
    if (!buf) return LNXRM_EFAIL;

    u64 flags;
    spin_lock_irqsave(&cache_lock, &flags);

    int idx = cache_find(dev, lba);

    if (idx >= 0) {
        /* Cache hit */
        memcpy(buf, cache[idx].data, CACHE_SECTOR_SIZE);
        spin_unlock_irqrestore(&cache_lock, flags);
        return 0;
    }

    /* Cache miss: need to read from disk */
    idx = cache_find_victim();

    /* Writeback if victim is dirty */
    if (cache[idx].valid && cache[idx].dirty) {
        int ret = cache_writeback(idx);
        if (ret != 0) {
            spin_unlock_irqrestore(&cache_lock, flags);
            return ret;
        }
    }

    /* Read from disk into cache entry */
    int ret = dev->read(dev, lba, 1, cache[idx].data);
    if (ret != 0) {
        spin_unlock_irqrestore(&cache_lock, flags);
        return ret;
    }

    /* Update cache entry */
    cache[idx].lba = lba;
    cache[idx].dev = dev;
    cache[idx].valid = true;
    cache[idx].dirty = false;
    cache[idx].used = true;

    /* Copy to user buffer */
    memcpy(buf, cache[idx].data, CACHE_SECTOR_SIZE);
    spin_unlock_irqrestore(&cache_lock, flags);
    return 0;
}

int blk_cache_write(struct blkdev *dev, u64 lba, const void *buf)
{
    /* Validate buffer pointer */
    if (!buf) return LNXRM_EFAIL;

    u64 flags;
    spin_lock_irqsave(&cache_lock, &flags);

    int idx = cache_find(dev, lba);

    if (idx < 0) {
        /* Cache miss: need to allocate new entry */
        idx = cache_find_victim();

        /* Writeback if victim is dirty */
        if (cache[idx].valid && cache[idx].dirty) {
            int ret = cache_writeback(idx);
            if (ret != 0) {
                spin_unlock_irqrestore(&cache_lock, flags);
                return ret;
            }
        }

        /* Initialize new cache entry */
        cache[idx].lba = lba;
        cache[idx].dev = dev;
        cache[idx].valid = true;
        cache[idx].used = true;
    }

    /* Update cache with new data */
    memcpy(cache[idx].data, buf, CACHE_SECTOR_SIZE);
    cache[idx].dirty = true;
    spin_unlock_irqrestore(&cache_lock, flags);
    return 0;
}

int blk_cache_flush(struct blkdev *dev)
{
    u64 flags;
    spin_lock_irqsave(&cache_lock, &flags);
    int ret = cache_flush_locked(dev);
    spin_unlock_irqrestore(&cache_lock, flags);
    return ret;
}
