/* usr/desktop.c -- Win98-style desktop environment */
#include "ulib.h"

/* ---- colour palette (VGA 16-colour + extended) ---- */
#define CLR_BLACK       0
#define CLR_BLUE        1
#define CLR_GREEN       2
#define CLR_CYAN        3
#define CLR_RED         4
#define CLR_MAGENTA     5
#define CLR_BROWN       6
#define CLR_LGREY       7
#define CLR_DGREY       8
#define CLR_LBLUE       9
#define CLR_LGREEN      10
#define CLR_LCYAN       11
#define CLR_LRED        12
#define CLR_LMAGENTA    13
#define CLR_YELLOW      14
#define CLR_WHITE       15

/* taskbar colours */
#define TB_HEIGHT       30
#define TB_BG           CLR_DGREY
#define TB_HIGHLIGHT     CLR_WHITE
#define TB_SHADOW        8
#define TB_BTNFACE       CLR_LGREY

/* ---- helpers ---- */
static void draw_hline(u32 x, u32 y, u32 w, u32 color)
{
    struct fb_rect r = { x, y, w, 1, color };
    kfb_fill(&r);
}

static void draw_vline(u32 x, u32 y, u32 h, u32 color)
{
    struct fb_rect r = { x, y, 1, h, color };
    kfb_fill(&r);
}

static void draw_rect(u32 x, u32 y, u32 w, u32 h, u32 color)
{
    struct fb_rect r = { x, y, w, h, color };
    kfb_fill(&r);
}

/* 3D raised border (Win98 style) */
static void draw_raised(u32 x, u32 y, u32 w, u32 h)
{
    draw_hline(x, y, w, CLR_WHITE);         /* top */
    draw_vline(x, y, h, CLR_WHITE);         /* left */
    draw_hline(x, y + h - 1, w, CLR_DGREY); /* bottom */
    draw_vline(x + w - 1, y, h, CLR_DGREY); /* right */
    draw_hline(x + 1, y + 1, w - 2, CLR_LGREY); /* inner top */
    draw_vline(x + 1, y + 1, h - 2, CLR_LGREY); /* inner left */
}

/* 3D sunken border */
static void draw_sunken(u32 x, u32 y, u32 w, u32 h)
{
    draw_hline(x, y, w, CLR_DGREY);
    draw_vline(x, y, h, CLR_DGREY);
    draw_hline(x, y + h - 1, w, CLR_WHITE);
    draw_vline(x + w - 1, y, h, CLR_WHITE);
    draw_hline(x + 1, y + 1, w - 2, 8);
    draw_vline(x + 1, y + 1, h - 2, 8);
}

/* ---- desktop icons ---- */
#define ICON_SIZE   40
#define ICON_LABEL_H 12

struct icon {
    const char *label;
    u32 x, y;
    u32 color;
    char letter;  /* single-char glyph */
};

static struct icon icons[] = {
    { "My Computer",  20,  20, CLR_LBLUE,   'C' },
    { "Recycle Bin",  20, 100, CLR_LGREY,   'R' },
    { "My Documents", 20, 180, CLR_YELLOW,  'D' },
    { "Terminal",     20, 260, CLR_GREEN,   '>' },
    { "fbtest",       20, 340, CLR_CYAN,    'F' },
};

#define N_ICONS (sizeof(icons) / sizeof(icons[0]))

static void draw_icon(struct icon *ic)
{
    /* icon body (raised 3D box) */
    draw_raised(ic->x, ic->y, ICON_SIZE, ICON_SIZE);
    draw_rect(ic->x + 2, ic->y + 2, ICON_SIZE - 4, ICON_SIZE - 4, ic->color);

    /* letter glyph in centre of icon */
    struct fb_char c;
    c.x = ic->x + 14;
    c.y = ic->y + 12;
    c.ch = ic->letter;
    c.fg = CLR_WHITE;
    c.bg = ic->color;
    kfb_char(&c);

    /* label below icon */
    struct fb_str s;
    s.x = ic->x - 10;
    s.y = ic->y + ICON_SIZE + 2;
    s.fg = CLR_WHITE;
    s.bg = CLR_BLUE;
    s.str = ic->label;
    kfb_puts(&s);
}

/* ---- window ---- */
#define WIN_MIN_W  200
#define WIN_MIN_H  120

static void draw_window(u32 x, u32 y, u32 w, u32 h, const char *title, u32 title_color)
{
    /* window body */
    draw_rect(x, y, w, h, CLR_LGREY);

    /* title bar */
    draw_rect(x + 2, y + 2, w - 4, 18, title_color);
    struct fb_str ts;
    ts.x = x + 6;
    ts.y = y + 4;
    ts.fg = CLR_WHITE;
    ts.bg = title_color;
    ts.str = title;
    kfb_puts(&ts);

    /* close button [X] */
    u32 bx = x + w - 20;
    u32 by = y + 3;
    draw_raised(bx, by, 16, 14);
    struct fb_char xc;
    xc.x = bx + 4;
    xc.y = by + 2;
    xc.ch = 'X';
    xc.fg = CLR_BLACK;
    xc.bg = CLR_LGREY;
    kfb_char(&xc);

    /* 3D border */
    draw_raised(x, y, w, h);
}

/* ---- taskbar ---- */
static void draw_taskbar(u32 screen_w, u32 screen_h)
{
    u32 ty = screen_h - TB_HEIGHT;

    /* background */
    draw_rect(0, ty, screen_w, TB_HEIGHT, TB_BG);

    /* top highlight line */
    draw_hline(0, ty, screen_w, CLR_WHITE);

    /* Start button */
    u32 btn_w = 60;
    draw_raised(2, ty + 3, btn_w, TB_HEIGHT - 6);
    draw_rect(3, ty + 4, btn_w - 2, TB_HEIGHT - 8, TB_BTNFACE);

    struct fb_str bs;
    bs.x = 8;
    bs.y = ty + 9;
    bs.fg = CLR_BLACK;
    bs.bg = TB_BTNFACE;
    bs.str = "Start";
    kfb_puts(&bs);

    /* task area (sunken) */
    draw_sunken(btn_w + 6, ty + 3, screen_w - btn_w - 60 - 10, TB_HEIGHT - 6);

    /* clock area (sunken) */
    draw_sunken(screen_w - 56, ty + 3, 52, TB_HEIGHT - 6);
}

/* ---- draw desktop ---- */
static void draw_desktop_bg(u32 screen_w, u32 screen_h)
{
    /* solid blue background (classic Win98) */
    kfb_clear(CLR_BLUE);

    /* draw icons */
    for (u32 i = 0; i < N_ICONS; i++)
        draw_icon(&icons[i]);
}

/* ---- simple animation (bouncing ball) ---- */
static void draw_ball(u32 x, u32 y, u32 color)
{
    struct fb_rect r = { x, y, 8, 8, color };
    kfb_fill(&r);
}

/* ---- main ---- */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct fb_info info;
    if (kfb_info(&info) < 0) {
        xputs("desktop: cannot get fb info\n");
        return 1;
    }

    u32 W = info.width;
    u32 H = info.height;

    /* draw initial desktop */
    draw_desktop_bg(W, H);

    /* draw a sample window */
    draw_window(W / 2 - 150, 60, 300, 200, "Welcome to lnxrm", CLR_BLUE);

    /* window content */
    struct fb_str ws;
    ws.x = W / 2 - 130;
    ws.y = 90;
    ws.fg = CLR_BLACK;
    ws.bg = CLR_LGREY;
    ws.str = "This is a Win98-style desktop";
    kfb_puts(&ws);

    ws.y = 108;
    ws.str = "running on lnxrm x86-64 kernel.";
    kfb_puts(&ws);

    ws.y = 126;
    ws.str = "Framebuffer: 640x480x256";
    kfb_puts(&ws);

    ws.y = 144;
    ws.str = "Try running other programs!";
    kfb_puts(&ws);

    /* draw taskbar */
    draw_taskbar(W, H);

    /* bouncing ball animation in the window */
    u32 bx = W / 2 - 120;
    u32 by = 170;
    int dx = 2, dy = 1;
    u32 ball_colors[] = { CLR_RED, CLR_GREEN, CLR_YELLOW, CLR_CYAN, CLR_LMAGENTA };
    int ci = 0;

    for (int frame = 0; frame < 300; frame++) {
        /* erase old ball */
        draw_rect(bx, by, 8, 8, CLR_LGREY);

        /* move */
        bx += dx;
        by += dy;

        /* bounce off window edges */
        if (bx <= W / 2 - 146 || bx >= W / 2 + 138) { dx = -dx; ci = (ci + 1) % 5; }
        if (by <= 170 || by >= 254) { dy = -dy; ci = (ci + 1) % 5; }

        /* draw new ball */
        draw_ball(bx, by, ball_colors[ci]);

        ksleep_ms(30);
    }

    xputs("desktop: done\n");
    return 0;
}
