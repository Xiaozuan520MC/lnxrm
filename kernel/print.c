#include <console.h>
#include <mm.h>
#include <cpu.h>
#include <io.h>
#include <spinlock.h>

/* Serialize all kprintf output across CPUs. */
static spinlock_t print_lock = SPINLOCK_INIT;

/* VGA text console at the high alias; COM1 via the Rust UART driver. */
extern void lnxrm_uart_init(void);
extern void lnxrm_uart_putc(char c);
extern int  lnxrm_uart_trygetc(void);

/* framebuffer console */
#include <framebuffer.h>

#define VGA_COLS 80
#define VGA_ROWS 25
volatile u16 *vga = (volatile u16 *)VGA_VMA;
int vrow, vcol;

/* ring buffer that captures every character sent to the console,
 * so fb_console_init can replay messages that scrolled off VGA. */
#define RING_SIZE 4096
static char ring_buf[RING_SIZE];
static int  ring_head, ring_count;

static void ring_putc(char c)
{
    int idx = (ring_head + ring_count) % RING_SIZE;
    if (ring_count < RING_SIZE) {
        ring_buf[idx] = c;
        ring_count++;
    } else {
        ring_buf[ring_head] = c;
        ring_head = (ring_head + 1) % RING_SIZE;
    }
}

/* called by fb_console_init to replay buffered output */
int ring_get_count(void) { return ring_count; }
char ring_get_char(int i) { return ring_buf[(ring_head + i) % RING_SIZE]; }

static void vga_update_cursor(void);

void console_init(void)
{
    lnxrm_uart_init();
    for (int r = 0; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            vga[r * VGA_COLS + c] = 0x0700;
    vrow = vcol = 0;

    /* Set cursor shape: scanlines 13–15 (block cursor) */
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0D);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
    vga_update_cursor();
}

static void vga_update_cursor(void)
{
    u16 pos = vrow * VGA_COLS + vcol;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

static void vga_scroll(void)
{
    for (int r = 1; r < VGA_ROWS; r++)
        memcpy((void *)&vga[(r - 1) * VGA_COLS], (void *)&vga[r * VGA_COLS],
               VGA_COLS * 2);
    for (int c = 0; c < VGA_COLS; c++)
        vga[(VGA_ROWS - 1) * VGA_COLS + c] = 0x0700;
}

/* ANSI escape sequence state machine */
static int esc_state = 0;   /* 0=normal, 1=got ESC, 2=got ESC[ */
static char esc_buf[8];
static int esc_len = 0;
static u8 cur_attr = 0x07;  /* current VGA attribute (default: light grey on black) */

/* Map ANSI color code (30-37) to VGA color index */
static u8 ansi_to_vga(u8 code)
{
    /* ANSI: 30=black 31=red 32=green 33=yellow 34=blue 35=magenta 36=cyan 37=white */
    static const u8 map[] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    return map[code - 30];
}

static void esc_process(void)
{
    /* esc_buf contains the params after ESC[ */
    char last = esc_buf[esc_len - 1];
    if (last == 'J') {
        /* ESC[2J = clear screen */
        if (esc_len >= 2 && esc_buf[0] == '2') {
            for (int r = 0; r < VGA_ROWS; r++)
                for (int c = 0; c < VGA_COLS; c++)
                    vga[r * VGA_COLS + c] = 0x0700;
            vrow = vcol = 0;
        }
    } else if (last == 'H') {
        /* ESC[H or ESC[row;colH = cursor position (1-based) */
        int row = 1, col = 1;
        int num = 0, have_num = 0;
        for (int i = 0; i < esc_len - 1; i++) {
            if (esc_buf[i] >= '0' && esc_buf[i] <= '9') {
                num = num * 10 + (esc_buf[i] - '0');
                have_num = 1;
            } else if (esc_buf[i] == ';') {
                if (have_num) { row = num; num = 0; have_num = 0; }
            }
        }
        if (have_num) { col = num; }
        vrow = row - 1;
        vcol = col - 1;
        if (vrow < 0) vrow = 0;
        if (vrow >= VGA_ROWS) vrow = VGA_ROWS - 1;
        if (vcol < 0) vcol = 0;
        if (vcol >= VGA_COLS) vcol = VGA_COLS - 1;
    } else if (last == 'm') {
        /* ESC[...m = Set Graphics Rendition (colors) */
        int num = 0;
        u8 fg = cur_attr & 0x0F;
        u8 bg = (cur_attr >> 4) & 0x07;
        int have_num = 0;
        for (int i = 0; i < esc_len; i++) {
            if (esc_buf[i] >= '0' && esc_buf[i] <= '9') {
                num = num * 10 + (esc_buf[i] - '0');
                have_num = 1;
            } else if (esc_buf[i] == ';' || i == esc_len - 1) {
                if (have_num) {
                    if (num == 0) {
                        fg = 7; bg = 0;  /* reset */
                    } else if (num >= 30 && num <= 37) {
                        fg = ansi_to_vga(num);
                    } else if (num >= 40 && num <= 47) {
                        bg = ansi_to_vga(num - 10);
                    } else if (num == 1) {
                        fg |= 0x08;  /* bold = bright */
                    }
                    num = 0;
                    have_num = 0;
                }
            }
        }
        cur_attr = (bg << 4) | fg;
    }
}

void console_putc(char c)
{
    /* Always forward to fb console so ANSI escapes reach it */
    fb_console_putc(c);

    /* Feed ANSI escape sequences into the VGA state machine */
    if (esc_state == 0 && c == '\033') {
        esc_state = 1;
        return;
    }
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_len = 0;
        } else {
            esc_state = 0;
        }
        return;
    }
    if (esc_state == 2) {
        if (esc_len < (int)sizeof(esc_buf) - 1)
            esc_buf[esc_len++] = c;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            esc_buf[esc_len] = 0;
            esc_process();
            esc_state = 0;
        }
        return;
    }

    /* Normal character handling */
    if (vcol >= VGA_COLS) {
        vcol = 0;
        if (++vrow >= VGA_ROWS) {
            vga_scroll();
            vrow--;
        }
    }
    switch (c) {
    case '\b':
        if (vcol > 0) {
            vcol--;
            vga[vrow * VGA_COLS + vcol] = (cur_attr << 8) | ' ';
        }
        break;
    case '\n':
        vcol = 0;
        if (++vrow >= VGA_ROWS) {
            vga_scroll();
            vrow--;
        }
        break;
    case '\r':
        break;
    default:
        vga[vrow * VGA_COLS + vcol] = (cur_attr << 8) | (u8)c;
        vcol++;
    }
    lnxrm_uart_putc(c);
    ring_putc(c);
    vga_update_cursor();
}

int console_trygetc(void)
{
    extern int input_pop(void);
    return input_pop();
}

int console_getc(void)
{
    int c;
    while ((c = console_trygetc()) < 0) {
        __asm__ volatile("hlt");
    }
    return c;
}

/* ---- minimal printf ---- */
static const char digits[] = "0123456789abcdef";

static void emit(char **buf, size_t *left, char c)
{
    if (*left > 1) {
        **buf = c;
        (*buf)++;
        (*left)--;
    }
}

static void putnum(char **b, size_t *l, u64 v, unsigned base, bool sgn, int width,
                   char pad)
{
    char tmp[32];
    int i = 0;
    bool neg = false;
    if (sgn && (i64)v < 0) {
        neg = true;
        v = -(i64)v;
    }
    do {
        tmp[i++] = digits[v % base];
        v /= base;
    } while (v);
    if (neg)
        tmp[i++] = '-';
    int padn = width - i;
    while (padn-- > 0)
        emit(b, l, pad);
    while (i--)
        emit(b, l, tmp[i]);
}

size_t vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap)
{
    char *p = buf;
    size_t left = size;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(&p, &left, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' ||
               *fmt == '\'')
            fmt++;                      /* ignored flags */
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == '.') {              /* precision -- ignored */
            fmt++;
            while (*fmt >= '0' && *fmt <= '9')
                fmt++;
        }
        bool lng = false;
        if (*fmt == 'l') {
            lng = true;
            fmt++;
            if (*fmt == 'l')
                fmt++;
        } else if (*fmt == 'z') {
            lng = true;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
            putnum(&p, &left, lng ? __builtin_va_arg(ap, long)
                                  : __builtin_va_arg(ap, int),
                   10, true, width, pad);
            break;
        case 'u':
            putnum(&p, &left, lng ? __builtin_va_arg(ap, unsigned long)
                                  : __builtin_va_arg(ap, unsigned),
                   10, false, width, pad);
            break;
        case 'x':
            putnum(&p, &left, lng ? __builtin_va_arg(ap, unsigned long)
                                  : __builtin_va_arg(ap, unsigned),
                   16, false, width, pad);
            break;
        case 'p':
            emit(&p, &left, '0');
            emit(&p, &left, 'x');
            putnum(&p, &left, __builtin_va_arg(ap, uptr), 16, false, 16, '0');
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s)
                emit(&p, &left, *s++);
            break;
        }
        case 'c':
            emit(&p, &left, (char)__builtin_va_arg(ap, int));
            break;
        case '%':
            emit(&p, &left, '%');
            break;
        default:
            emit(&p, &left, '%');
            emit(&p, &left, *fmt);
        }
    }
    if (size) {
        if (left > 0)
            *p = 0;
        else
            buf[size - 1] = 0;
    }
    return p - buf;
}

void kvprintf(const char *fmt, __builtin_va_list ap)
{
    static char kbuf[1024];
    spin_lock(&print_lock);
    vsnprintf(kbuf, sizeof(kbuf), fmt, ap);
    for (char *s = kbuf; *s; s++)
        console_putc(*s);
    spin_unlock(&print_lock);
}

void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf(fmt, ap);
    __builtin_va_end(ap);
}

__attribute__((noreturn)) void panic(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kprintf("\033[1;31m[PANIC] ");  /* bold red */
    kvprintf(fmt, ap);
    kprintf("\033[0m\n");          /* reset color */
    __builtin_va_end(ap);
    __asm__ volatile("cli; hlt; jmp .");
    for (;;)
        ;
}
