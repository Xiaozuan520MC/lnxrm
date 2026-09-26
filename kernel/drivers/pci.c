/* PCI configuration-space enumeration (port IO, bus 0..255). */
#include <pci.h>
#include <io.h>
#include <console.h>

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

u32 pci_read(u8 bus, u8 dev, u8 fn, u8 off)
{
    u32 addr = 0x80000000UL | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC);
    outl(CFG_ADDR, addr);
    return inl(CFG_DATA);
}

void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 val)
{
    u32 addr = 0x80000000UL | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC);
    outl(CFG_ADDR, addr);
    outl(CFG_DATA, val);
}

static pci_match_fn matcher;
static void *match_ctx;

static void probe_bus(u8 bus);

static void probe_fn(u8 bus, u8 dev, u8 fn)
{
    u32 id = pci_read(bus, dev, fn, 0x00);
    if (id == 0xFFFFFFFF) return;
    u32 classrev = pci_read(bus, dev, fn, 0x08);
    u8 base_class = classrev >> 24;

    if (matcher && matcher(match_ctx, bus, dev, fn, id & 0xFFFF, id >> 16, base_class))
        return; /* claimed */

    if (base_class == 6 && ((classrev >> 16) & 0xFF) == 4)
        probe_bus(pci_read(bus, dev, fn, 0x18) >> 8); /* PCI-to-PCI bridge */
}

static void probe_dev(u8 bus, u8 dev)
{
    u32 id = pci_read(bus, dev, 0, 0x00);
    if (id == 0xFFFFFFFF) return;
    probe_fn(bus, dev, 0);
    if (!(pci_read(bus, dev, 0, 0x0C) & 0x800000)) return; /* no multi-function */
    for (u8 fn = 1; fn < 8; fn++) probe_fn(bus, dev, fn);
}

static void probe_bus(u8 bus)
{
    for (u8 d = 0; d < 32; d++) probe_dev(bus, d);
}

void pci_scan(pci_match_fn fn, void *ctx)
{
    matcher = fn;
    match_ctx = ctx;
    probe_bus(0);
}
