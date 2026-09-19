#pragma once
#include <types.h>
#include <vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* Check if journal is active */
bool fat32_journal_is_active(void);

/* Get journal statistics */
void fat32_journal_stats(u32 *entries, bool *active);

#ifdef __cplusplus
}
#endif