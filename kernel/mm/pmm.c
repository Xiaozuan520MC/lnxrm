/* Physical frame buddy allocator (MAX_ORDER 10 => 4 MiB blocks).
 * Free lists are intrusive: the first 8 bytes of each free block hold the
 * next-free pointer. Per-order bitmaps record which blocks are free so the
 * buddy of a block being freed can be found and coalesced. */
#include <mm.h>
#include <console.h>
#include <boot.h>

#define MAX_ORDER 11
#define MIN_ORDER_BITS 12

struct freelist {
    u64 head;
    u64 count;
};

static struct freelist fl[MAX_ORDER];
static u8 *order_bits[MAX_ORDER];   /* bitmaps, allocated from low scratch */
#define BITS_BASE 0x80000UL         /* bitmap scratch area (below EBDA) */

static u64 base_addr, top_addr;     /* managed window */
static u64 reserved_lo, reserved_hi;/* [lo,hi) excluded (kernel image etc.) */
static u64 total_pg, free_pg;

static inline u64 bits_bytes(int order)
{
    return ((top_addr - base_addr) >> (MIN_ORDER_BITS + order) + 7) / 8 + 1;
}

static inline int bit_get(int order, u64 pa)
{
    u64 idx = (pa - base_addr) >> (MIN_ORDER_BITS + order);
    return !!(order_bits[order][idx >> 3] & (1 << (idx & 7)));
}

static inline void bit_set(int order, u64 pa)
{
    u64 idx = (pa - base_addr) >> (MIN_ORDER_BITS + order);
    order_bits[order][idx >> 3] |= 1 << (idx & 7);
}

static inline void bit_clr(int order, u64 pa)
{
    u64 idx = (pa - base_addr) >> (MIN_ORDER_BITS + order);
    order_bits[order][idx >> 3] &= ~(1 << (idx & 7));
}

static inline u64 buddy_of(u64 pa, int order)
{
    u64 sz = 1UL << (MIN_ORDER_BITS + order);
    return pa ^ sz;
}

void pmm_init(void)
{

    /* find the largest usable E820 region above 1 MiB */
    u64 best = 0, best_end = 0;
    for (int i = 0; i < bootinfo.map_len; i++) {
        struct e820_entry *e = &bootinfo.map[i];
        if (e->type != E820_RAM)
            continue;
        u64 lo = MAX(e->addr, 0x100000UL);
        u64 hi = e->addr + e->size;
        if (lo >= hi || lo > 0x100000000UL) /* stay under 4 GiB for DMA ease */
            continue;
        hi = MIN(hi, 0x100000000UL);
        if (hi - lo > best_end - best) {
            best = lo;
            best_end = hi;
        }
    }
    /* reserve everything up to the END of .bss (which contains the boot
     * stack): __bss_end is a VMA symbol */
    extern u8 __bss_end[], __initramfs_start[], __initramfs_end[];
    u64 kern_end = ALIGN_UP(VIRT_TO_PHYS((uptr)__bss_end), PAGE_SIZE);
    u64 ir_end = ALIGN_UP(VIRT_TO_PHYS((uptr)__initramfs_end), PAGE_SIZE);

    /* carve the kernel heap out of the top before the buddy sees it */
    extern void kheap_place(u64 * region_top, u64 size);
    u64 heap_top = ALIGN_DOWN(best_end, 0x200000UL);
    kheap_place(&heap_top, 16 << 20);   /* reserves downward, returns new top */

    reserved_lo = best;
    reserved_hi = MAX(kern_end, ir_end);
    /* 2 MiB safety margin past the kernel image -- guards against any
     * off-by-frame in early allocations touching image-adjacent RAM */
    base_addr = ALIGN_UP(reserved_hi, PAGE_SIZE) + 0x200000UL;
    top_addr = heap_top;

    memset((u8 *)BITS_BASE, 0, 0x10000);
    for (int o = 0; o < MAX_ORDER; o++)
        order_bits[o] = (u8 *)BITS_BASE + o * 0x2000;

    total_pg = (top_addr - base_addr) >> MIN_ORDER_BITS;
    free_pg = 0;

    /* seed the free lists with maximal aligned power-of-two blocks */
    u64 cur = base_addr;
    while (cur < top_addr) {
        int o = 0;
        while (o + 1 < MAX_ORDER &&
               ((cur & ((2UL << (MIN_ORDER_BITS + o)) - 1)) == 0) &&
               cur + (2UL << (MIN_ORDER_BITS + o)) <= top_addr)
            o++;
        pmm_free_order(cur, o);
        cur += 1UL << (MIN_ORDER_BITS + o);
    }
    kprintf("[pmm] frames %#lx..%#lx (%lu KiB)\n", base_addr, top_addr,
            (top_addr - base_addr) >> 10);
}

u64 pmm_alloc_order(int order)
{
    int o = order;
    if (o >= MAX_ORDER)
        return 0;
    while (o < MAX_ORDER && fl[o].count == 0)
        o++;
    if (o >= MAX_ORDER)
        return 0;

    u64 addr = fl[o].head;
    u64 next = *(u64 *)(uptr)addr;
    fl[o].head = next;
    fl[o].count--;
    bit_clr(o, addr);

    /* split down to the requested order */
    while (o > order) {
        o--;
        u64 half = addr + (1UL << (MIN_ORDER_BITS + o));
        *(u64 *)(uptr)half = fl[o].head;
        fl[o].head = half;
        fl[o].count++;
        bit_set(o, half);
    }
    free_pg -= 1UL << order;
    return addr;
}

void pmm_free_order(u64 pa, int order)
{
    int o = order;
    while (o + 1 < MAX_ORDER) {
        u64 bud = buddy_of(pa, o);
        if (bud < base_addr || bud >= top_addr)
            break;                  /* buddy outside managed window */
        if (!bit_get(o, bud))
            break;
        /* unlink buddy from list[o] */
        u64 *p = &fl[o].head;
        while (*p != bud)
            p = (u64 *)(uptr)*p;
        *p = *(u64 *)(uptr)bud;
        fl[o].count--;
        bit_clr(o, bud);
        pa = MIN(pa, bud);
        o++;
    }
    *(u64 *)(uptr)pa = fl[o].head;
    fl[o].head = pa;
    fl[o].count++;
    bit_set(o, pa);
    free_pg += 1UL << order;
}

u64 pmm_total_pages(void)
{
    return total_pg;
}

u64 pmm_free_pages(void)
{
    return free_pg;
}
