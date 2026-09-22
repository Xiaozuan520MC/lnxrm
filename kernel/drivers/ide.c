/* ide.c: Simple PIO-mode IDE/ATA driver for QEMU's built-in PIIX4 IDE.
 * Reads/writes sectors from the primary IDE controller (0x1F0). */
#include <vfs.h>
#include <io.h>
#include <console.h>
#include <mm.h>

#define IDE_DATA      0x1F0
#define IDE_ERROR     0x1F1
#define IDE_SECCOUNT  0x1F2
#define IDE_LBA_LO    0x1F3
#define IDE_LBA_MID   0x1F4
#define IDE_LBA_HI    0x1F5
#define IDE_DRIVE     0x1F6
#define IDE_STATUS    0x1F7
#define IDE_CMD       0x1F7

#define IDE_SR_BSY    0x80
#define IDE_SR_DRDY   0x40
#define IDE_SR_DRQ    0x08
#define IDE_SR_ERR    0x01

#define IDE_CMD_READ   0x20
#define IDE_CMD_WRITE  0x30
#define IDE_CMD_IDENTIFY 0xEC

static int ide_wait_ready(int mask, int timeout)
{
    for (int i = 0; i < timeout; i++) {
        u8 s = inb(IDE_STATUS);
        if (!(s & IDE_SR_BSY) && (s & mask))
            return 0;
    }
    return -1;
}

static int ide_wait_drq(void)
{
    for (int i = 0; i < 100000; i++) {
        u8 s = inb(IDE_STATUS);
        if (s & IDE_SR_ERR)
            return -1;
        if (s & IDE_SR_DRQ)
            return 0;
    }
    return -1;
}

static int ide_read_sector(u64 lba, void *buf)
{
    if (ide_wait_ready(IDE_SR_DRDY, 100000))
        return -1;

    outb(IDE_SECCOUNT, 1);
    outb(IDE_LBA_LO,  (u8)(lba));
    outb(IDE_LBA_MID, (u8)(lba >> 8));
    outb(IDE_LBA_HI,  (u8)(lba >> 16));
    outb(IDE_DRIVE,   0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_CMD, IDE_CMD_READ);

    if (ide_wait_drq())
        return -1;

    u16 *p = (u16 *)buf;
    for (int i = 0; i < 256; i++)
        p[i] = inw(IDE_DATA);
    return 0;
}

static int ide_write_sector(u64 lba, const void *buf)
{
    if (ide_wait_ready(IDE_SR_DRDY, 100000))
        return -1;

    outb(IDE_SECCOUNT, 1);
    outb(IDE_LBA_LO,  (u8)(lba));
    outb(IDE_LBA_MID, (u8)(lba >> 8));
    outb(IDE_LBA_HI,  (u8)(lba >> 16));
    outb(IDE_DRIVE,   0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_CMD, IDE_CMD_WRITE);

    if (ide_wait_drq())
        return -1;

    const u16 *p = (const u16 *)buf;
    for (int i = 0; i < 256; i++)
        outw(IDE_DATA, p[i]);

    /* flush */
    ide_wait_ready(IDE_SR_DRDY, 100000);
    return 0;
}

static struct blkdev ide_bd = {
    .name = "hda",
    .sector_size = 512,
    .read  = NULL,
    .write = NULL,
};

static int ide_blk_read(struct blkdev *b, u64 lba, u32 count, void *buf)
{
    u8 *p = buf;
    for (u32 i = 0; i < count; i++) {
        if (ide_read_sector(lba + i, p + i * 512))
            return -1;
    }
    return 0;
}

static int ide_blk_write(struct blkdev *b, u64 lba, u32 count, const void *buf)
{
    const u8 *p = buf;
    for (u32 i = 0; i < count; i++) {
        if (ide_write_sector(lba + i, p + i * 512))
            return -1;
    }
    return 0;
}

void ide_init(void)
{
    /* probe: send IDENTIFY to primary channel */
    outb(IDE_DRIVE, 0xA0);
    outb(IDE_CMD, IDE_CMD_IDENTIFY);

    u8 s = inb(IDE_STATUS);
    if (s == 0) {
        kprintf("[ide] no device on primary channel\n");
        return;
    }

    if (ide_wait_ready(IDE_SR_DRDY, 100000)) {
        kprintf("[ide] primary channel not ready\n");
        return;
    }

    u16 ident[256];
    for (int i = 0; i < 256; i++)
        ident[i] = inw(IDE_DATA);

    u64 sectors = *(u32 *)&ident[60];  /* total addressable sectors (LBA28) */
    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)(ident[27 + i] >> 8);
        model[i * 2 + 1] = (char)(ident[27 + i] & 0xFF);
    }
    model[40] = 0;

    ide_bd.read  = ide_blk_read;
    ide_bd.write = ide_blk_write;
    ide_bd.num_sectors = sectors;
    blk_register(&ide_bd);

    kprintf("[ide] hda: %.40s, %llu sectors (%llu MiB)\n",
            model, sectors, sectors / 2048);
}
