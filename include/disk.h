/* Block layer: MBR partition parsing, sector cache and write journal.
 * Merges the old mbr.h + blk_cache.h + fat32_journal.h. */
#pragma once
#include <types.h>
#include <sys/vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- MBR partition table ---------------- */

/* MBR Partition Table Entry */
struct mbr_entry {
    u8 status;         /* 0x80 = active/bootable */
    u8 chs_first[3];   /* CHS of first sector */
    u8 type;           /* partition type */
    u8 chs_last[3];    /* CHS of last sector */
    u32 lba_first;     /* LBA of first sector */
    u32 sectors_count; /* number of sectors */
} __attribute__((packed));

/* MBR Signature */
#define MBR_SIGNATURE              0xAA55
#define MBR_PARTITION_TABLE_OFFSET 446
#define MBR_PARTITION_ENTRY_SIZE   16

/* Partition Types */
#define PART_TYPE_NONE       0x00
#define PART_TYPE_FAT12      0x01
#define PART_TYPE_FAT16_SM   0x04 /* <32MB */
#define PART_TYPE_EXTENDED   0x05
#define PART_TYPE_FAT16      0x06
#define PART_TYPE_FAT32      0x0B
#define PART_TYPE_FAT32_LBA  0x0C
#define PART_TYPE_FAT16_LBA  0x0E
#define PART_TYPE_EXT_LBA    0x0F
#define PART_TYPE_LINUX      0x83 /* ext2/ext3/ext4 */
#define PART_TYPE_LINUX_SWAP 0x82

/* Max partitions we track */
#define MBR_MAX_PARTITIONS 4

struct mbr_info {
    struct blkdev *dev;
    u32 total_sectors;
    u8 boot_ind; /* active partition index or -1 */
    struct {
        u8 type;
        u32 lba_first;
        u32 sectors_count;
        bool is_extended;
    } parts[MBR_MAX_PARTITIONS];
    int part_count;
};

/* Parse MBR from a block device. Returns 0 on success. */
int mbr_parse(struct blkdev *dev, struct mbr_info *out);

/* Get partition as a virtual block device (for reading partition contents) */
int mbr_get_partition(struct mbr_info *mbr, int index, struct blkdev *out_dev,
                      struct blkdev *parent);

/* Get string name for partition type */
const char *mbr_type_name(u8 type);

/* ---------------- sector cache ---------------- */

#define CACHE_SECTOR_SIZE 512
#define CACHE_ENTRIES     64 /* Number of cached sectors */

struct cache_entry {
    u64 lba; /* Logical Block Address */
    u8 data[CACHE_SECTOR_SIZE];
    bool valid;         /* Is this entry valid? */
    bool dirty;         /* Does this entry need writeback? */
    bool used;          /* LRU: has this been accessed recently? */
    struct blkdev *dev; /* Associated block device */
};

/* Initialize the cache system */
void blk_cache_init(void);

/* Read a sector through the cache */
int blk_cache_read(struct blkdev *dev, u64 lba, void *buf);

/* Write a sector through the cache */
int blk_cache_write(struct blkdev *dev, u64 lba, const void *buf);

/* Flush all dirty entries for a device */
int blk_cache_flush(struct blkdev *dev);

/* ---------------- FAT32 write journal ---------------- */

/* Initialize the FAT32 journal */
void fat32_journal_init(struct blkdev *dev);

/* Start a new transaction */
int fat32_journal_begin(void);

/* Add a sector to the journal */
int fat32_journal_add(u64 lba, const void *data);

/* Commit the transaction */
int fat32_journal_commit(void);

/* Abort the transaction (discard changes) */
int fat32_journal_abort(void);

#ifdef __cplusplus
}
#endif
