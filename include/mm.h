#pragma once
#include <types.h>


#ifdef __cplusplus
extern "C" {
#endif

/* ---- physical frame buddy allocator (mm/pmm.c) ---- */
void pmm_init(void);
u64  pmm_alloc_order(int order);        /* phys addr or 0 */
void pmm_free_order(u64 pa, int order);
#define pmm_alloc() pmm_alloc_order(0)
#define pmm_free(pa) pmm_free_order((pa), 0)
u64 pmm_total_pages(void);
u64 pmm_free_pages(void);

/* ---- kernel heap (mm/kheap.c) ---- */
void kheap_region(u64 *phys_out, u64 *size_out);   /* set by kheap_place */
void kheap_init(void);
void *kmalloc(size_t n);
void kfree(void *p);
size_t kmem_used(void);
size_t kmem_total(void);

/* ---- virtual memory (mm/vmm.c) ----
 * Master tables are shared by every address space (PML4 entries copied).
 * User space is a per-process tree under PML4[0]. */
void vmm_init(void);                    /* fills heap window, switches cr3 */
u64  master_pml4_phys(void);
u64  vmm_new_user_aspace(void);         /* returns pml4 phys for a process */
void vmm_destroy_user_aspace(u64 pml4);
int  vmm_map_user(u64 pml4, u64 va, u64 pa, bool writable, bool user);
u64  vmm_unmap_user(u64 pml4, u64 va);  /* returns freed pa or 0 */
u64  vmm_translate(u64 va);             /* in current address space */
u64  vmm_translate_in(u64 pml4, u64 va);
void vmm_switch_to(u64 pml4);
void vmm_dump(void);
bool vmm_is_user_range(u64 lo, u64 hi);
int dup_user_aspace(u64 src, u64 dst);

#define PG_P   0x001
#define PG_W   0x002
#define PG_U   0x004
#define PG_PS  0x080

/* remote PTE editing via the fixmap slot: used to touch other ASes */
#define FIXMAP_VA      0xffffffff8e000000UL
void fixmap_map(u64 pa);


#ifdef __cplusplus
}
#endif
