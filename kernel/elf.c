/* Minimal static ELF64 loader for user programs. */
#include <elf.h>
#include <mm.h>
#include <console.h>

static int memeq(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    while (n--)
        if (*x++ != *y++)
            return 1;
    return 0;
}

u64 elf_load(u64 pml4, const void *img, size_t imgsize, u64 *brk_end)
{
    const Elf64_Ehdr *eh = img;
    if (imgsize < sizeof(*eh))
        return 0;
    if (memeq(eh->e_ident, "\x7f""ELF", 4) || eh->e_ident[4] != 2) {
        kprintf("[elf] not an ELF64 image\n");
        return 0;
    }
    if (eh->e_type != 2) {          /* ET_EXEC only */
        kprintf("[elf] e_type %d unsupported (need ET_EXEC)\n", eh->e_type);
        return 0;
    }

    u64 max_end = 0;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const u8 *)img + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (ph[i].p_vaddr + ph[i].p_memsz <= ph[i].p_vaddr ||
            ph[i].p_vaddr < USER_BASE || ph[i].p_vaddr >= USER_MAX_VMA)
            continue;               /* skip non-user segments (.note etc.) */
        u64 va = ALIGN_DOWN(ph[i].p_vaddr, PAGE_SIZE);
        u64 end = ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, PAGE_SIZE);

        for (u64 page = va; page < end; page += PAGE_SIZE) {
            if (vmm_translate_in(pml4, page))
                continue;
            u64 pa = pmm_alloc();
            if (!pa)
                return 0;
            memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
            bool w = !!(ph[i].p_flags & PF_W);
            vmm_map_user(pml4, page, pa, w, true);
        }

        /* segment bytes land exactly at p_vaddr; earlier bytes of its first
         * page stay zero (BSS-style padding) */
        memcpy((void *)ph[i].p_vaddr, (const u8 *)img + ph[i].p_offset,
               ph[i].p_filesz);

        if (end > max_end)
            max_end = end;
    }
    *brk_end = ALIGN_UP(max_end, PAGE_SIZE);
    return eh->e_entry;
}
