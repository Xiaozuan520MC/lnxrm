#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ext2 Superblock (at offset 1024 from start) */
struct ext2_superblock {
    u32 inodes_count;
    u32 blocks_count;
    u32 r_blocks_count;
    u32 free_blocks_count;
    u32 free_inodes_count;
    u32 first_data_block;
    u32 log_block_size;
    u32 log_frag_size;
    u32 blocks_per_group;
    u32 frags_per_group;
    u32 inodes_per_group;
    u32 mtime;
    u32 wtime;
    u16 mnt_count;
    u16 max_mnt_count;
    u16 magic;
    u16 state;
    u16 errors;
    u16 minor_rev_level;
    u32 lastcheck;
    u32 checkinterval;
    u32 creator_os;
    u32 rev_level;
    u16 def_resuid;
    u16 def_resgid;
    /* EXT2_DYNAMIC_REV fields */
    u32 first_ino;
    u16 inode_size;
    u16 block_group_nr;
    u32 feature_compat;
    u32 feature_incompat;
    u32 feature_ro_compat;
    u8  uuid[16];
    u8  volume_name[16];
    u8  last_mounted[64];
    u32 algo_bitmap;
} __attribute__((packed));

/* ext2 Block Group Descriptor */
struct ext2_bgd {
    u32 block_bitmap;
    u32 inode_bitmap;
    u32 inode_table;
    u16 free_blocks_count;
    u16 free_inodes_count;
    u16 used_dirs_count;
    u16 pad;
    u8  reserved[12];
} __attribute__((packed));

/* ext2 Inode */
struct ext2_inode {
    u16 mode;
    u16 uid;
    u32 size;
    u32 atime;
    u32 ctime;
    u32 mtime;
    u32 dtime;
    u16 gid;
    u16 links_count;
    u32 blocks;
    u32 flags;
    u32 osd1;
    u32 block[15];   /* 0-11 direct, 12 single, 13 double, 14 triple */
    u32 generation;
    u32 file_acl;
    u32 dir_acl;
    u32 faddr;
    u8  osd2[12];
} __attribute__((packed));

/* ext2 Directory Entry */
struct ext2_dirent {
    u32 inode;
    u16 rec_len;
    u8  name_len;
    u8  file_type;
    char name[256];  /* variable length */
} __attribute__((packed));

/* File types */
#define EXT2_FT_UNKNOWN     0
#define EXT2_FT_REG_FILE    1
#define EXT2_FT_DIR         2
#define EXT2_FT_CHRDEV      3
#define EXT2_FT_BLKDEV      4
#define EXT2_FT_FIFO        5
#define EXT2_FT_SOCK        6
#define EXT2_FT_SYMLINK     7

/* Inode modes */
#define EXT2_S_IFSOCK   0xC000
#define EXT2_S_IFLNK    0xA000
#define EXT2_S_IFREG    0x8000
#define EXT2_S_IFBLK    0x6000
#define EXT2_S_IFDIR    0x4000
#define EXT2_S_IFCHR    0x2000
#define EXT2_S_IFIFO    0x1000
#define EXT2_S_IFMT     0xF000

/* ext2 Magic */
#define EXT2_SUPER_MAGIC  0xEF53

/* Mount info */
struct ext2_mount {
    struct blkdev *dev;
    struct ext2_superblock sb;
    struct ext2_bgd *bgd;
    u32 bgd_count;
    u32 block_size;
    u32 inodes_per_block;
    u32 inode_size;
    u32 blocks_per_group;
    u32 group_count;
    u32 inode_table_blocks;
};

/* Mount and initialize ext2 filesystem */
int ext2_mount(struct blkdev *dev);

/* Global mount private data */
extern struct ext2_mount *ext2_priv;

/* Get the fs_ops for ext2 */
extern struct fs_ops ext2_ops;

#ifdef __cplusplus
}
#endif
