#include <console.h>
#include <mm.h>
#include <cpu.h>

/* VGA text console at the high alias; COM1 via the Rust UART driver. */
extern void lnxrm_uart_init(void);
extern void lnxrm_uart_putc(char c);
extern int  lnxrm_uart_trygetc(void);

#define VGA_COLS 80
#define VGA_ROWS 25
static volatile u16 *vga = (volatile u16 *)VGA_VMA;
static int vrow, vcol;

void console_init(void)
{
    lnxrm_uart_init();
    for (int r = 0; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            vga[r * VGA_COLS + c] = 0x0700;
    vrow = vcol = 0;
}

static void vga_scroll(void)
{
    for (int r = 1; r < VGA_ROWS; r++)
        memcpy((void *)&vga[(r - 1) * VGA_COLS], (void *)&vga[r * VGA_COLS],
               VGA_COLS * 2);
    for (int c = 0; c < VGA_COLS; c++)
        vga[(VGA_ROWS - 1) * VGA_COLS + c] = 0x0700;
}

void console_putc(char c)
{
    if (vcol >= VGA_COLS) {
        vcol = 0;
        if (++vrow >= VGA_ROWS) {
            vga_scroll();
            vrow--;
        }
    }
    switch (c) {
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
        vga[vrow * VGA_COLS + vcol] = 0x0700 | (u8)c;
        vcol++;
    }
    lnxrm_uart_putc(c);
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
    vsnprintf(kbuf, sizeof(kbuf), fmt, ap);
    for (char *s = kbuf; *s; s++)
        console_putc(*s);
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
    kprintf("\n[PANIC] ");
    kvprintf(fmt, ap);
    kprintf("\n");
    __builtin_va_end(ap);
    __asm__ volatile("cli; hlt; jmp .");
    for (;;)
        ;
}
