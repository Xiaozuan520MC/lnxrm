/* Block cache: LRU sector cache for improving disk I/O performance. */
#include <blk_cache.h>
#include <console.h>
#include <mm.h>

static struct cache_entry cache[CACHE_ENTRIES];
static u32 cache_hits = 0;
static u32 cache_misses = 0;
static u32 cache_writes = 0;

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
    return -1;
}

/* Find a victim entry for eviction using simple LRU.
 * Prefer invalid entries first, then unused entries. */
static int cache_find_victim(void)
{
    /* First pass: look for invalid entries */
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].valid)
            return i;
    }
    
    /* Second pass: look for unused entries */
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].used)
            return i;
    }
    
    /* Third pass: clear all used flags and try again */
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        cache[i].used = false;
    }
    
    /* Try again after clearing flags */
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].used)
            return i;
    }
    
    /* Should not reach here, but fallback to first entry */
    return 0;
}

/* Writeback a dirty cache entry */
static int cache_writeback(int idx)
{
    if (!cache[idx].valid || !cache[idx].dirty)
        return 0;
    
    struct blkdev *dev = cache[idx].dev;
    u64 lba = cache[idx].lba;
    int ret = dev->write(dev, lba, 1, cache[idx].data);
    if (ret == 0) {
        cache[idx].dirty = false;
        cache_writes++;
    }
    return ret;
}

int blk_cache_read(struct blkdev *dev, u64 lba, void *buf)
{
    /* Validate buffer pointer */
    if (!buf)
        return -1;
    
    int idx = cache_find(dev, lba);
    
    if (idx >= 0) {
        /* Cache hit */
        cache_hits++;
        memcpy(buf, cache[idx].data, CACHE_SECTOR_SIZE);
        return 0;
    }
    
    /* Cache miss: need to read from disk */
    cache_misses++;
    idx = cache_find_victim();
    
    /* Writeback if victim is dirty */
    if (cache[idx].valid && cache[idx].dirty) {
        int ret = cache_writeback(idx);
        if (ret != 0)
            return ret;
    }
    
    /* Read from disk into cache entry */
    int ret = dev->read(dev, lba, 1, cache[idx].data);
    if (ret != 0)
        return ret;
    
    /* Update cache entry */
    cache[idx].lba = lba;
    cache[idx].dev = dev;
    cache[idx].valid = true;
    cache[idx].dirty = false;
    cache[idx].used = true;
    
    /* Copy to user buffer */
    memcpy(buf, cache[idx].data, CACHE_SECTOR_SIZE);
    return 0;
}

int blk_cache_write(struct blkdev *dev, u64 lba, const void *buf)
{
    /* Validate buffer pointer */
    if (!buf)
        return -1;
    
    int idx = cache_find(dev, lba);
    
    if (idx < 0) {
        /* Cache miss: need to allocate new entry */
        idx = cache_find_victim();
        
        /* Writeback if victim is dirty */
        if (cache[idx].valid && cache[idx].dirty) {
            int ret = cache_writeback(idx);
            if (ret != 0)
                return ret;
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
    return 0;
}

int blk_cache_flush(struct blkdev *dev)
{
    int ret = 0;
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dirty && cache[i].dev == dev) {
            int r = cache_writeback(i);
            if (r != 0)
                ret = r;
        }
    }
    return ret;
}

void blk_cache_invalidate(struct blkdev *dev)
{
    /* First flush all dirty entries */
    blk_cache_flush(dev);
    
    /* Then invalidate all entries for this device */
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dev == dev) {
            cache[i].valid = false;
            cache[i].dirty = false;
            cache[i].used = false;
        }
    }
}

void blk_cache_stats(u32 *hits, u32 *misses, u32 *writes)
{
    if (hits) *hits = cache_hits;
    if (misses) *misses = cache_misses;
    if (writes) *writes = cache_writes;
}