/* AHCI (SATA) host adapter driver: PCI probe, port reset, DMA transfers.
 * Provides the first block device used by the FAT32 driver. */
#include <ahci.h>
#include <io.h>
#include <mm.h>
#include <console.h>
#include <vfs.h>
#include <pci.h>
#include <blk_cache.h>

#define AHCI_CAP     0x00
#define AHCI_GHC     0x04
#define GHC_ARE      (1u << 31)
#define GHC_IE       (1u << 1)
#define GHC_HR       1
#define AHCI_PI      0x0C

#define PX_CLB       0x100          /* per-port regs base */
#define PXFB         0x08
#define PXIS         0x13
#define PXSERR       0x130
#define PXCI         0x138
#define PXTFD        0x120
#define PXCMD        0x118
#define CMD_ST       1
#define CMD_FRE      (1u << 4)
#define CMD_SUD      (1u << 1)
#define CMD_POD      (1u << 2)
#define IS_TFES      (1u << 30)

struct hba_port {
    u64 _rsv[8];
} __attribute__((packed));

/* register offsets within a port (bytes) */
enum {
    P_CLB = 0x00, P_CLBU = 0x04, P_FB = 0x08, P_FBU = 0x0C, P_IS = 0x10,
    P_IE = 0x14, P_CMD = 0x18, P_TFD = 0x20, P_SIG = 0x24, P_SSTS = 0x28,
    P_SCTL = 0x2C, P_CI = 0x38,
};

struct cmd_hdr {
    u16 opts;
    u16 prdtlen;
    u32 prdbc;
    u64 ctba;
    u32 _rsv[4];
} __attribute__((packed));

struct prdt_entry {
    u32 dba;
    u32 dbahi;
    u32 _rsv;
    u32 flags;                      /* bit31 = interrupt on completion */
};
#define PRDT_IOC (1u << 31)

struct cmd_tbl {
    u8 cfis[64];
    u8 acmd[16];
    u8 _rsv[48];
    struct prdt_entry prdt[];
};

/* FIS host-to-device */
static void fis_h2d(u8 *cfis, u8 cmd, u64 lba, u16 count, bool lba48)
{
    memset(cfis, 0, 64);
    cfis[0] = 0x27;                 /* type */
    cfis[1] = 0x80;                 /* command bit */
    cfis[2] = cmd;
    if (!lba48) {
        cfis[4] = (u8)lba;
        cfis[5] = (u8)(lba >> 8);
        cfis[6] = (u8)(lba >> 16);
        cfis[7] = 0xE0 | ((lba >> 24) & 0x0F);
    } else {
        cfis[4] = (u8)lba;
        cfis[5] = (u8)(lba >> 8);
        cfis[6] = (u8)(lba >> 16);
        cfis[7] = 0x40;             /* DEVICE.LBA must be set for EXT cmds */
        cfis[8] = (u8)(lba >> 24);
        cfis[9] = (u8)(lba >> 32);
        cfis[10] = (u8)(lba >> 40);
    }
    if (!lba48)
        count &= 0xFF;
    cfis[12] = count & 0xFF;
    cfis[13] = (count >> 8) & 0xFF;
}

struct ahci_disk {
    volatile u8 *abar;
    int portno;
    volatile u8 *px;
    struct cmd_hdr *clb;            /* 32 headers, page aligned */
    void *fb;
    struct cmd_tbl **ctbl;          /* one table per slot */
    struct blkdev bd;
    u32 slot_busy;
};

static struct ahci_disk *g_disk;

static inline u32 px_read(volatile u8 *px, int off)
{
    return *(volatile u32 *)(px + off);
}

static inline void px_write(volatile u8 *px, int off, u32 v)
{
    *(volatile u32 *)(px + off) = v;
}

static int wait_clear(volatile u32 *reg, u32 mask, int ms)
{
    for (int i = 0; i < ms * 100; i++) {
        if (!(*(volatile u32 *)reg & mask))
            return 0;
        for (int j = 0; j < 100; j++)
            __asm__ volatile("pause");
    }
    return -1;
}

static int port_start(volatile u8 *px)
{
    u32 cmd = px_read(px, P_CMD);
    px_write(px, P_CMD, cmd | CMD_SUD | CMD_POD);
    if (wait_clear((volatile u32 *)(px + P_CMD), CMD_FRE | CMD_ST, 500))
        kprintf("\033[1;33m[ahci] warning: FRE/ST stuck\033[0m\n");
    px_write(px, P_IS, ~0u);        /* clear pending */
    cmd = px_read(px, P_CMD);
    px_write(px, P_CMD, cmd | CMD_ST | CMD_FRE);
    return wait_clear((volatile u32 *)(px + P_CMD), 0, 10);
}

static int port_stop(volatile u8 *px)
{
    u32 cmd = px_read(px, P_CMD);
    cmd &= ~(CMD_ST | CMD_FRE);
    px_write(px, P_CMD, cmd);
    return wait_clear((volatile u32 *)(px + P_CMD), CMD_FRE | CMD_ST, 500);
}

#define MAX_SLOTS 32  /* Maximum number of command slots */

/* Find a free command slot */
static int find_free_slot(struct ahci_disk *d)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!(d->slot_busy & (1u << i)))
            return i;
    }
    return -1;  /* No free slots */
}

/* Mark a slot as busy or free */
static void set_slot_busy(struct ahci_disk *d, int slot, bool busy)
{
    if (busy)
        d->slot_busy |= (1u << slot);
    else
        d->slot_busy &= ~(1u << slot);
}

static int issue(struct ahci_disk *d, u8 cmd, u64 lba, u16 count, void *buf,
                 size_t len, bool write)
{
    /* Validate buffer and length */
    if (!buf || len == 0)
        return -1;
    
    /* Limit transfer size to prevent overflow (max 4MiB per PRDT entry) */
    if (len > 4 * 1024 * 1024)
        return -1;
    
    /* Validate LBA and count */
    if (lba > 0x00FFFFFFFFFFFFFFULL)  /* 48-bit LBA max */
        return -1;
    if (count == 0 || count > 65535)
        return -1;

    /* Find a free command slot */
    int slot = find_free_slot(d);
    if (slot < 0) {
        kprintf("[ahci] no free command slots\n");
        return -1;
    }
    
    /* Mark slot as busy */
    set_slot_busy(d, slot, true);

    /* one PRDT entry supports up to 4 MiB; our transfers are <= 128 KiB */
    struct cmd_hdr *h = &d->clb[slot];
    struct cmd_tbl *t = d->ctbl[slot];

    h->opts = (5u << 0)             /* FIS length in dwords */
            | (write ? (1u << 6) : 0)   /* W: 1 = host->device (write) */
            | (1u << 16);           /* clear busy upon R_OK */
    h->prdtlen = 1;
    h->prdbc = 0;

    u64 bpa = vmm_translate((uptr)buf);
    if (!bpa) {
        set_slot_busy(d, slot, false);
        return -1;
    }
    t->prdt[0].dba = (u32)bpa;
    t->prdt[0].dbahi = (u32)(bpa >> 32);
    t->prdt[0]._rsv = 0;
    t->prdt[0].flags = (len - 1) | PRDT_IOC;

    fis_h2d(t->cfis, cmd, lba, count, true);

    /* wait until the port is idle before arming a new command */
    for (int i = 0; i < 1000000; i++) {
        if (!(px_read(d->px, P_TFD) & 0x88))   /* BSY | DRQ */
            break;
        __asm__ volatile("pause");
    }
    px_write(d->px, P_IS, ~0u);
    px_write(d->px, 0x30, ~0u);                /* PxSERR: clear diagnostics */
    px_write(d->px, P_CI, 1u << slot);

    int err = -1;
    for (int spin = 0; spin < 8000000; spin++) {
        u32 is = px_read(d->px, P_IS);
        if (is & IS_TFES)
            break;
        if (!(px_read(d->px, P_CI) & (1u << slot))) {
            err = 0;
            break;
        }
        __asm__ volatile("pause");
    }

    u32 tfd = px_read(d->px, P_TFD);
    if (err || (tfd & 1) || (px_read(d->px, P_IS) & IS_TFES)) {
        kprintf("\033[1;31m[ahci] cmd=%#x failed tfd=%02x is=%08x -- recovering\033[0m\n",
                cmd, tfd & 0xFF, px_read(d->px, P_IS));
        px_write(d->px, 0x30, ~0u);            /* unfreeze the port */
        port_stop(d->px);
        port_start(d->px);
        set_slot_busy(d, slot, false);
        return -1;
    }
    
    /* Mark slot as free after successful completion */
    set_slot_busy(d, slot, false);
    return 0;
}

static int ahci_rw(struct blkdev *bd, u64 lba, u32 cnt, void *buf, bool wr)
{
    struct ahci_disk *d = bd->drv;
    /* contiguous buffer => single DMA region */
    return issue(d, wr ? 0x35 : 0x25, lba, cnt, buf, cnt * bd->sector_size, wr);
}

static int ahci_rd(struct blkdev *bd, u64 lba, u32 cnt, void *buf)
{
    return ahci_rw(bd, lba, cnt, buf, false);
}

static int ahci_wr(struct blkdev *bd, u64 lba, u32 cnt, const void *buf)
{
    int ret = ahci_rw(bd, lba, cnt, (void *)buf, true);
    if (ret != 0)
        return ret;

    /* Write barrier: issue a dummy read to ensure data is flushed to disk.
     * This prevents the issue where reading back immediately after writing
     * returns stale data due to insufficient command queuing delays. */
    u8 dummy[512];
    ret = ahci_rw(bd, lba, 1, dummy, false);
    return ret;
}

extern long console_write(struct file *, const void *, size_t);
extern void fixmap_map(u64 pa);
#define DEV_PT 0x56000UL           /* PT_DEV from entry64.S */

/* map ABAR physical page at DEV_VMA through the static PT_DEV window */
static void map_abar(u64 pa)
{
    volatile u64 *ptdev = (volatile u64 *)PHYS_TO_VIRT(DEV_PT);
    ptdev[0] = pa | PG_P | PG_W;
    __asm__ volatile("invlpg (%0)" ::"r"(DEV_VMA) : "memory");
}

int ahci_probe_fn(void *ctx, u8 bus, u8 dev, u8 fn, u16 vendor,
                         u16 devid, u8 class_)
{
    (void)ctx;
    if (class_ != 1)                /* mass storage */
        return 0;
    u32 sub = pci_read(bus, dev, fn, 0x08);
    u8 subcl = (sub >> 16) & 0xFF;
    if (subcl != 6)                 /* not SATA/AHCI: skip IDE etc. */
        return 0;

    pci_enable_bm(bus, dev, fn);
    u32 bar5lo = pci_read(bus, dev, fn, 0x24);
    u32 bar5hi = pci_read(bus, dev, fn, 0x28);
    u64 abar_pa = (bar5lo & ~0xFUL) | ((u64)bar5hi << 32);

    map_abar(abar_pa & ~4095UL);
    volatile u8 *abar = (volatile u8 *)(DEV_VMA + (abar_pa & 4095UL));

    kprintf("[ahci] sata controller %04x:%04x at %02x:%02x.%d abar=%#llx\n",
            vendor, devid, bus, dev, fn, abar_pa);

    u32 ghc = *(volatile u32 *)(abar + AHCI_GHC);
    *(volatile u32 *)(abar + AHCI_GHC) = ghc & ~GHC_IE;  /* no MSI yet */
    ghc = *(volatile u32 *)(abar + AHCI_GHC);
    *(volatile u32 *)(abar + AHCI_GHC) = GHC_ARE | (ghc & GHC_IE ? GHC_IE : 0);

    u32 pi = *(volatile u32 *)(abar + AHCI_PI);
    for (int p = 0; p < 32; p++) {
        if (!((pi >> p) & 1))
            continue;
        volatile u8 *px = abar + 0x100 + p * 0x80;
        u32 ssts = px_read(px, P_SSTS);
        if ((ssts & 0xF) != 3)      /* device not present */
            continue;
        u32 ipm = (ssts >> 8) & 0xF;
        if (!ipm)
            continue;
        u32 sig = px_read(px, P_SIG);
        if (sig != 0x00000101)      /* SATA */
            continue;

        /* bring the port up */
        port_stop(px);
        px_write(px, P_SCTL, 1);    /* COMRESET */
        for (int i = 0; i < 20000; i++)
            __asm__ volatile("pause");
        px_write(px, P_SCTL, 3);    /* spin-up, no reset */

        struct ahci_disk *d = kmalloc(sizeof(*d));
        memset(d, 0, sizeof(*d));
        d->abar = abar;
        d->portno = p;
        d->px = px;
        d->clb = kmalloc(PAGE_SIZE);        /* 32 hdrs fit easily */
        memset(d->clb, 0, PAGE_SIZE);
        d->fb = kmalloc(PAGE_SIZE);
        memset(d->fb, 0, PAGE_SIZE);
        
        /* Allocate command tables for all slots */
        d->ctbl = kmalloc(MAX_SLOTS * sizeof(void *));
        for (int i = 0; i < MAX_SLOTS; i++) {
            d->ctbl[i] = kmalloc(PAGE_SIZE);
            memset(d->ctbl[i], 0, PAGE_SIZE);
        }

        u64 clb_pa = VIRT_TO_PHYS((uptr)d->clb);
        u64 fb_pa = VIRT_TO_PHYS((uptr)d->fb);
        px_write(px, P_CLB, (u32)clb_pa);
        px_write(px, P_CLBU, (u32)(clb_pa >> 32));
        px_write(px, P_FB, (u32)fb_pa);
        px_write(px, P_FBU, (u32)(fb_pa >> 32));
        
        /* Initialize command headers for all slots */
        for (int i = 0; i < MAX_SLOTS; i++) {
            u64 ct_pa = VIRT_TO_PHYS((uptr)d->ctbl[i]);
            d->clb[i].ctba = ct_pa;
            d->clb[i].prdtlen = 1;
        }

        port_start(px);

        /* IDENTIFY DEVICE via PIO-in-DMA */
        u16 ident[256];
        memset(ident, 0xCC, sizeof(ident));
        if (issue(d, 0xEC, 0, 0, ident, 512, false)) {
            kprintf("\033[1;31m[ahci] identify failed on port %d\033[0m\n", p);
            continue;
        }
        {
            u64 chk = vmm_translate((uptr)ident);
            volatile u16 *alias = (volatile u16 *)PHYS_TO_VIRT(chk);
            kprintf("[ahci] ident[0..3]=%04x %04x %04x %04x | phys[%04x %04x] prdbc=%u\n",
                    ident[0], ident[1], ident[2], ident[3],
                    alias[0], alias[1], d->clb[0].prdbc);
        }
        kprintf("[ahci] serr=%08x tfd=%02x\n", px_read(px, 0x30),
                px_read(px, P_TFD) & 0xFF);
        u64 sectors = *(u32 *)&ident[100];  /* LBA48 total sectors */
        if (!sectors || sectors > 0x100000000ULL / 512)
            sectors = *(u32 *)&ident[120];
        char model[41];
        for (int i = 0; i < 20; i++) {
            model[i * 2] = (char)(ident[27 + i] >> 8);
            model[i * 2 + 1] = (char)(ident[27 + i] & 0xFF);
        }
        model[40] = 0;

        d->bd.name = "sda";
        d->bd.sector_size = 512;
        d->bd.num_sectors = sectors;
        d->bd.read = ahci_rd;
        d->bd.write = ahci_wr;
        d->bd.drv = d;
        g_disk = d;
        blk_register(&d->bd);
        kprintf("[ahci] port %d: %.40s, %lu sectors (%lu MiB)\n", p, model,
                sectors, sectors / 2048);
        return 1;                   /* claimed: stop scanning */
    }
    return 0;
}

void ahci_init(void)
{
    /* Initialize block cache before scanning for devices */
    blk_cache_init();
    pci_scan(ahci_probe_fn, NULL);
}
