#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_MAX_WIDTH  1280
#define FB_MAX_HEIGHT 1024
#define FB_VMA        0xffffffff8d000000UL /* PD_HI[104], 4 KiB pages */

/* runtime values set from VBE mode info */
extern u32 fb_width;
extern u32 fb_height;
extern u32 fb_pitch;
extern u32 fb_bpp;   /* actual depth of the running mode (8/15/16/24/32) */
extern u32 fb_pxsz;  /* bytes per stored pixel */

void fb_init(void);
void fb_put_pixel(u32 x, u32 y, u8 color);
void fb_clear(u8 color);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u8 color);
void fb_draw_char(u32 x, u32 y, char ch, u8 fg, u8 bg);
void fb_puts(u32 x, u32 y, const char *s, u8 fg, u8 bg);

/* framebuffer text console (mirrors VGA text mode) */
void fb_console_init(void);
void fb_console_putc(char c);
extern int fb_active;

/* standard 16-color VGA palette indices */
#define FB_CLR_BLACK    0
#define FB_CLR_BLUE     1
#define FB_CLR_GREEN    2
#define FB_CLR_CYAN     3
#define FB_CLR_RED      4
#define FB_CLR_MAGENTA  5
#define FB_CLR_BROWN    6
#define FB_CLR_LGREY    7
#define FB_CLR_DGREY    8
#define FB_CLR_LBLUE    9
#define FB_CLR_LGREEN   10
#define FB_CLR_LCYAN    11
#define FB_CLR_LRED     12
#define FB_CLR_LMAGENTA 13
#define FB_CLR_YELLOW   14
#define FB_CLR_WHITE    15

#ifdef __cplusplus
}
#endif
