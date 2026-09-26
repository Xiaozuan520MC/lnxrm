#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- physical frame buddy allocator (mm/pmm.c) ---- */
void pmm_init(void);
void pmm_reserve(u64 lo, u64 hi); /* keep [lo,hi) out of the free lists (MMIO) */
u64 pmm_alloc_order(int order); /* phys addr or 0 */
void pmm_free_order(u64 pa, int order);
#define pmm_alloc()  pmm_alloc_order(0)
#define pmm_free(pa) pmm_free_order((pa), 0)

/* ---- kernel heap (mm/kheap.c) ---- */
void kheap_init(void);
void *kmalloc(size_t n);
void kfree(void *p);

/* ---- virtual memory (mm/vmm.c) ----
 * Master tables are shared by every address space (PML4 entries copied).
 * User space is a per-process tree under PML4[0]. */
void vmm_init(void); /* fills heap window, switches cr3 */
u64 master_pml4_phys(void);
u64 vmm_new_user_aspace(void); /* returns pml4 phys for a process */
void vmm_destroy_user_aspace(u64 pml4);
int vmm_map_user(u64 pml4, u64 va, u64 pa, bool writable, bool user);
u64 vmm_unmap_user(u64 pml4, u64 va); /* returns freed pa or 0 */
u64 vmm_translate_in(u64 pml4, u64 va);
void vmm_switch_to(u64 pml4);
bool vmm_is_user_range(u64 lo, u64 hi);
int dup_user_aspace(u64 src, u64 dst);

#define PG_P   0x001
#define PG_W   0x002
#define PG_U   0x004
#define PG_PWT 0x008
#define PG_PCD 0x010
#define PG_PS  0x080

/* remote PTE editing via the fixmap slot: used to touch other ASes */
#define FIXMAP_VA 0xffffffff8e000000UL

/* kernel-only MMIO mapping into DEV_VMA window (slot 98, 2 MiB).
 * Maps one 4 KiB page: DEV_VMA + offset → phys. */
void vmm_map_kernel_page(u64 va, u64 pa, u64 flags);

#ifdef __cplusplus
}
#endif
