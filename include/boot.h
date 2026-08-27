#pragma once
#include <types.h>


#ifdef __cplusplus
extern "C" {
#endif

/* Linux boot_params (zero page) subset we consume. */
struct e820_entry {
    u64 addr;
    u64 size;
    u32 type;
} __attribute__((packed));

struct boot_params {
    u8  _pad0[0x1e8];
    u8  e820_entries;
    u8  _pad1[0x2d0 - 0x1e9];
    struct e820_entry e820_map[128];
} __attribute__((packed));

#define E820_RAM        1
#define E820_RESERVED   2
#define E820_ACPI       3
#define E820_NVS        4

struct boot_info {
    char cmdline[512];
    struct e820_entry map[64];
    int map_len;
};

extern struct boot_info bootinfo;

/* start_kernel: C entry, called from entry64.S with the zero-page pointer */
void start_kernel(struct boot_params *bp);


#ifdef __cplusplus
}
#endif
