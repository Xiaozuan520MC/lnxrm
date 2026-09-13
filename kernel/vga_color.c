#include <console.h>
#include <types.h>

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_VADDR 0xffffffff8c000000UL

static volatile u16 *vga_color_buf = (volatile u16 *)VGA_VADDR;

static inline u16 vga_make_entry(u8 ch, u8 fg, u8 bg)
{
    return ((u16)bg << 12) | ((u16)fg << 8) | ch;
}

static void vga_puts_at(int row, int col, const char *s, u8 fg, u8 bg)
{
    for (int i = 0; s[i]; i++)
        vga_color_buf[row * VGA_COLS + col + i] = vga_make_entry(s[i], fg, bg);
}

void vga_show_color_blocks(void)
{
    /* Clear screen */
    for (int r = 0; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            vga_color_buf[r * VGA_COLS + c] = vga_make_entry(' ', 7, 0);

    /* 16 color blocks in a single row, centered */
    int start_col = (VGA_COLS - 16) / 2;
    for (int i = 0; i < 16; i++)
        vga_color_buf[2 * VGA_COLS + start_col + i] = vga_make_entry(' ', 0, (u8)i);
}
