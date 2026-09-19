/* /bin/fdisk -- list block devices. */
#include "ulib.h"

/* must match kernel pack layout: name[16] + u32 + u64 = 28 bytes, no padding */
#pragma pack(push, 1)
struct disk_info {
    char name[16];
    u32 sector_size;
    u64 num_sectors;
};
#pragma pack(pop)

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    struct disk_info devs[8];

    long n = kdiskinfo(devs, 8);
    if (n <= 0) {
        xputs("fdisk: no block devices found\n");
        return 1;
    }

    xputs("Device  Sectors  MiB      SectorSize\n");
    for (int i = 0; i < n; i++) {
        xputs(devs[i].name);
        xputs("  ");
        xprinti(devs[i].num_sectors);
        xputs("  ");
        xprinti(devs[i].num_sectors / 2048);
        xputs("  ");
        xprinti(devs[i].sector_size);
        putc_('\n');
    }
    return 0;
}
