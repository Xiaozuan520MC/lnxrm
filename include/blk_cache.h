#pragma once
#include <types.h>
#include <vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CACHE_SECTOR_SIZE 512
#define CACHE_ENTRIES 64  /* Number of cached sectors */

struct cache_entry {
    u64 lba;              /* Logical Block Address */
    u8 data[CACHE_SECTOR_SIZE];
    bool valid;           /* Is this entry valid? */
    bool dirty;           /* Does this entry need writeback? */
    bool used;           /* LRU: has this been accessed recently? */
    struct blkdev *dev;  /* Associated block device */
};

/* Initialize the cache system */
void blk_cache_init(void);

/* Read a sector through the cache */
int blk_cache_read(struct blkdev *dev, u64 lba, void *buf);

/* Write a sector through the cache */
int blk_cache_write(struct blkdev *dev, u64 lba, const void *buf);

/* Flush all dirty entries for a device */
int blk_cache_flush(struct blkdev *dev);

/* Invalidate all entries for a device */
void blk_cache_invalidate(struct blkdev *dev);

/* Get cache statistics */
void blk_cache_stats(u32 *hits, u32 *misses, u32 *writes);

#ifdef __cplusplus
}
#endif