/* FAT32 transaction support: provides atomic operations for FAT32 updates.
 * Uses a simple journaling mechanism to ensure consistency. */
#include <sys/vfs.h>
#include <console.h>
#include <mm/mm.h>
#include <disk.h>

#define MAX_JOURNAL_ENTRIES 32
#define JOURNAL_MAGIC       0x4A4F5552 /* "JOUR" */

struct journal_entry {
    u64 lba;      /* Logical Block Address */
    u8 data[512]; /* Sector data */
    bool valid;   /* Is this entry valid? */
};

struct journal {
    u32 magic; /* Magic number for validation */
    struct journal_entry entries[MAX_JOURNAL_ENTRIES];
    u32 count;          /* Number of valid entries */
    bool active;        /* Is a transaction active? */
    struct blkdev *dev; /* Associated block device */
};

static struct journal global_journal;

/* Forward declarations */
int fat32_journal_abort(void);

/* Initialize the journal */
void fat32_journal_init(struct blkdev *dev)
{
    global_journal.magic = JOURNAL_MAGIC;
    global_journal.count = 0;
    global_journal.active = false;
    global_journal.dev = dev;
    kprintf("[fat32_journal] initialized for device %s\n", dev->name);
}

/* Start a new transaction */
int fat32_journal_begin(void)
{
    if (global_journal.active) return LNXRM_EFAIL; /* Transaction already active */

    global_journal.count = 0;
    global_journal.active = true;
    return 0;
}

/* Add a sector to the journal */
int fat32_journal_add(u64 lba, const void *data)
{
    if (!global_journal.active) return LNXRM_EFAIL; /* No active transaction */

    if (global_journal.count >= MAX_JOURNAL_ENTRIES) return LNXRM_ENOENT; /* Journal full */

    struct journal_entry *entry = &global_journal.entries[global_journal.count];
    entry->lba = lba;
    memcpy(entry->data, data, 512);
    entry->valid = true;
    global_journal.count++;

    return 0;
}

/* Commit the transaction */
int fat32_journal_commit(void)
{
    if (!global_journal.active) return LNXRM_EFAIL; /* No active transaction */

    struct blkdev *dev = global_journal.dev;

    /* Write all journaled sectors to disk */
    for (u32 i = 0; i < global_journal.count; i++) {
        struct journal_entry *entry = &global_journal.entries[i];
        if (entry->valid) {
            int ret = dev->write(dev, entry->lba, 1, entry->data);
            if (ret != 0) {
                kprintf("\033[1;31m[fat32_journal] write failed at LBA %llu\033[0m\n", entry->lba);
                fat32_journal_abort();
                return ret;
            }
        }
    }

    /* Mark transaction as complete */
    global_journal.active = false;
    global_journal.count = 0;

    /* Flush cache to ensure data reaches disk */
    blk_cache_flush(dev);

    return 0;
}

/* Abort the transaction (discard changes) */
int fat32_journal_abort(void)
{
    global_journal.active = false;
    global_journal.count = 0;
    return 0;
}