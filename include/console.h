#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, __builtin_va_list ap);
void console_init(void);
void console_putc(char c); /* dual: VGA + COM1 */

void panic(const char *fmt, ...) __attribute__((noreturn));

/* ring buffer for replaying boot messages in framebuffer console */
int ring_get_count(void);
char ring_get_char(int i);

/* ---- ANSI (CSI) escape tokenizer (lib/.. ansi.c) ----
 * Both console backends (VGA text in print.c, framebuffer in framebuffer.c)
 * consume character streams that may contain sequences like "ESC [ 3 2 m".
 * Only the tokenizing state machine lives here; interpreting the sequence is
 * device specific. */

#define ANSI_SEQ_MAX 8

struct ansi_seq {
    int phase; /* 0 = text, 1 = got ESC, 2 = got ESC[ */
    int len;
    char buf[ANSI_SEQ_MAX]; /* params + final byte, NUL terminated */
};

enum {
    ANSI_TEXT = -1,    /* ordinary character: render it */
    ANSI_CONSUMED = 0, /* part of a sequence: swallow it */
    ANSI_DONE = 1,     /* complete sequence in s->buf */
};

/* Feed one character. On ANSI_DONE, s->buf holds the bytes after "ESC[",
 * including the terminating letter (NUL terminated). */
int ansi_feed(struct ansi_seq *s, char c);

/* ANSI colour code (30-37 foreground / 40-47 background) to palette index. */
int ansi_color_index(int code);

/* string (lib/string.c) */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strcasecmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
int memcmp(const void *a, const void *b, size_t n);

#ifdef __cplusplus
}
#endif
