/* ANSI escape sequence tokenizer shared by the console backends. */
#include <console.h>

int ansi_feed(struct ansi_seq *s, char c)
{
    if (s->phase == 0) {
        if (c == '\033') {
            s->phase = 1;
            return ANSI_CONSUMED;
        }
        return ANSI_TEXT;
    }
    if (s->phase == 1) {
        s->phase = (c == '[') ? 2 : 0;
        if (c == '[') s->len = 0;
        return ANSI_CONSUMED;
    }
    if (s->len < (int)sizeof(s->buf) - 1) s->buf[s->len++] = c;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        s->buf[s->len] = 0;
        s->phase = 0;
        return ANSI_DONE;
    }
    return ANSI_CONSUMED;
}

int ansi_color_index(int code)
{
    static const u8 map[] = {0, 4, 2, 6, 1, 5, 3, 7};
    return map[code - 30];
}
