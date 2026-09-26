/* Virtual memory: master kernel tables (built by entry64.S) + per-process
 * user address spaces under PML4[255].
 *
 * Address-space layout:
 *   slot 0   : identity map of low RAM (kernel-only, shared by ALL ASes)
 *   slot 255 : per-process user space  [0x7f8000000000, 0x7fffffffffff]
 *   slot 511 : high-half alias of physical memory (kernel image, heap,
 *              device windows, VGA), shared by all ASes.
 *
 * vmm_new_user_aspace() copies every PML4 entry except slot 255, so kernel
 * mappings propagate to all tasks automatically. */
#include <mm/mm.h>
#include <boot.h>
#include <console.h>

#define PD_HI  0x54000UL /* keep in sync with entry64.S */
#define PT_FIX 0x57000UL

#define ALIAS_BASE 0xffffffff80000000UL /* KERNEL_VMA - KERNEL_LMA */
#define USER_SLOT  255

static u64 pml4_phys = 0x50000UL;
static u64 heap_phys, heap_size;

void vmm_heap_region(u64 *phys_out, u64 *size_out)
{
    *phys_out = heap_phys;
    *size_out = heap_size;
}

u64 master_pml4_phys(void)
{ return pml4_phys; }

void kheap_place(u64 *region_top, u64 size)
{
    /* called from pmm_init BEFORE the buddy exists: reserve from the top */
    u64 top = ALIGN_DOWN(*region_top, PAGE_SIZE);
    heap_phys = ALIGN_DOWN(top - size, PAGE_SIZE);
    heap_size = size;
    *region_top = heap_phys;
}

static inline u64 *ptable_ptr(u64 pa)
{ return (u64 *)PHYS_TO_VIRT(pa); }

static u64 ensure_table(u64 *parent, u64 idx, u64 extra_flags)
{
    if (parent[idx] & PG_P) return parent[idx] & ~0xfffUL;
    u64 t = pmm_alloc();
    if (!t) panic("vmm: out of frames");
    memset(ptable_ptr(t), 0, PAGE_SIZE);
    parent[idx] = t | PG_P | PG_W | extra_flags;
    return t;
}

int vmm_map_user(u64 root, u64 va, u64 pa, bool writable, bool user)
{
    if (va < USER_BASE || va >= USER_MAX_VMA || (va & 0xfff)) return LNXRM_EFAIL;
    u64 i4 = (va >> 39) & 511, i3 = (va >> 30) & 511;
    u64 i2 = (va >> 21) & 511, i1 = (va >> 12) & 511;
    u64 uf = user ? PG_U : 0;

    u64 *pml4 = ptable_ptr(root);
    u64 pdpt_pa = ensure_table(pml4, i4, PG_U);
    u64 *pdpt = ptable_ptr(pdpt_pa);
    u64 pd_pa = ensure_table(pdpt, i3, PG_U);
    u64 *pd = ptable_ptr(pd_pa);
    u64 pt_pa = ensure_table(pd, i2, PG_U);
    u64 *pt = ptable_ptr(pt_pa);

    pt[i1] = pa | PG_P | (writable ? PG_W : 0) | uf;
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    return 0;
}

static int walk(u64 root, u64 va, u64 **pte_out, u64 *big_buf)
{
    u64 i4 = (va >> 39) & 511, i3 = (va >> 30) & 511;
    u64 i2 = (va >> 21) & 511, i1 = (va >> 12) & 511;
    u64 *pml4 = ptable_ptr(root);
    if (!(pml4[i4] & PG_P)) return LNXRM_EFAIL;
    u64 *pdpt = ptable_ptr(pml4[i4] & ~0xfff);
    if (!(pdpt[i3] & PG_P)) return LNXRM_EFAIL;
    u64 *pd = ptable_ptr(pdpt[i3] & ~0xfff);
    if (!(pd[i2] & PG_P)) return LNXRM_EFAIL;
    if (pd[i2] & PG_PS) {
        *big_buf = pd[i2]; /* caller only reads the phys bits */
        *pte_out = big_buf;
        return 1; /* 2 MiB leaf */
    }
    *pte_out = &ptable_ptr(pd[i2] & ~0xfff)[i1];
    return 0;
}

u64 vmm_translate_in(u64 root, u64 va)
{
    if (va >= ALIAS_BASE && va < ALIAS_BASE + 0x100000000UL) return va - ALIAS_BASE;
    u64 *pte;
    u64 big_buf;
    int r = walk(root, va, &pte, &big_buf);
    if (r < 0 || !(*pte & PG_P)) return 0;
    if (r == 1) /* 2 MiB leaf */
        return (*pte & ~0x1fffffUL) | (va & 0x1fffff);
    return (*pte & ~0xfffUL) | (va & 0xfff);
}

u64 vmm_unmap_user(u64 root, u64 va)
{
    u64 *pte;
    u64 big_buf;
    int r = walk(root, va, &pte, &big_buf);
    if (r < 0 || !(*pte & PG_P)) return 0;
    u64 pa;
    if (r == 1) {
        /* 2 MiB large page: need to clear the PD entry directly */
        u64 i4 = (va >> 39) & 511, i3 = (va >> 30) & 511;
        u64 i2 = (va >> 21) & 511;
        u64 *pd = ptable_ptr(ptable_ptr(ptable_ptr(root)[i4] & ~0xfff)[i3] & ~0xfff);
        pa = pd[i2] & ~0x1fffffUL;
        pd[i2] = 0;
    } else {
        pa = *pte & ~0xfffUL;
        *pte = 0;
    }
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    return pa;
}

bool vmm_is_user_range(u64 lo, u64 hi)
{ return lo >= USER_BASE && hi <= USER_MAX_VMA && lo < hi; }

void vmm_switch_to(u64 root)
{ __asm__ volatile("mov %0, %%cr3" ::"r"(root) : "memory"); }

u64 vmm_new_user_aspace(void)
{
    u64 root = pmm_alloc();
    if (!root) panic("vmm: no frame for aspace");
    memset(ptable_ptr(root), 0, PAGE_SIZE);
    for (int i = 0; i < 512; i++)
        if (i != USER_SLOT) ptable_ptr(root)[i] = ptable_ptr(pml4_phys)[i];
    return root;
}

void vmm_destroy_user_aspace(u64 root)
{
    u64 *pml4 = ptable_ptr(root);
    for (int i4 = 0; i4 < 512; i4++) {
        if (!(pml4[i4] & PG_P) || i4 == 0 || i4 == 511) continue;
        u64 *pdpt = ptable_ptr(pml4[i4] & ~0xfff);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PG_P)) continue;
            u64 *pd = ptable_ptr(pdpt[i3] & ~0xfff);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PG_P)) continue;
                if (pd[i2] & PG_PS) {
                    pmm_free_order(pd[i2] & ~0x1fffff, 9);
                    continue;
                }
                u64 *pt = ptable_ptr(pd[i2] & ~0xfff);
                for (int i1 = 0; i1 < 512; i1++)
                    if (pt[i1] & PG_P) pmm_free(pt[i1] & ~0xfff);
                pmm_free(pd[i2] & ~0xfff);
            }
            pmm_free(pdpt[i3] & ~0xfff);
        }
        pmm_free(pml4[i4] & ~0xfff);
    }
    pmm_free(root);
}

extern void load_cr3(u64);

void vmm_init(void)
{
    volatile u64 *pdhi = (volatile u64 *)PHYS_TO_VIRT(PD_HI);

    /* Map EVERY managed physical frame through the high alias. Slots 0..31
     * already hold the kernel image; fill the rest of the buddy/heap range
     * plus any gap so PHYS_TO_VIRT() is valid for all of RAM.
     * Use the actual kernel end symbol instead of a hardcoded address. */
    extern u8 __kernel_end[];
    u64 kern_end_pa = ALIGN_UP(VIRT_TO_PHYS((uptr)__kernel_end), 0x200000UL);
    u64 lo = kern_end_pa;
    u64 hi = ALIGN_UP(heap_phys + heap_size, 0x200000UL);

    /* the framebuffer is MMIO: fb_init() maps it uncached at FB_VMA, so it
     * must not also get a cacheable high-half alias here */
    u64 fb_lo = 0, fb_hi = 0;
    if (bootinfo.fb.ok && bootinfo.fb.phys_addr) {
        fb_lo = bootinfo.fb.phys_addr;
        fb_hi = fb_lo + (u64)bootinfo.fb.pitch * bootinfo.fb.height;
    }

    for (u64 pa = lo; pa < hi && pa < 0xC0000000UL; pa += 0x200000UL) {
        u64 idx = (pa >> 21) & 511;
        if (idx == 96 || idx == 97 || idx == 104 || idx == 112 || idx == 128)
            continue; /* VGA / FB / fixmap / device windows */
        if (fb_hi && pa < fb_hi && pa + 0x200000UL > fb_lo)
            continue; /* LFB page range */
        pdhi[idx] = pa | PG_P | PG_W | PG_PS;
    }

    /* kernel heap window (KHEAP_VMA -> PD_HI[256..271], 2 MiB pages) */
    for (int i = 0; i < 16; i++) pdhi[256 + i] = (heap_phys + i * 0x200000UL) | PG_P | PG_W | PG_PS;

    load_cr3(pml4_phys);
}

/* Map a single physical page into the kernel's DEV_VMA window (slot 98).
 * va must be in [DEV_VMA, DEV_VMA + 2 MiB).  flags: PG_PCD etc. */
void vmm_map_kernel_page(u64 va, u64 pa, u64 flags)
{
    u64 *pdhi = (u64 *)PHYS_TO_VIRT(PD_HI);
    u64 pd_idx = (va >> 21) & 511;

    if (!(pdhi[pd_idx] & PG_P)) {
        /* allocate a fresh page table for this PD slot */
        u64 pt_pa = pmm_alloc();
        if (!pt_pa) panic("vmm: no frame for MMIO pt");
        memset(ptable_ptr(pt_pa), 0, PAGE_SIZE);
        pdhi[pd_idx] = pt_pa | PG_P | PG_W;
    }
    /* split: if it's a 2 MiB page we must not clobber it;
     * callers only target fresh slots so this is fine. */
    u64 *pt = ptable_ptr(pdhi[pd_idx] & ~0xfffUL);
    u64 pt_idx = (va >> 12) & 511;
    pt[pt_idx] = (pa & ~0xfffUL) | PG_P | PG_W | flags;
    __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
}

/* Deep-copy the user subtree of `src` into `dst` (both PML4 phys).
 * Source pages are reachable through the CURRENT address space. */

/* cleanup helper: free any partially-built page tables in dst's user slot */
static void dup_cleanup(u64 dst)
{
    u64 *d4 = ptable_ptr(dst);
    if (!(d4[USER_SLOT] & PG_P)) return;
    /* walk and free everything under USER_SLOT, then clear the entry */
    u64 *ddpt = ptable_ptr(d4[USER_SLOT] & ~0xfff);
    for (int i3 = 0; i3 < 512; i3++) {
        if (!(ddpt[i3] & PG_P)) continue;
        u64 *dpd = ptable_ptr(ddpt[i3] & ~0xfff);
        for (int i2 = 0; i2 < 512; i2++) {
            if (!(dpd[i2] & PG_P)) continue;
            if (dpd[i2] & PG_PS) {
                pmm_free_order(dpd[i2] & ~0x1fffff, 9);
                continue;
            }
            u64 *dpt = ptable_ptr(dpd[i2] & ~0xfff);
            for (int i1 = 0; i1 < 512; i1++)
                if (dpt[i1] & PG_P) pmm_free(dpt[i1] & ~0xfff);
            pmm_free(dpd[i2] & ~0xfff);
        }
        pmm_free(ddpt[i3] & ~0xfff);
    }
    pmm_free(d4[USER_SLOT] & ~0xfff);
    d4[USER_SLOT] = 0;
}

int dup_user_aspace(u64 src, u64 dst)
{
    u64 slot = (u64)USER_SLOT << 39;

    u64 *s4 = ptable_ptr(src);
    if (!(s4[USER_SLOT] & PG_P)) return 0; /* nothing mapped yet */

    u64 *d4 = ptable_ptr(dst);
    u64 d3_pa = pmm_alloc(), s3 = s4[USER_SLOT] & ~0xfffUL;
    if (!d3_pa) return LNXRM_EFAIL;
    memset(ptable_ptr(d3_pa), 0, PAGE_SIZE);
    d4[USER_SLOT] = d3_pa | PG_P | PG_W | PG_U;

    u64 *sdpt = ptable_ptr(s3), *ddpt = ptable_ptr(d3_pa);
    for (int i3 = 0; i3 < 512; i3++) {
        if (!(sdpt[i3] & PG_P)) continue;
        u64 d2_pa = pmm_alloc();
        if (!d2_pa) goto fail;
        memset(ptable_ptr(d2_pa), 0, PAGE_SIZE);
        ddpt[i3] = d2_pa | PG_P | PG_W | PG_U;
        u64 *spd = ptable_ptr(sdpt[i3] & ~0xfff), *dpd = ptable_ptr(d2_pa);

        for (int i2 = 0; i2 < 512; i2++) {
            if (!(spd[i2] & PG_P)) continue;
            u64 d1_pa = pmm_alloc();
            if (!d1_pa) goto fail;
            memset(ptable_ptr(d1_pa), 0, PAGE_SIZE);
            dpd[i2] = d1_pa | PG_P | PG_W | PG_U;
            u64 *spt = ptable_ptr(spd[i2] & ~0xfff), *dpt = ptable_ptr(d1_pa);

            for (int i1 = 0; i1 < 512; i1++) {
                if (!(spt[i1] & PG_P)) continue;
                u64 va = slot | ((u64)i3 << 30) | ((u64)i2 << 21) | ((u64)i1 << 12);
                /* defensive: only copy pages that truly resolve */
                if (!vmm_translate_in(src, va)) continue;
                u64 npa = pmm_alloc();
                if (!npa) goto fail;
                memcpy((void *)PHYS_TO_VIRT(npa), (void *)va, PAGE_SIZE);
                dpt[i1] = npa | (spt[i1] & 0xFFFUL);
            }
        }
    }
    return 0;

fail:
    dup_cleanup(dst);
    return LNXRM_EFAIL;
}
