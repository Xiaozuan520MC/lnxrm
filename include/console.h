#pragma once
#include <types.h>


#ifdef __cplusplus
extern "C" {
#endif

void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, __builtin_va_list ap);
void console_init(void);
void console_putc(char c);          /* dual: VGA + COM1 */
int  console_getc(void);            /* blocking; kbd + serial */
int  console_trygetc(void);

void vga_show_color_blocks(void);

void panic(const char *fmt, ...) __attribute__((noreturn));

/* string (lib/string.c) */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
int memcmp(const void *a, const void *b, size_t n);
void *memmove(void *d, const void *s, size_t n);


#ifdef __cplusplus
}
#endif
