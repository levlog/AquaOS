/*
 * AquaOS boot splash / desktop shell (v4).
 *
 * Renders directly on the Linux framebuffer (/dev/fb0):
 *   - boot:        live kernel log lines (from /dev/kmsg, real records) in the
 *                  bottom-left corner, white macOS-style arc spinner in center
 *   - transition:  smooth 1.4 s crossfade into the desktop
 *   - desktop:     wallpaper + frosted menu bar ("Settings >" -> "About"),
 *                  dark frosted dock with the Terminal app (icon magnifies on
 *                  hover like macOS), live FPS counter top-right
 *   - terminal:    real terminal window (busybox sh on a pty, keyboard input
 *                  via evdev) with macOS chrome: traffic lights, shadow,
 *                  open/minimize/restore/close animations
 *   - rendering:   dirty-rect compositor: only regions that actually changed
 *                  are recomposed and pushed to the framebuffer, which keeps
 *                  the loop at ~60 fps even under emulation at 1920x1080
 *
 * No fake data: every log line is a genuine kernel record with its genuine
 * kernel timestamp, the FPS number is a genuine measurement of frames
 * actually composed, the terminal runs a real shell.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

#include "font.h"
#include "font_ui.h"
#include "terminal_icon.h"
#include "drop_icon.h"

/* ---------------- tuning ------------------------------------------- */
#define WALL_PATH        "/usr/share/splash/wallpaper.raw"
#define DONE_PATH        "/run/bootdone"
#define READY_PATH       "/run/splash.ready"
#define KMSG_PATH        "/dev/kmsg"
#define WALL_W           1920    /* baked wallpaper size (build-time)   */
#define WALL_H           1080
#define FADE_SECONDS     1.4     /* crossfade duration                  */
#define SPIN_PERIOD      1.25    /* seconds per spinner revolution      */
#define REVEAL_PER_FRAME 3       /* log lines revealed per frame        */
#define DISPLAY_LINES    14      /* log lines visible simultaneously    */
#define LOG_MARGIN       16      /* log area margin, px                 */
#define LOG_LINE_MAX     160
#define HIST_MAX         512
#define SAFETY_SECONDS   45.0    /* fade even if init never signals     */
#define FRAME_DT         (1.0 / 60.0)
#define FPS_WINDOW       0.5     /* FPS averaging window, seconds       */
#define MENU_X           10      /* panel logo x offset, px             */
#define DOCK_MARGIN_B    7       /* dock gap from the bottom edge, px   */
#define CURSOR_W         13      /* arrow cursor sprite, px (scaled)    */
#define CURSOR_H         19
#define ICON_BASE        56      /* dock icon size, unit-px             */
#define DOCK_W           240     /* dock width, unit-px                 */
#define WIN_R            10      /* terminal window corner radius, px   */
#define TERM_TITLE       "Terminal - sh"
#define TERM_DONE        "Done - sh"

/* terminal grid limits */
#define TCOLS_MAX 130
#define TROWS_MAX 40

static const int LOG_R = 185, LOG_G = 187, LOG_B = 193;

/* ---------------- framebuffer --------------------------------------- */
static int fb_fd = -1;
static uint8_t *fb_map;
static int W, H, BPP;
static struct fb_bitfield rf, gf, bf;
static struct fb_fix_screeninfo finfo;
static bool fast32 = false;
static uint32_t *back;    /* internal ARGB8888 composition buffer      */
static uint32_t *wall;    /* wallpaper ARGB8888                        */
static uint32_t *desktop; /* final desktop texture (wallpaper+panel)   */
static int panel_h;       /* macOS-style top panel height              */
static int fps_now;       /* real measured frames per second           */
static bool fps_ready;    /* first measurement window completed        */

/* clip rect (inclusive); every draw must respect it                     */
static int cx0, cy0, cx1, cy1;

static inline void clip_full(void) { cx0 = 0; cy0 = 0; cx1 = W - 1; cy1 = H - 1; }
static inline bool in_clip(int x, int y)
{
    return x >= cx0 && x <= cx1 && y >= cy0 && y <= cy1;
}

/* UI scales */
static double u;          /* linear unit scale, H/768                  */

/* mouse / menu / cursor state */
static int mouse_fd = -1;
static int vt_fd = -1;    /* /dev/tty0 kept open in KD_GRAPHICS mode    */
static double mouse_try = 0;
static int mx, my;        /* cursor position, hotspot at the tip       */
static int pmx, pmy;      /* cursor position on the previous frame     */
static int btn_l;         /* left button state                         */
static double last_clk = -1;  /* click debounce timer                      */
static bool menu_open = false;
static double menu_a = 0; /* dropdown animation 0..1                   */
static bool menu_closing = false;
static bool in_desktop = false;
static int ti_x0, ti_y0, ti_x1, ti_y1;   /* "Settings" item rect       */
static int dr_x0, dr_y0, dr_x1, dr_y1;   /* droplet logo rect          */
static int set_x;                        /* "Settings" text x          */
static int dd_x0, dd_y0, dd_x1, dd_y1;   /* dropdown rect              */
static int it_x0, it_y0, it_x1, it_y1;   /* "About AquaOS" item rect   */

/* macOS menu bar clock (real system time, HH:MM) */
static char clk_str[8];
static bool clk_valid = false;

/* About panel (real data only: kernel, framebuffer, uptime, fps) */
static bool about_open = false;
static bool about_dirty = false;
static int ab_x0, ab_y0, ab_x1, ab_y1;
static char kver[48];          /* kernel release, parsed from real kmsg */

/* pre-baked frosted panels (dropdown / About) incl. soft shadow  */
static uint32_t *menu_tex;
static int menu_tex_w, menu_tex_h;
static int DDMc;                          /* dropdown shadow margin     */
static uint32_t *about_tex;
static int about_tex_w, about_tex_h;
static int AMDc;                          /* About shadow margin        */

/* dock state */
static int ddx0, ddy0, ddx1, ddy1;       /* dock glass rect            */
static int dock_h;
static double icon_s = 1.0;              /* current magnification      */
static double icon_s_drawn = -1.0;       /* last drawn magnification   */
static bool dock_dot_drawn = false;      /* running-dot visibility     */

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int fb_open(void)
{
    for (int i = 0; i < 300; i++) {           /* wait up to 30 s */
        fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd >= 0)
            break;
        usleep(100000);
    }
    if (fb_fd < 0)
        return -1;
    struct fb_var_screeninfo v;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v))
        return -1;
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo))
        return -1;
    W = v.xres; H = v.yres;
    BPP = (v.bits_per_pixel + 7) / 8;
    rf = v.red; gf = v.green; bf = v.blue;
    fb_map = mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_map == MAP_FAILED)
        return -1;
    back = malloc((size_t)W * H * 4);
    if (!back)
        return -1;
    fast32 = (BPP == 4 && rf.offset == 16 && gf.offset == 8 && bf.offset == 0 &&
              rf.length == 8 && gf.length == 8 && bf.length == 8);
    u = (double)H / 1080.0;    /* UI metrics are authored at 1080p */
    clip_full();
    fprintf(stderr, "splash: fb0 %dx%d %dbpp (fast32=%d, 1=%d, u=%.3f)\n",
            W, H, BPP * 8, fast32, 1, u);
    return 0;
}

static inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* push a rect of `back` to the framebuffer (per-pixel conversion if needed) */
static void blit_rect(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    if (x0 > x1 || y0 > y1)
        return;
    int rw = x1 - x0 + 1;
    if (fast32) {
        for (int y = y0; y <= y1; y++)
            memcpy(fb_map + (size_t)y * finfo.line_length + (size_t)x0 * 4,
                   back + (size_t)y * W + x0, (size_t)rw * 4);
        return;
    }
    for (int y = y0; y <= y1; y++) {
        const uint32_t *src = back + (size_t)y * W + x0;
        uint8_t *row = fb_map + (size_t)y * finfo.line_length;
        if (BPP == 4) {
            uint32_t *d = (uint32_t *)row + x0;
            for (int x = 0; x < rw; x++) {
                uint32_t v = src[x];
                d[x] = (((v >> 16 & 255) >> (8 - rf.length)) << rf.offset) |
                       (((v >>  8 & 255) >> (8 - gf.length)) << gf.offset) |
                       (((v       & 255) >> (8 - bf.length)) << bf.offset);
            }
        } else if (BPP == 3) {
            for (int x = 0; x < rw; x++) {
                uint32_t v = src[x];
                uint32_t val = (((v >> 16 & 255) >> (8 - rf.length)) << rf.offset) |
                               (((v >>  8 & 255) >> (8 - gf.length)) << gf.offset) |
                               (((v       & 255) >> (8 - bf.length)) << bf.offset);
                uint8_t *d = row + (size_t)(x0 + x) * 3;
                d[0] = val & 0xff; d[1] = (val >> 8) & 0xff; d[2] = (val >> 16) & 0xff;
            }
        } else if (BPP == 2) {
            uint16_t *d = (uint16_t *)row + x0;
            for (int x = 0; x < rw; x++) {
                uint32_t v = src[x];
                d[x] = (((v >> 16 & 255) >> (8 - rf.length)) << rf.offset) |
                       (((v >>  8 & 255) >> (8 - gf.length)) << gf.offset) |
                       (((v       & 255) >> (8 - bf.length)) << bf.offset);
            }
        }
    }
}

/* blend `alpha` (0..256) of `tex` over `back` at 1:1, clipped */
/* (opaque blit removed in v6; alpha-aware tex_blit_alpha lives below) */

/* nearest-neighbour scaled blend of `tex` into an arbitrary dst rect */
static void tex_blit_scaled(const uint32_t *tex, int tw, int th,
                            int dx0, int dy0, int dx1, int dy1, int alpha)
{
    if (dx1 < dx0 || dy1 < dy0 || tw <= 0 || th <= 0)
        return;
    int dw = dx1 - dx0 + 1, dh = dy1 - dy0 + 1;
    int x0 = dx0 < cx0 ? cx0 : dx0;
    int y0 = dy0 < cy0 ? cy0 : dy0;
    int x1 = dx1 > cx1 ? cx1 : dx1;
    int y1 = dy1 > cy1 ? cy1 : dy1;
    for (int y = y0; y <= y1; y++) {
        int sy = (int)(((long)(y - dy0) * th) / dh);
        if (sy >= th) sy = th - 1;
        const uint32_t *srow = tex + (size_t)sy * tw;
        uint32_t *d = back + (size_t)y * W;
        for (int x = x0; x <= x1; x++) {
            int sx = (int)(((long)(x - dx0) * tw) / dw);
            if (sx >= tw) sx = tw - 1;
            uint32_t v = d[x], w = srow[sx];
            unsigned r = ((v >> 16 & 255) * (256 - alpha) + (w >> 16 & 255) * alpha) >> 8;
            unsigned g = ((v >>  8 & 255) * (256 - alpha) + (w >>  8 & 255) * alpha) >> 8;
            unsigned b = ((v       & 255) * (256 - alpha) + (w       & 255) * alpha) >> 8;
            d[x] = rgb(r, g, b);
        }
    }
}

/* ---------------- wallpaper ------------------------------------------ */
static int wall_load(void)
{
    FILE *f = fopen(WALL_PATH, "rb");
    if (!f)
        return -1;
    uint8_t *raw = malloc((size_t)WALL_W * WALL_H * 3);
    if (!raw || fread(raw, 1, (size_t)WALL_W * WALL_H * 3, f)
                != (size_t)WALL_W * WALL_H * 3) {
        fclose(f);
        free(raw);
        return -1;
    }
    fclose(f);
    wall = malloc((size_t)W * H * 4);
    if (!wall) {
        free(raw);
        return -1;
    }
    for (int y = 0; y < H; y++) {             /* nearest-neighbour cover */
        int sy = (int)((long)y * WALL_H / H);
        if (sy >= WALL_H) sy = WALL_H - 1;
        const uint8_t *s = raw + (size_t)sy * WALL_W * 3;
        uint32_t *d = wall + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            int sx = (int)((long)x * WALL_W / W);
            if (sx >= WALL_W) sx = WALL_W - 1;
            d[x] = rgb(s[sx * 3], s[sx * 3 + 1], s[sx * 3 + 2]);
        }
    }
    free(raw);
    return 0;
}

/* ---------------- blur / sdf helpers ---------------------------------- */
static void box_blur_h(uint32_t *dst, const uint32_t *src, int w, int h, int R)
{
    int *P = malloc(sizeof(int) * (size_t)(w + 1));
    if (!P)
        return;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = src + (size_t)y * w;
        uint32_t *out = dst + (size_t)y * w;
        for (int c = 0; c < 3; c++) {
            int sh = 16 - c * 8;              /* r, g, b */
            P[0] = 0;
            for (int x = 0; x < w; x++)
                P[x + 1] = P[x] + ((row[x] >> sh) & 255);
            for (int x = 0; x < w; x++) {
                int lo = x - R; if (lo < 0) lo = 0;
                int hi = x + R; if (hi > w - 1) hi = w - 1;
                int v = (P[hi + 1] - P[lo]) / (hi - lo + 1);
                out[x] = (out[x] & ~(255u << sh)) | ((uint32_t)v << sh);
            }
        }
    }
    free(P);
}

static void box_blur_v(uint32_t *dst, const uint32_t *src, int w, int h, int R)
{
    int *P = malloc(sizeof(int) * (size_t)(h + 1));
    if (!P)
        return;
    for (int x = 0; x < w; x++) {
        for (int c = 0; c < 3; c++) {
            int sh = 16 - c * 8;
            P[0] = 0;
            for (int y = 0; y < h; y++)
                P[y + 1] = P[y] + ((src[(size_t)y * w + x] >> sh) & 255);
            for (int y = 0; y < h; y++) {
                int lo = y - R; if (lo < 0) lo = 0;
                int hi = y + R; if (hi > h - 1) hi = h - 1;
                int v = (P[hi + 1] - P[lo]) / (hi - lo + 1);
                dst[(size_t)y * w + x] = (dst[(size_t)y * w + x] & ~(255u << sh))
                                       | ((uint32_t)v << sh);
            }
        }
    }
    free(P);
}

/* signed distance to a rounded box (positive outside) */
static double sd_round(double px, double py,
                       double x0, double y0, double x1, double y1, double r)
{
    double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
    double hx = (x1 - x0) * 0.5 - r, hy = (y1 - y0) * 0.5 - r;
    double qx = fabs(px - cx) - hx, qy = fabs(py - cy) - hy;
    double ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    return sqrt(ax * ax + ay * ay) + fmin(fmax(qx, qy), 0.0) - r;
}

/* distance from a point to a line segment (for vector overlays) */
static double seg_dist(double px, double py,
                       double ax, double ay, double bx, double by)
{
    double vx = bx - ax, vy = by - ay;
    double t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrt(dx * dx + dy * dy);
}

/* ---------------- terminal window geometry & textures ------------------ */
static int ww, wh, wx, wy;        /* terminal window target rect (normal) */
static int tw_title;              /* title bar height                     */
static uint32_t *winbuf;          /* window texture (max size)            */
static int winbuf_w, winbuf_h;    /* current texture dims (incl. margin)  */
static int SMc;                   /* baked shadow margin around window    */
static uint32_t *winblur;         /* blurred backdrop under the window    */
static int winblur_w, winblur_h, winblur_x0, winblur_y0;
static int mw_x0, mw_y0, mw_x1, mw_y1;   /* maximized ("zoomed") rect     */

/* terminal states */
enum TmSt { TM_CLOSED, TM_OPENING, TM_OPEN, TM_MINIMIZING, TM_MINIMIZED,
            TM_RESTORE, TM_CLOSING, TM_ZOOMIN, TM_ZOOMOUT };
static enum TmSt tm = TM_CLOSED;
static bool tm_dead = false;      /* shell exited                         */
static bool caret_on = true;      /* caret blink phase                    */

#define ANIM_MS   0.20            /* window animation duration, s         */
#define MENU_MS   0.15            /* dropdown animation duration, s       */

/* ---------------- desktop panel --------------------------------------- */
static void build_desktop(void)
{
    panel_h = (int)fmax(26.0, 28.0 * u);      /* slim macOS menu bar */
    if (panel_h > H / 4)
        panel_h = H / 4;
    SMc = (int)(22 * u);                      /* window shadow margin */
    DDMc = (int)(16 * u);                     /* dropdown shadow margin */
    AMDc = (int)(16 * u);                     /* About panel shadow margin */
    desktop = malloc((size_t)W * H * 4);
    if (!desktop)
        return;
    if (wall)
        memcpy(desktop, wall, (size_t)W * H * 4);
    else
        memset(desktop, 0, (size_t)W * H * 4);

    uint32_t *s1 = malloc((size_t)W * panel_h * 4);
    uint32_t *s2 = malloc((size_t)W * panel_h * 4);
    if (s1 && s2) {
        if (wall) {
            for (int y = 0; y < panel_h; y++)
                memcpy(s1 + (size_t)y * W, wall + (size_t)y * W, (size_t)W * 4);
        } else {
            for (int i = 0; i < W * panel_h; i++)
                s1[i] = rgb(200, 200, 205);   /* fallback strip */
        }
        box_blur_h(s2, s1, W, panel_h, 14);
        box_blur_v(s1, s2, W, panel_h, 3);
        /* frosted glass: lighten towards white (58%) */
        for (int i = 0; i < W * panel_h; i++) {
            uint32_t v = s1[i];
            unsigned r = ((v >> 16 & 255) * 107 + 255 * 148) >> 8;
            unsigned g = ((v >>  8 & 255) * 107 + 255 * 148) >> 8;
            unsigned b = ((v       & 255) * 107 + 255 * 148) >> 8;
            desktop[i] = rgb(r, g, b);
        }
    }
    free(s1);
    free(s2);
    /* hairline along the bottom edge of the panel */
    for (int x = 0; x < W; x++) {
        uint32_t v = desktop[(size_t)(panel_h - 1) * W + x];
        unsigned r = ((v >> 16 & 255) * 219) >> 8;
        unsigned g = ((v >>  8 & 255) * 219) >> 8;
        unsigned b = ((v       & 255) * 219) >> 8;
        desktop[(size_t)(panel_h - 1) * W + x] = rgb(r, g, b);
    }

    /* ---- dock: frosted dark-glass bar at the bottom, macOS-style ---- */
    dock_h = (int)(ICON_BASE * u) + (int)(16 * u);
    if (dock_h < 48)
        dock_h = 48;
    if (dock_h > H / 5)
        dock_h = H / 5;
    int dock_w = (int)(DOCK_W * u);
    if (dock_w > W - 60)
        dock_w = W - 60;
    if (dock_w < (int)(ICON_BASE * u) + (int)(24 * u))
        dock_w = (int)(ICON_BASE * u) + (int)(24 * u);
    int dock_r = dock_h * 30 / 100;
    ddx0 = (W - dock_w) / 2; ddx1 = ddx0 + dock_w - 1;
    ddy1 = H - DOCK_MARGIN_B - 1; ddy0 = ddy1 - dock_h + 1;

    const int BM = 20;                        /* blur margin around the dock */
    int bx0 = ddx0 - BM < 0 ? 0 : ddx0 - BM;
    int by0 = ddy0 - BM < 0 ? 0 : ddy0 - BM;
    int bx1 = ddx1 + BM > W - 1 ? W - 1 : ddx1 + BM;
    int by1 = ddy1 + BM > H - 1 ? H - 1 : ddy1 + BM;
    int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    uint32_t *s3 = malloc((size_t)bw * bh * 4);
    uint32_t *s4 = malloc((size_t)bw * bh * 4);
    if (s3 && s4) {
        if (wall) {
            for (int y = 0; y < bh; y++)
                memcpy(s3 + (size_t)y * bw,
                       wall + (size_t)(by0 + y) * W + bx0, (size_t)bw * 4);
        } else {
            for (int i = 0; i < bw * bh; i++)
                s3[i] = rgb(120, 120, 126);
        }
        box_blur_h(s4, s3, bw, bh, 10);
        box_blur_v(s3, s4, bw, bh, 5);
        for (int y = ddy0; y <= ddy1; y++) {
            for (int x = ddx0; x <= ddx1; x++) {
                double d = sd_round(x + 0.5, y + 0.5,
                                    ddx0, ddy0, ddx1, ddy1, dock_r);
                double cov = 0.5 - d;         /* rounded-rect coverage */
                if (cov <= 0)
                    continue;
                if (cov > 1)
                    cov = 1;
                uint32_t v = s3[(size_t)(y - by0) * bw + (x - bx0)];
                /* dark glass: 70% blurred wallpaper + 30% dark tint */
                unsigned r = ((v >> 16 & 255) * 178 + 20 * 78) >> 8;
                unsigned g = ((v >>  8 & 255) * 178 + 20 * 78) >> 8;
                unsigned b = ((v       & 255) * 178 + 22 * 78) >> 8;
                int A = (int)(cov * 255.0 + 0.5);
                uint32_t ob = desktop[(size_t)y * W + x];
                unsigned orr = ((ob >> 16 & 255) * (255 - A) + r * A) / 255;
                unsigned ogg = ((ob >>  8 & 255) * (255 - A) + g * A) / 255;
                unsigned obb = ((ob       & 255) * (255 - A) + b * A) / 255;
                desktop[(size_t)y * W + x] = rgb(orr, ogg, obb);
                /* subtle light border along the glass edge */
                double bcov = 0.5 - fabs(d);
                if (bcov > 0) {
                    if (bcov > 1)
                        bcov = 1;
                    int BA = (int)(bcov * 56 + 0.5);
                    uint32_t pb = desktop[(size_t)y * W + x];
                    unsigned pr = ((pb >> 16 & 255) * (255 - BA) + 255 * BA) / 255;
                    unsigned pg = ((pb >>  8 & 255) * (255 - BA) + 255 * BA) / 255;
                    unsigned pbb = ((pb       & 255) * (255 - BA) + 255 * BA) / 255;
                    desktop[(size_t)y * W + x] = rgb(pr, pg, pbb);
                }
            }
        }
    }
    free(s3);
    free(s4);

    /* ---- terminal window geometry + frosted backdrop ---- */
    ww = W * 44 / 100;
    wh = H * 54 / 100;
    if (ww > W - 80) ww = W - 80;
    if (wh > H - panel_h - dock_h - 60) wh = H - panel_h - dock_h - 60;
    wx = (W - ww) / 2;
    wy = panel_h + (int)(24 * u);
    tw_title = (int)(30 * u);
    if (tw_title < 26)
        tw_title = 26;
    /* maximized rect: full work area between panel and dock */
    mw_x0 = (int)(6 * u);
    mw_y0 = panel_h + 2;
    mw_x1 = W - 1 - (int)(6 * u);
    mw_y1 = ddy0 - (int)(10 * u);

    const int SM = (int)(44 * u);             /* blur margin around window */
    int kx0 = wx - SM < 0 ? 0 : wx - SM;
    int ky0 = wy - SM < 0 ? 0 : wy - SM;
    int kx1 = wx + ww + SM > W - 1 ? W - 1 : wx + ww + SM;
    int ky1 = wy + wh + SM > H - 1 ? H - 1 : wy + wh + SM;
    winblur_x0 = kx0; winblur_y0 = ky0;
    winblur_w = kx1 - kx0 + 1; winblur_h = ky1 - ky0 + 1;
    s3 = malloc((size_t)winblur_w * winblur_h * 4);
    s4 = malloc((size_t)winblur_w * winblur_h * 4);
    if (s3 && s4) {
        for (int y = 0; y < winblur_h; y++)
            memcpy(s3 + (size_t)y * winblur_w,
                   desktop + (size_t)(ky0 + y) * W + kx0, (size_t)winblur_w * 4);
        box_blur_h(s4, s3, winblur_w, winblur_h, 12);
        box_blur_v(s3, s4, winblur_w, winblur_h, 6);
        winblur = s3;
    } else {
        free(s3);
    }
    free(s4);

    winbuf = malloc((size_t)W * (size_t)(H - panel_h) * 4);
    menu_tex = malloc((size_t)((int)(262 * u)) * (size_t)((int)(76 * u)) * 4);
    about_tex = malloc((size_t)((int)(372 * u)) * (size_t)((int)(226 * u)) * 4);
    fprintf(stderr, "splash: desktop ready (panel %d, dock %dx%d, win %dx%d@%d,%d)\n",
            panel_h, ddx1 - ddx0 + 1, dock_h, ww, wh, wx, wy);
}

/* ---------------- kernel log ------------------------------------------ */
typedef struct {
    char text[LOG_LINE_MAX];
} KLine;

static KLine hist[HIST_MAX];
static int hist_n, reveal_n;
static int kfd = -1;

static void push_line(double ts, const char *msg)
{
    if (hist_n == HIST_MAX) {
        memmove(hist, hist + 1, sizeof(KLine) * (HIST_MAX - 1));
        hist_n--;
        if (reveal_n > 0)
            reveal_n--;
    }
    KLine *k = &hist[hist_n++];
    snprintf(k->text, LOG_LINE_MAX, "%8.3f %s", ts, msg);
}

static void handle_record(char *rec, ssize_t n)
{
    if (n <= 0)
        return;
    rec[n] = 0;
    for (ssize_t i = n - 1; i >= 0 && (rec[i] == '\n' || rec[i] == '\r'); i--)
        rec[i] = 0;
    char *semi = memchr(rec, ';', (size_t)n);
    if (!semi)
        return;
    *semi = 0;

    /* format: level,seq,timestamp[,flags[,caller]];message */
    double ts = 0;
    int c = 0;
    char *flags = NULL, *q = rec;
    while (*q) {
        if (*q == ',') {
            c++;
            if (c == 2) { char *e = NULL; ts = strtod(q + 1, &e) / 1e6; (void)e; }
            if (c == 3) { flags = q + 1; *q = 0; break; }
        }
        q++;
    }
    bool cont = flags && flags[0] == 'c' && (flags[1] == 0 || flags[1] == ',');
    char *msg = semi + 1;
    if (!*msg)
        return;
    /* grab the real kernel release from its genuine boot banner */
    if (!kver[0] && strncmp(msg, "Linux version ", 14) == 0) {
        const char *v = msg + 14;
        size_t k = 0;
        while (v[k] && v[k] != ' ' && k < sizeof(kver) - 1) {
            kver[k] = v[k];
            k++;
        }
        kver[k] = 0;
    }
    if (cont && hist_n > 0) {                 /* continuation of previous line */
        KLine *k = &hist[hist_n - 1];
        size_t len = strlen(k->text);
        snprintf(k->text + len, LOG_LINE_MAX - len, "%s", msg);
        return;
    }
    push_line(ts, msg);
}

static void pump_kmsg(void)
{
    char buf[2048];
    for (int i = 0; i < 64; i++) {
        ssize_t n = read(kfd, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;
        handle_record(buf, n);
    }
}

static void reveal(void)
{
    int k = 0;
    while (reveal_n < hist_n && k < REVEAL_PER_FRAME) {
        reveal_n++;
        k++;
    }
}

/* ---------------- rect helper ------------------------------------------- */
static bool in_rect(int x, int y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

/* ---------------- drawing primitives (clipped) ------------------------- */
/* glyph pixels carry 8-bit alpha coverage (anti-aliased TTF raster) */
static void draw_char_s(int x, int y, char ch, uint32_t col, int s)
{
    (void)s;
    if (ch < FONT_FIRST || ch > FONT_FIRST + FONT_COUNT - 1)
        return;
    const unsigned char (*gl)[FONT_W] =
        (const unsigned char (*)[FONT_W])FONT_BITMAP[ch - FONT_FIRST];
    int cr = col >> 16 & 255, cg = col >> 8 & 255, cb = col & 255;
    for (int ry = 0; ry < FONT_H; ry++) {
        int py = y + ry;
        if (py < cy0 || py > cy1)
            continue;
        const unsigned char *row = gl[ry];
        uint32_t *d = back + (size_t)py * W;
        for (int rx = 0; rx < FONT_W; rx++) {
            int a = row[rx];
            if (!a)
                continue;
            int px = x + rx;
            if (px < cx0 || px > cx1)
                continue;
            uint32_t ob = d[px];
            unsigned rr = ((ob >> 16 & 255) * (255 - a) + cr * a) / 255;
            unsigned gg = ((ob >>  8 & 255) * (255 - a) + cg * a) / 255;
            unsigned bb = ((ob       & 255) * (255 - a) + cb * a) / 255;
            d[px] = rgb(rr, gg, bb);
        }
    }
}

static int font_w_s(int s) { return FONT_W * s; }
static int font_h_s(int s) { return FONT_H * s; }

static void draw_text_s(int x, int y, const char *str, uint32_t col, int s)
{
    int maxc = (W - 2 * LOG_MARGIN) / font_w_s(s);
    for (int i = 0; str[i] && i < maxc; i++)
        draw_char_s(x + i * font_w_s(s), y, str[i], col, s);
}

/* ---- proportional UI font (Carlito, anti-aliased, variable width) ---- */
static int ui_adv(char ch, int bold)
{
    int i = (int)(unsigned char)ch - UIF_FIRST;
    if (i < 0 || i >= UIF_COUNT)
        return 6;
    return bold ? UIFB_ADV[i] : UIF_ADV[i];
}

static int ui_text_w(const char *s, int bold)
{
    int w = 0;
    for (; *s; s++)
        w += ui_adv(*s, bold);
    return w;
}

/* generic glyph blit into an arbitrary ARGB buffer with explicit clip */
static void ui_char_buf(uint32_t *buf, int stride, int qx0, int qy0,
                        int qx1, int qy1, int x, int y, char ch,
                        uint32_t col, int bold)
{
    if (ch < UIF_FIRST || ch > UIF_FIRST + UIF_COUNT - 1)
        return;
    int i = ch - UIF_FIRST;
    int gw = bold ? UIFB_GW[i] : UIF_GW[i];
    int gh = bold ? UIFB_GH[i] : UIF_GH[i];
    int ox = bold ? UIFB_OX[i] : UIF_OX[i];
    int oy = bold ? UIFB_OY[i] : UIF_OY[i];
    const unsigned char *bm = (bold ? UIFB_BM[i][0] : UIF_BM[i][0]);
    int cr = col >> 16 & 255, cg = col >> 8 & 255, cb = col & 255;
    for (int ry = 0; ry < gh; ry++) {
        int py = y + oy + ry;
        if (py < qy0 || py > qy1)
            continue;
        const unsigned char *row = bm + (size_t)ry * UIF_MAXW;
        uint32_t *d = buf + (size_t)py * stride;
        for (int rx = 0; rx < gw; rx++) {
            int a = row[rx];
            if (!a)
                continue;
            int px = x + ox + rx;
            if (px < qx0 || px > qx1)
                continue;
            uint32_t ob = d[px];
            unsigned rr = ((ob >> 16 & 255) * (255 - a) + cr * a) / 255;
            unsigned gg = ((ob >>  8 & 255) * (255 - a) + cg * a) / 255;
            unsigned bb = ((ob       & 255) * (255 - a) + cb * a) / 255;
            d[px] = rgb(rr, gg, bb);
        }
    }
}

/* draw into `back`, honoring the global clip rect */
static void ui_char(int x, int y, char ch, uint32_t col, int bold)
{
    ui_char_buf(back, W, cx0, cy0, cx1, cy1, x, y, ch, col, bold);
}

static void ui_text(int x, int y, const char *s, uint32_t col, int bold)
{
    for (; *s; s++) {
        ui_char(x, y, *s, col, bold);
        x += ui_adv(*s, bold);
    }
}

/* draw into an off-screen texture with explicit bounds */
static void ui_text_tex(uint32_t *tex, int tstride, int th_, int x, int y,
                        const char *s, uint32_t col, int bold)
{
    for (; *s; s++) {
        ui_char_buf(tex, tstride, 0, 0, tstride - 1, th_ - 1,
                    x, y, *s, col, bold);
        x += ui_adv(*s, bold);
    }
}

/* ---- real clock for the menu bar (right side, macOS-like) ---- */
static void clock_update(void)
{
    time_t rt = time(NULL);
    struct tm tmb;
    if (!localtime_r(&rt, &tmb))
        return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tmb.tm_hour, tmb.tm_min);
    if (!clk_valid || strcmp(buf, clk_str) != 0) {
        snprintf(clk_str, sizeof(clk_str), "%s", buf);
        clk_valid = true;
    }
}

/* anti-aliased rounded rect: fill (border=0) or 1px edge band */
static void paint_round(int x0, int y0, int x1, int y1, int r,
                        uint32_t col, int A, int border)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    if (x0 > x1 || y0 > y1)
        return;
    int cr = col >> 16 & 255, cg = col >> 8 & 255, cb = col & 255;
    for (int y = y0; y <= y1; y++) {
        if (y < cy0 || y > cy1)
            continue;
        for (int x = x0; x <= x1; x++) {
            if (x < cx0 || x > cx1)
                continue;
            double d = sd_round(x + 0.5, y + 0.5, x0, y0, x1, y1, r);
            double cov = border ? (0.5 - fabs(d)) * 0.6 : 0.5 - d;
            if (cov <= 0)
                continue;
            if (cov > 1)
                cov = 1;
            int a = (int)(cov * A + 0.5);
            if (a <= 0)
                continue;
            uint32_t ob = back[(size_t)y * W + x];
            unsigned rr = ((ob >> 16 & 255) * (255 - a) + cr * a) / 255;
            unsigned gg = ((ob >>  8 & 255) * (255 - a) + cg * a) / 255;
            unsigned bb = ((ob       & 255) * (255 - a) + cb * a) / 255;
            back[(size_t)y * W + x] = rgb(rr, gg, bb);
        }
    }
}

/* filled anti-aliased disc */
static void paint_disc(double ccx, double ccy, double rad, uint32_t col, int A)
{
    int x0 = (int)floor(ccx - rad - 1), x1 = (int)ceil(ccx + rad + 1);
    int y0 = (int)floor(ccy - rad - 1), y1 = (int)ceil(ccy + rad + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    int cr = col >> 16 & 255, cg = col >> 8 & 255, cb = col & 255;
    for (int y = y0; y <= y1; y++) {
        if (y < cy0 || y > cy1)
            continue;
        for (int x = x0; x <= x1; x++) {
            if (x < cx0 || x > cx1)
                continue;
            double dx = x + 0.5 - ccx, dy = y + 0.5 - ccy;
            double cov = 0.5 + rad - sqrt(dx * dx + dy * dy);
            if (cov <= 0)
                continue;
            if (cov > 1)
                cov = 1;
            int a = (int)(cov * A + 0.5);
            uint32_t ob = back[(size_t)y * W + x];
            unsigned rr = ((ob >> 16 & 255) * (255 - a) + cr * a) / 255;
            unsigned gg = ((ob >>  8 & 255) * (255 - a) + cg * a) / 255;
            unsigned bb = ((ob       & 255) * (255 - a) + cb * a) / 255;
            back[(size_t)y * W + x] = rgb(rr, gg, bb);
        }
    }
}

/* macOS-like white arc spinner with anti-aliasing and a fading tail */
static void draw_spinner(double ccx, double ccy, double R, double rot)
{
    double sw = fmax(2.5, R * 0.15);          /* stroke width  */
    double span = M_PI * 0.60;                /* arc length    */
    double track_a = 0.14;
    double lx = ccx + cos(rot) * R, ly = ccy + sin(rot) * R;
    int x0 = (int)floor(ccx - R - sw * 2), x1 = (int)ceil(ccx + R + sw * 2);
    int y0 = (int)floor(ccy - R - sw * 2), y1 = (int)ceil(ccy + R + sw * 2);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            double dx = px + 0.5 - ccx, dy = py + 0.5 - ccy;
            double dist = sqrt(dx * dx + dy * dy);
            double cov = 0.5 + sw * 0.5 - fabs(dist - R);   /* radial AA */
            if (cov <= 0)
                continue;
            if (cov > 1)
                cov = 1;
            double alpha = track_a * cov;
            double ang = atan2(dy, dx);
            double d = fmod(rot - ang, 2 * M_PI);
            if (d < 0)
                d += 2 * M_PI;
            if (d < span) {
                double f = 1.0 - d / span;
                f = f * f * (3 - 2 * f);      /* smooth fading tail */
                if (f * cov > alpha)
                    alpha = f * cov;
            }
            double cdx = px + 0.5 - lx, cdy = py + 0.5 - ly; /* leading cap */
            double ccov = 0.5 + sw * 0.5 - sqrt(cdx * cdx + cdy * cdy);
            if (ccov > 0) {
                if (ccov > 1)
                    ccov = 1;
                if (ccov > alpha)
                    alpha = ccov;
            }
            if (alpha <= 0)
                continue;
            uint32_t ob = back[(size_t)py * W + px];
            int A = (int)(alpha * 255.0 + 0.5);
            int r = ((ob >> 16 & 255) * (255 - A) + 255 * A) / 255;
            int g = ((ob >>  8 & 255) * (255 - A) + 255 * A) / 255;
            int b = ((ob       & 255) * (255 - A) + 255 * A) / 255;
            back[(size_t)py * W + px] = rgb(r, g, b);
        }
    }
}

/* ---------------- cursor ------------------------------------------------ */
/* macOS-style black arrow with a thin white outline, rasterized from a
 * polygon with 4x supersampling for perfectly smooth edges */
static unsigned char *cur_alpha_w, *cur_alpha_b;   /* outline / body       */
static int cur_w, cur_h;

static void cursor_init(void)
{
    const double P[][2] = {
        { 0.0, 0.0 }, { 0.0, 16.2 }, { 3.18, 13.1 }, { 5.33, 18.4 },
        { 7.11, 17.6 }, { 4.89, 12.4 }, { 9.33, 12.4 },
    };
    const int NP = (int)(sizeof(P) / sizeof(P[0]));
    const double SC = 1.55;
    cur_w = (int)ceil(9.33 * SC) + 2;
    cur_h = (int)ceil(18.4 * SC) + 2;
    cur_alpha_w = calloc((size_t)cur_w * cur_h, 1);
    cur_alpha_b = calloc((size_t)cur_w * cur_h, 1);
    const int SS = 4;
    for (int py = 0; py < cur_h; py++) {
        for (int px = 0; px < cur_w; px++) {
            int aw = 0, ab = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    double x = (px + (sx + 0.5) / SS - 1.0) / SC;
                    double y = (py + (sy + 0.5) / SS - 1.0) / SC;
                    int inside = 0;
                    for (int i = 0, j = NP - 1; i < NP; j = i++) {
                        if ((P[i][1] > y) != (P[j][1] > y) &&
                            x < (P[j][0] - P[i][0]) * (y - P[i][1]) /
                                (P[j][1] - P[i][1]) + P[i][0])
                            inside = !inside;
                    }
                    double d = 1e9;
                    for (int i = 0, j = NP - 1; i < NP; j = i++)
                        d = fmin(d, seg_dist(x, y, P[i][0], P[i][1],
                                             P[j][0], P[j][1]));
                    if (inside) {
                        ab++;
                        if (d * SC <= 1.35)
                            aw++;            /* outline melts into body */
                    } else if (d * SC <= 0.9) {
                        aw++;
                    }
                }
            }
            cur_alpha_w[(size_t)py * cur_w + px] =
                (unsigned char)(aw * 255 / (SS * SS));
            cur_alpha_b[(size_t)py * cur_w + px] =
                (unsigned char)(ab * 255 / (SS * SS));
        }
    }
}

static void draw_cursor(void)
{
    for (int r = 0; r < cur_h; r++) {
        int py = my + r;
        if (py < 0 || py >= H)
            continue;
        for (int c = 0; c < cur_w; c++) {
            int px = mx + c;
            if (px < 0 || px >= W)
                continue;
            int aw = cur_alpha_w[(size_t)r * cur_w + c];
            int ab = cur_alpha_b[(size_t)r * cur_w + c];
            if (!aw && !ab)
                continue;
            uint32_t ob = back[(size_t)py * W + px];
            unsigned rr = ob >> 16 & 255, gg = ob >> 8 & 255, bb = ob & 255;
            if (aw) {                          /* white outline */
                rr = (rr * (255 - aw) + 255 * aw) / 255;
                gg = (gg * (255 - aw) + 255 * aw) / 255;
                bb = (bb * (255 - aw) + 255 * aw) / 255;
            }
            if (ab) {                          /* black body on top */
                rr = (rr * (255 - ab)) / 255;
                gg = (gg * (255 - ab)) / 255;
                bb = (bb * (255 - ab)) / 255;
            }
            back[(size_t)py * W + px] = rgb(rr, gg, bb);
        }
    }
}

/* live FPS readout, top-right corner (inside the panel on the desktop) */
static void fps_bbox(int *bx0, int *by0, int *bx1, int *by1)
{
    int tw = ui_text_w("999 FPS", 0) + 14 + ui_text_w("23:59", 0);
    *bx1 = W - 12 + 4;
    *bx0 = *bx1 - tw - 8;
    *by0 = (panel_h - UIF_CELLH) / 2 - 2;
    if (*by0 < 0) *by0 = 0;
    *by1 = panel_h - 1;
}

static void draw_fps(int dark)
{
    char buf[32];
    if (!fps_ready)
        return;                               /* no completed window yet */
    snprintf(buf, sizeof(buf), "%d FPS", fps_now);
    int x = W - 12 - ui_text_w("23:59", 0) - 14 - ui_text_w(buf, 0);
    int y = (panel_h - UIF_CELLH) / 2;
    if (y < 2)
        y = 2;
    ui_text(x, y, buf, dark ? rgb(60, 60, 67) : rgb(168, 170, 176), 0);
}

static void draw_clock(int dark)
{
    if (!clk_valid)
        return;
    int x = W - 12 - ui_text_w(clk_str, 0);
    int y = (panel_h - UIF_CELLH) / 2;
    if (y < 2)
        y = 2;
    ui_text(x, y, clk_str, dark ? rgb(60, 60, 67) : rgb(168, 170, 176), 0);
}

/* ---------------- menu bar --------------------------------------------- */
static void menu_geometry(void)
{
    int drop_s = (int)fmax(13.0, 15.0 * u);
    dr_x0 = MENU_X;
    dr_y0 = (panel_h - drop_s) / 2;
    dr_x1 = dr_x0 + drop_s - 1;
    dr_y1 = dr_y0 + drop_s - 1;
    set_x = dr_x1 + 1 + (int)(11 * u);
    int tww = ui_text_w("Settings", 1);
    ti_x0 = set_x - (int)(8 * u);
    ti_y0 = (int)(3 * u);
    ti_x1 = set_x + tww + 4 + 9 + (int)(7 * u);   /* text + gap + chevron */
    ti_y1 = panel_h - 1 - (int)(3 * u);
    /* the dropdown hangs below the item it belongs to (macOS) */
    dd_x0 = ti_x0;
    dd_y0 = panel_h + (int)(3 * u);
    int mw = ui_text_w("About AquaOS", 0) + (int)(40 * u);
    if (mw < (int)(150 * u))
        mw = (int)(150 * u);
    dd_x1 = dd_x0 + mw;
    it_x0 = dd_x0 + (int)(5 * u);
    it_y0 = dd_y0 + (int)(5 * u);
    it_x1 = dd_x1 - (int)(5 * u);
    it_y1 = it_y0 + (int)(26 * u);
    dd_y1 = it_y1 + (int)(5 * u);
}

/* draw an arbitrary RGBA icon (premultiplied-less, straight alpha) */
static void draw_icon_rgba(const unsigned char *data, int iw, int ih,
                           int x, int y, int size);

/* alpha-aware texture blitters (defined further below) */
static void tex_blit_alpha(const uint32_t *tex, int tx, int ty, int tw_,
                           int th_, int galpha);

/* small anti-aliased chevron pointing down (~9x6 px) */
static void draw_chevron(int x, int y, uint32_t col)
{
    const int SS = 3;
    double th = 0.95;                          /* half stroke width      */
    for (int py = 0; py < 7; py++) {
        int yy = y + py;
        if (yy < cy0 || yy > cy1)
            continue;
        for (int px = 0; px < 10; px++) {
            int xx = x + px;
            if (xx < cx0 || xx > cx1)
                continue;
            int cov = 0;
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    double fx = px + (sx + 0.5) / SS;
                    double fy = py + (sy + 0.5) / SS;
                    double d1 = seg_dist(fx, fy, 0.4, 0.6, 4.5, 5.2);
                    double d2 = seg_dist(fx, fy, 4.5, 5.2, 8.6, 0.6);
                    if (fmin(d1, d2) <= th)
                        cov++;
                }
            if (!cov)
                continue;
            int a = cov * 255 / (SS * SS);
            uint32_t ob = back[(size_t)yy * W + xx];
            unsigned rr = ((ob >> 16 & 255) * (255 - a) + (col >> 16 & 255) * a) / 255;
            unsigned gg = ((ob >>  8 & 255) * (255 - a) + (col >>  8 & 255) * a) / 255;
            unsigned bb = ((ob       & 255) * (255 - a) + (col       & 255) * a) / 255;
            back[(size_t)yy * W + xx] = rgb(rr, gg, bb);
        }
    }
}

/* ---- frosted macOS panels (dropdown / About), baked with soft shadow ----
 * texel format: ARGB, alpha in the top byte; shadow texels are pure alpha */
static void bake_panel_tex(uint32_t *tex, int *ptw, int *pth,
                           int px0, int py0, int px1, int py1,
                           int M, double rad, double sigma, double amax)
{
    int pw = px1 - px0 + 1, ph = py1 - py0 + 1;
    int tw_ = pw + 2 * M, th_ = ph + 2 * M;
    *ptw = tw_;
    *pth = th_;
    memset(tex, 0, (size_t)tw_ * th_ * 4);

    /* blurred backdrop for the frosted look */
    int bx0 = px0 - M < 0 ? 0 : px0 - M;
    int by0 = py0 - M < 0 ? 0 : py0 - M;
    int bx1 = px1 + M > W - 1 ? W - 1 : px1 + M;
    int by1 = py1 + M > H - 1 ? H - 1 : py1 + M;
    int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    uint32_t *s1 = NULL, *s2 = NULL;
    bool have_blur = false;
    if (bw > 0 && bh > 0) {
        s1 = malloc((size_t)bw * bh * 4);
        s2 = malloc((size_t)bw * bh * 4);
        if (s1 && s2) {
            for (int y = 0; y < bh; y++)
                memcpy(s1 + (size_t)y * bw,
                       desktop + (size_t)(by0 + y) * W + bx0, (size_t)bw * 4);
            box_blur_h(s2, s1, bw, bh, 4);
            box_blur_v(s1, s2, bw, bh, 2);
            have_blur = true;
        }
    }

    for (int y = 0; y < th_; y++) {
        uint32_t *d = tex + (size_t)y * tw_;
        for (int x = 0; x < tw_; x++) {
            double dS = sd_round(x + 0.5, y + 0.5, M, M, M + pw - 1,
                                 M + ph - 1, rad);
            if (dS <= 0.5) {                  /* panel body (AA edge) */
                double cov = 0.5 - dS;
                if (cov > 1)
                    cov = 1;
                unsigned r = 246, g = 246, b = 248;
                if (have_blur) {
                    int sx = px0 - M + x - bx0;
                    int sy = py0 - M + y - by0;
                    if (sx < 0) sx = 0;
                    if (sy < 0) sy = 0;
                    if (sx >= bw) sx = bw - 1;
                    if (sy >= bh) sy = bh - 1;
                    uint32_t v = s1[(size_t)sy * bw + sx];
                    r = (unsigned)(((v >> 16 & 255) * 56 + 246 * 200) >> 8);
                    g = (unsigned)(((v >>  8 & 255) * 56 + 246 * 200) >> 8);
                    b = (unsigned)(((v       & 255) * 59 + 248 * 197) >> 8);
                }
                double bcov = 0.5 - fabs(dS); /* hairline border */
                if (bcov > 0) {
                    if (bcov > 1)
                        bcov = 1;
                    int BA = (int)(bcov * 44 + 0.5);
                    r = (unsigned)((r * (255 - BA)) / 255);
                    g = (unsigned)((g * (255 - BA)) / 255);
                    b = (unsigned)((b * (255 - BA)) / 255);
                }
                int A = (int)(cov * 255.0 + 0.5);
                d[x] = ((uint32_t)A << 24) | (r << 16) | (g << 8) | b;
            } else if (dS <= M) {             /* soft neutral shadow */
                double a = amax * exp(-(dS - 0.5) / sigma);
                int A = (int)(a + 0.5);
                if (A > 255)
                    A = 255;
                if (A >= 2)
                    d[x] = (uint32_t)A << 24;
            }
        }
    }
    free(s1);
    free(s2);
}

static void render_menu_tex(void)
{
    if (!menu_tex || !desktop)
        return;
    bake_panel_tex(menu_tex, &menu_tex_w, &menu_tex_h,
                   dd_x0, dd_y0, dd_x1, dd_y1, DDMc, 8.0 * u, 8.0 * u, 70.0);
}

static void render_about_tex(void)
{
    if (!about_tex || !desktop)
        return;
    char ln_kernel[80], ln_fb[80], ln_up[80], ln_fps[80];
    int n = 0;
    const char *lines[6];
    lines[n++] = "Version 1.6 (AquaOS desktop)";
    if (kver[0]) {
        snprintf(ln_kernel, sizeof(ln_kernel), "Kernel %s", kver);
        lines[n++] = ln_kernel;
    }
    snprintf(ln_fb, sizeof(ln_fb), "Framebuffer %dx%d @ %d bpp", W, H, BPP * 8);
    lines[n++] = ln_fb;
    double up = now();
    snprintf(ln_up, sizeof(ln_up), "Uptime %d:%02d:%02d",
             (int)up / 3600, ((int)up / 60) % 60, (int)up % 60);
    lines[n++] = ln_up;
    if (fps_ready) {
        snprintf(ln_fps, sizeof(ln_fps), "Compositor %d FPS", fps_now);
        lines[n++] = ln_fps;
    }
    int wmax = ui_text_w("AquaOS", 1);
    for (int i = 0; i < n; i++) {
        int w = ui_text_w(lines[i], 0);
        if (w > wmax)
            wmax = w;
    }
    int pw = wmax + (int)(44 * u);
    int ph = (int)(16 * u) + UIF_CELLH + (int)(7 * u) +
             n * (UIF_CELLH + (int)(5 * u)) + (int)(15 * u);
    if (pw > (int)(330 * u))
        pw = (int)(330 * u);
    if (ph > (int)(188 * u))
        ph = (int)(188 * u);
    ab_x0 = (W - pw) / 2;
    ab_x1 = ab_x0 + pw - 1;
    ab_y0 = (H - ph) * 2 / 5;
    ab_y1 = ab_y0 + ph - 1;
    bake_panel_tex(about_tex, &about_tex_w, &about_tex_h,
                   ab_x0, ab_y0, ab_x1, ab_y1, AMDc, 10.0 * u, 8.0 * u, 80.0);
    int tx = AMDc + (int)(22 * u);
    int ty = AMDc + (int)(16 * u);
    ui_text_tex(about_tex, about_tex_w, about_tex_h, tx, ty, "AquaOS",
                rgb(20, 20, 24), 1);
    ty += UIF_CELLH + (int)(7 * u);
    for (int i = 0; i < n; i++) {
        ui_text_tex(about_tex, about_tex_w, about_tex_h, tx, ty, lines[i],
                    rgb(60, 60, 66), 0);
        ty += UIF_CELLH + (int)(5 * u);
    }
}

static void draw_about(void)
{
    if (!about_open || !about_tex)
        return;
    tex_blit_alpha(about_tex, ab_x0 - AMDc, ab_y0 - AMDc,
                   about_tex_w, about_tex_h, 256);
}

static void draw_menu_animated(void)
{
    /* droplet logo at the left (clickable, never highlighted) */
    draw_icon_rgba(DROP_ICON_DATA, DROP_ICON_W, DROP_ICON_H,
                   dr_x0, dr_y0, dr_x1 - dr_x0 + 1);
    bool hov = in_rect(mx, my, ti_x0, ti_y0, ti_x1, ti_y1);
    if (menu_open)
        paint_round(ti_x0, ti_y0, ti_x1, ti_y1, 5, rgb(10, 122, 255), 256, 0);
    else if (hov)
        paint_round(ti_x0, ti_y0, ti_x1, ti_y1, 5, rgb(0, 0, 0), 30, 0);
    int ty = (panel_h - UIF_CELLH) / 2;
    if (ty < 2)
        ty = 2;
    ui_text(set_x, ty, "Settings",
            menu_open ? rgb(255, 255, 255) : rgb(28, 28, 30), 1);
    draw_chevron(set_x + ui_text_w("Settings", 1) + 4, (panel_h - 7) / 2,
                 menu_open ? rgb(255, 255, 255) : rgb(70, 70, 75));
    if (menu_a <= 0.003)
        return;

    /* eased open state: slide + fade, texture includes the soft shadow */
    double e = menu_a;
    e = e * e * (3 - 2 * e);                  /* smoothstep */
    int oy = (int)((1.0 - e) * -10 * u);
    int alpha = (int)(e * 256);
    if (alpha > 256)
        alpha = 256;
    tex_blit_alpha(menu_tex, dd_x0 - DDMc, dd_y0 - DDMc + oy,
                   menu_tex_w, menu_tex_h, alpha);
    int iy0 = it_y0 + oy, iy1 = it_y1 + oy;
    bool ah = in_rect(mx, my, it_x0, iy0, it_x1, iy1);
    if (ah && alpha > 60)
        paint_round(it_x0, iy0, it_x1, iy1, 5, rgb(10, 122, 255), alpha, 0);
    /* text only while visibly open: ui_text has no per-call alpha, so
     * drawing it during the fade-out tail would leave a ghost behind */
    if (alpha > 150) {
        int lty = iy0 + ((iy1 - iy0) - UIF_CELLH) / 2;
        ui_text(it_x0 + (int)(10 * u), lty, "About AquaOS",
                ah ? rgb(255, 255, 255) : rgb(28, 28, 30), 0);
    }
}

/* ---------------- dock icons -------------------------------------------- */
/* draw an RGBA icon with bilinear sampling and alpha blending (smooth at
 * any scale, no ragged nearest-neighbour edges) */
static void draw_icon_rgba(const unsigned char *data, int iw, int ih,
                           int x, int y, int size)
{
    if (size <= 0 || !data || iw < 1 || ih < 1)
        return;
    int x0 = x < cx0 ? cx0 : x;
    int y0 = y < cy0 ? cy0 : y;
    int x1 = x + size - 1 > cx1 ? cx1 : x + size - 1;
    int y1 = y + size - 1 > cy1 ? cy1 : y + size - 1;
    for (int py = y0; py <= y1; py++) {
        double fy = ((double)(py - y) + 0.5) * ih / size - 0.5;
        if (fy < 0) fy = 0;
        if (fy > ih - 1) fy = ih - 1;
        int iy = (int)fy;
        int iy1_ = iy + 1 < ih ? iy + 1 : iy;
        double ty_ = iy1_ == iy ? 0 : fy - iy;
        const unsigned char *r0 = data + (size_t)iy * iw * 4;
        const unsigned char *r1 = data + (size_t)iy1_ * iw * 4;
        uint32_t *d = back + (size_t)py * W;
        for (int px = x0; px <= x1; px++) {
            double fx = ((double)(px - x) + 0.5) * iw / size - 0.5;
            if (fx < 0) fx = 0;
            if (fx > iw - 1) fx = iw - 1;
            int ix = (int)fx;
            int ix1_ = ix + 1 < iw ? ix + 1 : ix;
            double tx_ = ix1_ == ix ? 0 : fx - ix;
            const unsigned char *p00 = r0 + (size_t)ix * 4;
            const unsigned char *p01 = r0 + (size_t)ix1_ * 4;
            const unsigned char *p10 = r1 + (size_t)ix * 4;
            const unsigned char *p11 = r1 + (size_t)ix1_ * 4;
            unsigned v[4];
            for (int c = 0; c < 4; c++) {
                double top = p00[c] * (1 - tx_) + p01[c] * tx_;
                double bot = p10[c] * (1 - tx_) + p11[c] * tx_;
                v[c] = (unsigned)(top * (1 - ty_) + bot * ty_ + 0.5);
            }
            unsigned a = v[3];
            if (!a)
                continue;
            if (a > 247)
                a = 255;
            uint32_t ob = d[px];
            unsigned orr = ((ob >> 16 & 255) * (255 - a) + v[0] * a) / 255;
            unsigned ogg = ((ob >>  8 & 255) * (255 - a) + v[1] * a) / 255;
            unsigned obb = ((ob       & 255) * (255 - a) + v[2] * a) / 255;
            d[px] = rgb(orr, ogg, obb);
        }
    }
}

/* dock icon geometry for a given magnification */
static void icon_rect(double s, int *ix0, int *iy0, int *ix1, int *iy1)
{
    int size = (int)(ICON_BASE * u * s);
    int cx = (ddx0 + ddx1) / 2;
    int bottom = ddy1 - (int)(8 * u);
    *ix0 = cx - size / 2;
    *ix1 = *ix0 + size - 1;
    *iy1 = bottom;
    *iy0 = bottom - size + 1;
}

static void draw_dock_icon(void)
{
    int ix0, iy0, ix1, iy1;
    icon_rect(icon_s, &ix0, &iy0, &ix1, &iy1);
    draw_icon_rgba(TERMINAL_ICON_DATA, TERMINAL_ICON_W, TERMINAL_ICON_H,
                   ix0, iy0, ix1 - ix0 + 1);
    /* running dot under the icon while the app is open (macOS-like) */
    bool running = tm != TM_CLOSED;
    if (running) {
        int dcx = (ddx0 + ddx1) / 2;
        int dy = ddy1 - (int)(4 * u);
        paint_disc(dcx + 0.5, dy + 0.5, fmax(2.0, 2.5 * u),
                   rgb(255, 255, 255), 165);
    }
}

static void dock_update(double dt)
{
    /* magnification: grows when the cursor is near the dock (macOS-like) */
    bool near = in_desktop && my > ddy0 - (int)(ICON_BASE * u * 1.6) &&
                mx > ddx0 - (int)(ICON_BASE * u) && mx < ddx1 + (int)(ICON_BASE * u);
    double target = near ? 1.45 : 1.0;
    double k = 1.0 - exp(-dt * 14.0);
    icon_s += (target - icon_s) * k;
    if (fabs(icon_s - 1.0) < 0.004 && target == 1.0)
        icon_s = 1.0;
    if (fabs(icon_s - 1.0) < 0.004 && target == 1.45)
        icon_s = 1.45;
}

/* ---------------- terminal emulator ------------------------------------- */
static uint8_t tch[TCOLS_MAX * TROWS_MAX];
static uint8_t tfg[TCOLS_MAX * TROWS_MAX];
static uint8_t tbo[TCOLS_MAX * TROWS_MAX];
static int t_cols, t_rows;        /* live grid size                        */
static int tcx, tcy;              /* cursor cell                           */
static int tfcol = 255;           /* current fg color index, 255 = default */
static bool tfbold = false;
static int ansi_st = 0;           /* 0 ground, 1 esc, 2 csi, 3 osc         */
static char csi_buf[48];
static int csi_n;
static bool term_dirty = false;
static pid_t sh_pid = -1;
static int sh_fd = -1;
static bool kbd_ready = false;

/* macOS Terminal "Pro"-like palette (8 basic + 8 bright) */
static uint32_t PAL[16] = {
    /*  0 black   */ 0, /* set at runtime via rgb() */
};

static void pal_init(void)
{
    PAL[0]  = rgb(60, 60, 64);
    PAL[1]  = rgb(255, 98, 90);
    PAL[2]  = rgb(80, 210, 90);
    PAL[3]  = rgb(255, 200, 60);
    PAL[4]  = rgb(90, 150, 255);
    PAL[5]  = rgb(255, 100, 220);
    PAL[6]  = rgb(70, 200, 230);
    PAL[7]  = rgb(210, 210, 214);
    PAL[8]  = rgb(120, 120, 126);
    PAL[9]  = rgb(255, 130, 120);
    PAL[10] = rgb(120, 240, 130);
    PAL[11] = rgb(255, 220, 110);
    PAL[12] = rgb(130, 175, 255);
    PAL[13] = rgb(255, 140, 235);
    PAL[14] = rgb(115, 220, 245);
    PAL[15] = rgb(240, 240, 244);
}
#define TERM_FG_DEFAULT rgb(235, 235, 238)

static uint32_t cell_color(int i)
{
    int f = tfg[i];
    if (f == 255)
        return TERM_FG_DEFAULT;
    if (tbo[i] && f < 8)
        return PAL[f + 8];
    return PAL[f];
}

static void grid_clear_row(int r)
{
    memset(tch + (size_t)r * t_cols, 0, (size_t)t_cols);
    memset(tfg + (size_t)r * t_cols, 255, (size_t)t_cols);
    memset(tbo + (size_t)r * t_cols, 0, (size_t)t_cols);
}

static void grid_scroll(void)
{
    memmove(tch, tch + t_cols, (size_t)(t_rows - 1) * t_cols);
    memmove(tfg, tfg + t_cols, (size_t)(t_rows - 1) * t_cols);
    memmove(tbo, tbo + t_cols, (size_t)(t_rows - 1) * t_cols);
    grid_clear_row(t_rows - 1);
}

static void grid_newline(void)
{
    tcx = 0;
    tcy++;
    if (tcy >= t_rows) {
        tcy = t_rows - 1;
        grid_scroll();
    }
}

static void term_reset(void)
{
    memset(tch, 0, sizeof(tch));
    memset(tfg, 255, sizeof(tfg));
    memset(tbo, 0, sizeof(tbo));
    tcx = tcy = 0;
    tfcol = 255;
    tfbold = false;
    ansi_st = 0;
    csi_n = 0;
}

static int csi_param(int idx, int defv)
{
    /* parse idx-th (0-based) ';'-separated parameter from csi_buf */
    int seen = 0, tn = 0;
    char tmp[16];
    for (int p = 0; p <= csi_n; p++) {
        char c = p < csi_n ? csi_buf[p] : ';';
        if (c >= '0' && c <= '9') {
            if (tn < 15)
                tmp[tn++] = c;
        } else {
            tmp[tn] = 0;
            if (seen == idx)
                return tn ? atoi(tmp) : defv;
            seen++;
            tn = 0;
        }
    }
    return defv;
}

static void sgr(void)
{
    /* parse full parameter list for SGR */
    int vals[16];
    int n = 0;
    char tmp[16];
    int tn = 0;
    for (int p = 0; p <= csi_n && n < 16; p++) {
        char c = p < csi_n ? csi_buf[p] : ';';
        if (c >= '0' && c <= '9' && tn < 15) {
            tmp[tn++] = c;
        } else {
            tmp[tn] = 0;
            vals[n++] = tn ? atoi(tmp) : 0;
            tn = 0;
        }
    }
    if (n == 0)
        vals[n++] = 0;
    for (int i = 0; i < n; i++) {
        int v = vals[i];
        if (v == 0) { tfcol = 255; tfbold = false; }
        else if (v == 1) tfbold = true;
        else if (v == 22) tfbold = false;
        else if (v >= 30 && v <= 37) tfcol = v - 30;
        else if (v == 39) tfcol = 255;
        else if (v >= 90 && v <= 97) tfcol = v - 90 + 8;
    }
}

static void term_feed(char c)
{
    if (ansi_st == 3) {                       /* OSC: swallow until BEL/ST */
        if (c == 0x07)
            ansi_st = 0;
        else if (c == 0x1B)
            ansi_st = 4;                      /* expect ST (\\) */
        return;
    }
    if (ansi_st == 4) {
        if (c == '\\')
            ansi_st = 0;
        else if (c != 0x1B)
            ansi_st = 3;
        return;
    }
    if (ansi_st == 1) {                       /* after ESC */
        if (c == '[') {
            ansi_st = 2;
            csi_n = 0;
        } else if (c == ']') {
            ansi_st = 3;
        } else if (c == 'c') {
            term_reset();
        } else {
            ansi_st = 0;
        }
        return;
    }
    if (ansi_st == 2) {                       /* CSI */
        if ((c >= '0' && c <= '9') || c == ';' || c == '?' || c == '!') {
            if (csi_n < (int)sizeof(csi_buf))
                csi_buf[csi_n++] = c;
            return;
        }
        ansi_st = 0;
        if (c == 'm') { sgr(); return; }
        if (c == 'A') { tcy -= csi_param(0, 1); if (tcy < 0) tcy = 0; return; }
        if (c == 'B') { tcy += csi_param(0, 1); if (tcy >= t_rows) tcy = t_rows - 1; return; }
        if (c == 'C') { tcx += csi_param(0, 1); if (tcx >= t_cols) tcx = t_cols - 1; return; }
        if (c == 'D') { tcx -= csi_param(0, 1); if (tcx < 0) tcx = 0; return; }
        if (c == 'E') { grid_newline(); return; }
        if (c == 'G') { int x = csi_param(0, 1) - 1; tcx = x < 0 ? 0 : (x >= t_cols ? t_cols - 1 : x); return; }
        if (c == 'H' || c == 'f') {
            int ry = csi_param(0, 1) - 1, rx = csi_param(1, 1) - 1;
            tcy = ry < 0 ? 0 : (ry >= t_rows ? t_rows - 1 : ry);
            tcx = rx < 0 ? 0 : (rx >= t_cols ? t_cols - 1 : rx);
            return;
        }
        if (c == 'J') {
            int m = csi_param(0, 0);
            if (m == 2 || m == 3) {
                term_reset();
            } else if (m == 0) {
                memset(tch + (size_t)tcy * t_cols + tcx, 0, (size_t)(t_cols - tcx));
                memset(tfg + (size_t)tcy * t_cols + tcx, 255, (size_t)(t_cols - tcx));
                memset(tbo + (size_t)tcy * t_cols + tcx, 0, (size_t)(t_cols - tcx));
                for (int r = tcy + 1; r < t_rows; r++)
                    grid_clear_row(r);
            } else if (m == 1) {
                memset(tch + (size_t)tcy * t_cols, 0, (size_t)(tcx + 1));
                memset(tfg + (size_t)tcy * t_cols, 255, (size_t)(tcx + 1));
                memset(tbo + (size_t)tcy * t_cols, 0, (size_t)(tcx + 1));
                for (int r = 0; r < tcy; r++)
                    grid_clear_row(r);
            }
            return;
        }
        if (c == 'K') {
            int m = csi_param(0, 0);
            if (m == 0) {
                memset(tch + (size_t)tcy * t_cols + tcx, 0, (size_t)(t_cols - tcx));
                memset(tfg + (size_t)tcy * t_cols + tcx, 255, (size_t)(t_cols - tcx));
                memset(tbo + (size_t)tcy * t_cols + tcx, 0, (size_t)(t_cols - tcx));
            } else if (m == 1) {
                memset(tch + (size_t)tcy * t_cols, 0, (size_t)(tcx + 1));
                memset(tfg + (size_t)tcy * t_cols, 255, (size_t)(tcx + 1));
                memset(tbo + (size_t)tcy * t_cols, 0, (size_t)(tcx + 1));
            } else {
                grid_clear_row(tcy);
            }
            return;
        }
        return;                               /* other final bytes ignored */
    }

    /* ground state */
    if (c >= 32 && c < 127) {
        size_t i = (size_t)tcy * t_cols + tcx;
        tch[i] = (uint8_t)c;
        tfg[i] = (uint8_t)tfcol;
        tbo[i] = tfbold;
        tcx++;
        if (tcx >= t_cols)
            grid_newline();
        return;
    }
    switch (c) {
    case '\r': tcx = 0; return;
    case '\n': grid_newline(); return;
    case '\b': if (tcx > 0) tcx--; return;
    case '\t': tcx = (tcx / 8 + 1) * 8; if (tcx >= t_cols) grid_newline(); return;
    case 0x07: return;
    case 0x1B: ansi_st = 1; return;
    default: return;                          /* other control bytes */
    }
}

/* ---- pty / shell ------------------------------------------------------- */
static void term_spawn(void)
{
    term_reset();
    t_cols = (ww - (int)(16 * u)) / FONT_W;
    t_rows = (wh - tw_title - (int)(12 * u)) / FONT_H;
    if (t_cols > TCOLS_MAX) t_cols = TCOLS_MAX;
    if (t_rows > TROWS_MAX) t_rows = TROWS_MAX;
    if (t_cols < 20) t_cols = 20;
    if (t_rows < 6) t_rows = 6;

    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0)
        return;
    if (grantpt(m) != 0 || unlockpt(m) != 0) {
        close(m);
        return;
    }
    char sn[64];
    snprintf(sn, sizeof(sn), "%s", ptsname(m));

    pid_t p = fork();
    if (p < 0) {
        close(m);
        return;
    }
    if (p == 0) {
        setsid();
        int s = open(sn, O_RDWR);
        if (s < 0)
            _exit(127);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2)
            close(s);
        close(m);
        struct winsize ws;
        ws.ws_col = (unsigned short)t_cols;
        ws.ws_row = (unsigned short)t_rows;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        ioctl(s, TIOCSWINSZ, &ws);
        setenv("TERM", "xterm", 1);
        setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
        setenv("HOME", "/root", 1);
        setenv("PWD", "/", 1);
        setenv("PS1", "aquaos:~$ ", 1);
        execl("/bin/sh", "sh", (char *)0);
        _exit(127);
    }
    sh_pid = p;
    sh_fd = m;
    fcntl(m, F_SETFL, fcntl(m, F_GETFL, 0) | O_NONBLOCK);
    tm_dead = false;
    term_dirty = true;
}

static void term_kill(void)
{
    if (sh_fd >= 0) {
        close(sh_fd);
        sh_fd = -1;
    }
    if (sh_pid > 0) {
        kill(sh_pid, SIGHUP);
        sh_pid = -1;
    }
}

static void term_poll(void)
{
    if (sh_fd < 0)
        return;
    uint8_t buf[4096];
    for (int i = 0; i < 8; i++) {
        ssize_t n = read(sh_fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        term_dirty = true;
        for (ssize_t k = 0; k < n; k++)
            term_feed((char)buf[k]);
    }
    if (sh_pid > 0 && waitpid(sh_pid, NULL, WNOHANG) == sh_pid) {
        sh_pid = -1;
        tm_dead = true;
        term_dirty = true;
        const char *msg = "\r\n[Process completed]";
        for (const char *q = msg; *q; q++)
            term_feed(*q);
    }
}

static void term_write(const uint8_t *b, int n)
{
    if (sh_fd < 0)
        return;
    ssize_t w = write(sh_fd, b, (size_t)n);
    (void)w;                                  /* EAGAIN: keystroke dropped */
}

/* ---- keyboard (evdev) --------------------------------------------------- */
static int kbd_fd = -1;
static double kbd_try = 0;
static bool kshift = false, kctrl = false, kcaps = false;
static uint8_t keyq[64];
static int keyq_n = 0;

static void key_push(const uint8_t *b, int n)
{
    for (int i = 0; i < n && keyq_n < (int)sizeof(keyq); i++)
        keyq[keyq_n++] = b[i];
}

/* map an evdev key code to terminal bytes; returns length 0..3 */
static int map_key(uint16_t code, const uint8_t **out)
{
    static uint8_t tmp[3];
    bool sh = kshift;
    bool up = false;

    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) return 0;
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) return 0;
    if (code == KEY_CAPSLOCK) return 0;
    if (code == KEY_LEFTALT || code == KEY_RIGHTALT) return 0;

    if (code >= KEY_F1 && code <= KEY_F12) return 0;

    switch (code) {
    case KEY_ENTER:    tmp[0] = '\r'; *out = tmp; return 1;
    case KEY_BACKSPACE:tmp[0] = 0x7f; *out = tmp; return 1;
    case KEY_TAB:      tmp[0] = '\t'; *out = tmp; return 1;
    case KEY_ESC:      tmp[0] = 0x1b; *out = tmp; return 1;
    case KEY_SPACE:    tmp[0] = ' ';  *out = tmp; return 1;
    case KEY_UP:       tmp[0]=0x1b; tmp[1]='['; tmp[2]='A'; *out=tmp; return 3;
    case KEY_DOWN:     tmp[0]=0x1b; tmp[1]='['; tmp[2]='B'; *out=tmp; return 3;
    case KEY_RIGHT:    tmp[0]=0x1b; tmp[1]='['; tmp[2]='C'; *out=tmp; return 3;
    case KEY_LEFT:     tmp[0]=0x1b; tmp[1]='['; tmp[2]='D'; *out=tmp; return 3;
    case KEY_DELETE:   tmp[0] = 0x7f; *out = tmp; return 1;
    case KEY_HOME:     tmp[0]=0x1b; tmp[1]='['; tmp[2]='H'; *out=tmp; return 3;
    case KEY_END:      tmp[0]=0x1b; tmp[1]='['; tmp[2]='F'; *out=tmp; return 3;
    default: break;
    }

    char c = 0;
    if (code >= KEY_1 && code <= KEY_0) {
        static const char ns[] = "1234567890";
        static const char ss[] = "!@#$%^&*()";
        c = sh ? ss[code - KEY_1] : ns[code - KEY_1];
    } else if (code >= KEY_Q && code <= KEY_P) {
        static const char ls[] = "qwertyuiop";
        c = ls[code - KEY_Q];
        up = true;
    } else if (code >= KEY_A && code <= KEY_L) {
        static const char ls[] = "asdfghjkl";
        c = ls[code - KEY_A];
        up = true;
    } else if (code >= KEY_Z && code <= KEY_M) {
        static const char ls[] = "zxcvbnm";
        c = ls[code - KEY_Z];
        up = true;
    } else {
        static const struct { uint16_t code; char n, s; } mt[] = {
            { KEY_MINUS, '-', '_' }, { KEY_EQUAL, '=', '+' },
            { KEY_LEFTBRACE, '[', '{' }, { KEY_RIGHTBRACE, ']', '}' },
            { KEY_SEMICOLON, ';', ':' }, { KEY_APOSTROPHE, '\'', '"' },
            { KEY_GRAVE, '`', '~' }, { KEY_BACKSLASH, '\\', '|' },
            { KEY_COMMA, ',', '<' }, { KEY_DOT, '.', '>' },
            { KEY_SLASH, '/', '?' },
        };
        for (size_t i = 0; i < sizeof(mt) / sizeof(mt[0]); i++) {
            if (mt[i].code == code) {
                c = sh ? mt[i].s : mt[i].n;
                break;
            }
        }
    }
    if (!c)
        return 0;
    if (up) {
        bool eff = kcaps ? !sh : sh;
        if (eff)
            c = (char)(c - 'a' + 'A');
        if (kctrl)
            c = (char)(c & 0x1f);
    }
    tmp[0] = (uint8_t)c;
    *out = tmp;
    return 1;
}

static void pump_keyboard(void)
{
    if (!kbd_ready) {
        double t = now();
        if (t - kbd_try >= 0.25) {
            kbd_try = t;
            kbd_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (kbd_fd >= 0) {
                kbd_ready = true;
                fprintf(stderr, "splash: keyboard ready (event0)\n");
            }
        }
        return;
    }
    uint8_t buf[sizeof(struct input_event) * 16];
    for (int k = 0; k < 8; k++) {
        ssize_t n = read(kbd_fd, buf, sizeof(buf));
        if (n < (ssize_t)sizeof(struct input_event))
            break;
        for (ssize_t off = 0; off + (ssize_t)sizeof(struct input_event) <= n;
             off += sizeof(struct input_event)) {
            struct input_event ev;
            memcpy(&ev, buf + off, sizeof(ev));
            if (ev.type != EV_KEY)
                continue;
            if (ev.value == 1 || ev.value == 2) {          /* press / repeat */
                const uint8_t *out;
                int l = map_key(ev.code, &out);
                if (l > 0)
                    key_push(out, l);
            }
            switch (ev.code) {
            case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
                kshift = ev.value != 0; break;
            case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
                kctrl = ev.value != 0; break;
            case KEY_CAPSLOCK:
                if (ev.value == 1) kcaps = !kcaps; break;
            default: break;
            }
        }
        if (n < (ssize_t)sizeof(buf))
            break;
    }
}

/* ---------------- terminal window texture rendering ---------------------- */
/* render the window texture; the texture spans the window rect plus the
 * baked soft-shadow margin (SMc) on every side, with rounded corners
 * carried as per-texel alpha, so compositing never loses the shadow */
static void render_winbuf(int w, int h)
{
    if (!winbuf || w <= 0 || h <= 0)
        return;
    while (SMc > 4 && (size_t)(w + 2 * SMc) * (size_t)(h + 2 * SMc) >
                      (size_t)W * (size_t)(H - panel_h))
        SMc--;                                /* keep the texture in bounds */
    int tw_ = w + 2 * SMc, th_ = h + 2 * SMc;
    winbuf_w = tw_;
    winbuf_h = th_;
    memset(winbuf, 0, (size_t)tw_ * th_ * 4);
    const int ox = SMc, oy = SMc;

    /* body: frosted dark glass from the pre-blurred backdrop */
    if (winblur) {
        for (int y = 0; y < h; y++) {
            uint32_t *d = winbuf + (size_t)(y + oy) * tw_ + ox;
            for (int x = 0; x < w; x++) {
                int bx = x + (wx - winblur_x0);
                int by = y + (wy - winblur_y0);
                if (bx < 0) bx = 0;
                if (by < 0) by = 0;
                if (bx >= winblur_w) bx = winblur_w - 1;
                if (by >= winblur_h) by = winblur_h - 1;
                uint32_t v = winblur[(size_t)by * winblur_w + bx];
                /* ~12% blurred backdrop + ~88% dark tint */
                unsigned r = ((v >> 16 & 255) * 31 + 20 * 225) >> 8;
                unsigned g = ((v >>  8 & 255) * 31 + 20 * 225) >> 8;
                unsigned b = ((v       & 255) * 34 + 24 * 225) >> 8;
                d[x] = rgb(r, g, b);
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            uint32_t *d = winbuf + (size_t)(y + oy) * tw_ + ox;
            for (int x = 0; x < w; x++)
                d[x] = rgb(22, 22, 26);
        }
    }

    /* title bar: light gradient (macOS light chrome) */
    int r0 = 238, g0 = 238, b0 = 240, r1 = 224, g1 = 224, b1 = 227;
    for (int y = 0; y < tw_title && y < h; y++) {
        double f = (tw_title > 1) ? (double)y / (tw_title - 1) : 0;
        unsigned r = (unsigned)(r0 + (r1 - r0) * f);
        unsigned g = (unsigned)(g0 + (g1 - g0) * f);
        unsigned b = (unsigned)(b0 + (b1 - b0) * f);
        uint32_t *d = winbuf + (size_t)(y + oy) * tw_ + ox;
        for (int x = 0; x < w; x++)
            d[x] = rgb(r, g, b);
    }
    /* hairline under the title bar */
    if (tw_title < h) {
        uint32_t *d = winbuf + (size_t)(tw_title + oy) * tw_ + ox;
        for (int x = 0; x < w; x++)
            d[x] = rgb(196, 196, 200);
    }

    /* traffic lights */
    {
        double lr = fmax(5.5, 6.5 * u);
        double lcy = oy + (tw_title - 1) / 2.0 + 0.5;
        double lx[3] = { ox + 19 * u, ox + 39 * u, ox + 59 * u };
        uint32_t lcol[3] = { rgb(255, 95, 87), rgb(254, 188, 46), rgb(40, 200, 64) };
        for (int i = 0; i < 3; i++) {
            double ccx = lx[i], ccy = lcy;
            int x0 = (int)floor(ccx - lr - 1), x1 = (int)ceil(ccx + lr + 1);
            int y0 = (int)floor(ccy - lr - 1), y1 = (int)ceil(ccy + lr + 1);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > tw_ - 1) x1 = tw_ - 1;
            if (y1 > th_ - 1) y1 = th_ - 1;
            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    double dx = x + 0.5 - ccx, dy = y + 0.5 - ccy;
                    double cov = 0.5 + lr - sqrt(dx * dx + dy * dy);
                    if (cov <= 0)
                        continue;
                    if (cov > 1)
                        cov = 1;
                    int a = (int)(cov * 255 + 0.5);
                    uint32_t ob = winbuf[(size_t)y * tw_ + x];
                    unsigned orr = ((ob >> 16 & 255) * (255 - a) + (lcol[i] >> 16 & 255) * a) / 255;
                    unsigned ogg = ((ob >>  8 & 255) * (255 - a) + (lcol[i] >>  8 & 255) * a) / 255;
                    unsigned obb = ((ob       & 255) * (255 - a) + (lcol[i]       & 255) * a) / 255;
                    winbuf[(size_t)y * tw_ + x] = rgb(orr, ogg, obb);
                }
            }
        }
    }

    /* title text, centered in the title bar (proportional UI font) */
    {
        const char *tstr = tm_dead ? TERM_DONE : TERM_TITLE;
        int tx = ox + (w - ui_text_w(tstr, 0)) / 2;
        int ty = oy + (tw_title - UIF_CELLH) / 2;
        if (ty < oy)
            ty = oy;
        ui_text_tex(winbuf, tw_, th_, tx, ty, tstr, rgb(110, 110, 116), 0);
    }

    /* rounded corners: rewrite the alpha channel of the 4 corner boxes */
    {
        int r = (int)(WIN_R * u);
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        for (int cyy = 0; cyy < 2; cyy++) {
            int by0 = cyy ? oy + h - r : oy;
            for (int cxx = 0; cxx < 2; cxx++) {
                int bx0 = cxx ? ox + w - r : ox;
                for (int y = 0; y < r; y++) {
                    uint32_t *d = winbuf + (size_t)(by0 + y) * tw_;
                    for (int x = 0; x < r; x++) {
                        double dd = sd_round(bx0 + x + 0.5, by0 + y + 0.5,
                                             ox, oy, ox + w - 1, oy + h - 1,
                                             (double)r);
                        double cov = 0.5 - dd;
                        if (cov >= 1)
                            continue;
                        uint32_t *p = &d[bx0 + x];
                        if (cov <= 0)
                            *p = 0;
                        else
                            *p = ((uint32_t)(cov * 255.0 + 0.5) << 24) |
                                 (*p & 0xFFFFFF);
                    }
                }
            }
        }
    }

    /* baked soft shadow in the margin ring around the window */
    {
        double sig = 10.0 * u, amax = 58.0;
        double rr = (WIN_R - 2) * u;
        if (rr < 2)
            rr = 2;
        for (int y = 0; y < th_; y++) {
            uint32_t *d = winbuf + (size_t)y * tw_;
            for (int x = 0; x < tw_; x++) {
                if (x >= ox && x < ox + w && y >= oy && y < oy + h)
                    continue;                 /* window area is opaque */
                double dd = sd_round(x + 0.5, y + 0.5, ox, oy,
                                     ox + w - 1, oy + h - 1, rr);
                if (dd < 0 || dd > SMc)
                    continue;
                int A = (int)(amax * exp(-dd / sig) + 0.5);
                if (A < 2)
                    continue;
                if (A > 255) A = 255;
                d[x] = (uint32_t)A << 24;     /* neutral black, alpha only */
            }
        }
    }
}

/* blend an RGBA texture (alpha in the top byte) over `back`, clipped to
 * the global clip rect; galpha 256 = full strength */
static void tex_blit_alpha(const uint32_t *tex, int tx, int ty, int tw_, int th_,
                           int galpha)
{
    int x0 = tx < cx0 ? cx0 : tx;
    int y0 = ty < cy0 ? cy0 : ty;
    int x1 = tx + tw_ - 1 > cx1 ? cx1 : tx + tw_ - 1;
    int y1 = ty + th_ - 1 > cy1 ? cy1 : ty + th_ - 1;
    for (int y = y0; y <= y1; y++) {
        const uint32_t *s = tex + (size_t)(y - ty) * tw_ + (x0 - tx);
        uint32_t *d = back + (size_t)y * W + x0;
        for (int x = 0; x <= x1 - x0; x++) {
            unsigned ta = s[x] >> 24;
            if (!ta)
                continue;
            if (ta == 255 && galpha >= 256) {
                d[x] = s[x];
                continue;
            }
            int a = galpha >= 256 ? (int)ta : (int)(((unsigned)ta * (unsigned)galpha) >> 8);
            uint32_t w = s[x] & 0xFFFFFF;
            uint32_t v = d[x];
            unsigned r = ((v >> 16 & 255) * (255 - a) + (w >> 16 & 255) * a) / 255;
            unsigned g = ((v >>  8 & 255) * (255 - a) + (w >>  8 & 255) * a) / 255;
            unsigned b = ((v       & 255) * (255 - a) + (w       & 255) * a) / 255;
            d[x] = rgb(r, g, b);
        }
    }
}

/* nearest-neighbour RGBA-scaled blend of `tex` into an arbitrary dst rect
 * (window animations: the texture carries shadow + rounded corners) */
static void tex_blit_scaled_alpha(const uint32_t *tex, int tw_, int th_,
                                  int dx0, int dy0, int dx1, int dy1, int alpha)
{
    if (dx1 < dx0 || dy1 < dy0 || tw_ <= 0 || th_ <= 0)
        return;
    int dw = dx1 - dx0 + 1, dh = dy1 - dy0 + 1;
    int x0 = dx0 < cx0 ? cx0 : dx0;
    int y0 = dy0 < cy0 ? cy0 : dy0;
    int x1 = dx1 > cx1 ? cx1 : dx1;
    int y1 = dy1 > cy1 ? cy1 : dy1;
    for (int y = y0; y <= y1; y++) {
        int sy = (int)(((long)(y - dy0) * th_) / dh);
        if (sy >= th_) sy = th_ - 1;
        const uint32_t *srow = tex + (size_t)sy * tw_;
        uint32_t *d = back + (size_t)y * W;
        for (int x = x0; x <= x1; x++) {
            int sx = (int)(((long)(x - dx0) * tw_) / dw);
            if (sx >= tw_) sx = tw_ - 1;
            unsigned ta = srow[sx] >> 24;
            if (!ta)
                continue;
            int a = (int)(((unsigned)ta * (unsigned)alpha) >> 8);
            if (!a)
                continue;
            uint32_t w = srow[sx] & 0xFFFFFF;
            uint32_t v = d[x];
            unsigned r = ((v >> 16 & 255) * (255 - a) + (w >> 16 & 255) * a) / 255;
            unsigned g = ((v >>  8 & 255) * (255 - a) + (w >>  8 & 255) * a) / 255;
            unsigned b = ((v       & 255) * (255 - a) + (w       & 255) * a) / 255;
            d[x] = rgb(r, g, b);
        }
    }
}

/* draw terminal grid + caret straight into `back` (the window texture
 * holds only chrome and backdrop, so typing never re-renders it) */
static void draw_term_content(int wx0, int wy0)
{
    if (sh_fd < 0)
        return;
    int gx = wx0 + (int)(8 * u);
    int gy = wy0 + tw_title + (int)(4 * u);
    for (int r = 0; r < t_rows; r++) {
        int py0 = gy + r * FONT_H;
        for (int c = 0; c < t_cols; c++) {
            int i = r * t_cols + c;
            uint8_t ch = tch[i];
            if (!ch)
                continue;
            if (ch < FONT_FIRST || ch > FONT_FIRST + FONT_COUNT - 1)
                ch = '?';
            const unsigned char (*gl)[FONT_W] =
                (const unsigned char (*)[FONT_W])FONT_BITMAP[ch - FONT_FIRST];
            uint32_t col = cell_color(i);
            int px0 = gx + c * FONT_W;
            for (int ry = 0; ry < FONT_H; ry++) {
                int py = py0 + ry;
                if (py < cy0 || py > cy1)
                    continue;
                const unsigned char *row = gl[ry];
                uint32_t *d = back + (size_t)py * W;
                for (int rx = 0; rx < FONT_W; rx++) {
                    int a = row[rx];
                    if (!a)
                        continue;
                    int px = px0 + rx;
                    if (px < cx0 || px > cx1)
                        continue;
                    uint32_t ob = d[px];
                    unsigned rr = ((ob >> 16 & 255) * (255 - a) + (col >> 16 & 255) * a) / 255;
                    unsigned gg = ((ob >>  8 & 255) * (255 - a) + (col >>  8 & 255) * a) / 255;
                    unsigned bb = ((ob       & 255) * (255 - a) + (col       & 255) * a) / 255;
                    d[px] = rgb(rr, gg, bb);
                }
            }
        }
    }
    /* caret: thin light bar at the cursor cell (macOS-style) */
    if (caret_on) {
        int px0 = gx + tcx * FONT_W;
        int py0 = gy + tcy * FONT_H;
        int cw = FONT_W >= 8 ? 2 : 1;
        for (int ry = 0; ry < FONT_H; ry++) {
            int py = py0 + ry;
            if (py < cy0 || py > cy1)
                continue;
            uint32_t *d = back + (size_t)py * W;
            for (int rx = 0; rx < cw; rx++) {
                int px = px0 + rx;
                if (px < cx0 || px > cx1)
                    continue;
                d[px] = rgb(235, 235, 238);
            }
        }
    }
}

/* ---------------- terminal window: states & animations ------------------ */
/* stable states: CLOSED / OPEN / MINIMIZED; transitions carry an animation */
static bool tm_anim = false;
static double tm_t0 = 0;
static int A_fx0, A_fy0, A_fx1, A_fy1;   /* from rect                     */
static int A_tx0, A_ty0, A_tx1, A_ty1;   /* to rect                       */
static int A_fa, A_ta;                   /* from/to alpha                 */
static enum TmSt A_end = TM_CLOSED;      /* state after animation         */
static bool A_zoom = false;              /* zoomed after animation        */
static bool tm_zoomed = false;
static int force_x0, force_y0, force_x1, force_y1;   /* post-anim repaint  */
static bool force_dirty = false;
static bool force_full = false;         /* wipe the whole screen once   */

static void win_norm_rect(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = wx; *y0 = wy; *x1 = wx + ww - 1; *y1 = wy + wh - 1;
}

static bool win_max_rect(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = mw_x0; *y0 = mw_y0; *x1 = mw_x1; *y1 = mw_y1;
    return true;
}

/* current interpolated rect; returns false when nothing to draw */
static bool win_cur_rect(int *x0, int *y0, int *x1, int *y1, int *alpha)
{
    if (tm_anim) {
        double t = (now() - tm_t0) / ANIM_MS;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        double e = 1 - (1 - t) * (1 - t) * (1 - t);   /* ease-out cubic */
        *x0 = A_fx0 + (int)((A_tx0 - A_fx0) * e);
        *y0 = A_fy0 + (int)((A_ty0 - A_fy0) * e);
        *x1 = A_fx1 + (int)((A_tx1 - A_fx1) * e);
        *y1 = A_fy1 + (int)((A_ty1 - A_fy1) * e);
        *alpha = A_fa + (int)((A_ta - A_fa) * e);
        return *alpha > 2;
    }
    if (tm == TM_MINIMIZED || tm == TM_CLOSED)
        return false;
    if (tm_zoomed)
        win_max_rect(x0, y0, x1, y1);
    else
        win_norm_rect(x0, y0, x1, y1);
    *alpha = 256;
    return true;
}

static void anim_start(enum TmSt end, bool zoom,
                       int fx0, int fy0, int fx1, int fy1, int fa,
                       int tx0, int ty0, int tx1, int ty1, int ta)
{
    A_end = end;
    A_zoom = zoom;
    A_fx0 = fx0; A_fy0 = fy0; A_fx1 = fx1; A_fy1 = fy1; A_fa = fa;
    A_tx0 = tx0; A_ty0 = ty0; A_tx1 = tx1; A_ty1 = ty1; A_ta = ta;
    tm_anim = true;
    tm_t0 = now();
}

static void anim_update(void)
{
    if (!tm_anim)
        return;
    double t = (now() - tm_t0) / ANIM_MS;
    if (t < 1.0)
        return;
    /* force a repaint of the whole 'from' region so no animation frame
     * can leave ghost pixels behind (SMc covers the baked shadow) */
    force_x0 = A_fx0; force_y0 = A_fy0;
    force_x1 = A_fx1; force_y1 = A_fy1;
    force_dirty = true;
    tm_anim = false;
    tm = A_end;
    tm_zoomed = A_zoom;
    if (tm == TM_MINIMIZED || tm == TM_CLOSED)
        force_full = true;                 /* wipe any animation residue */
    if (tm == TM_CLOSED)
        term_kill();
    if (tm == TM_OPEN) {
        int a, b, c, d, al;
        win_cur_rect(&a, &b, &c, &d, &al);
        render_winbuf(c - a + 1, d - b + 1);  /* re-render at final dims */
    }
    term_dirty = true;
}

/* ---------------- window actions ---------------------------------------- */
static void win_open(void)
{
    if (tm != TM_CLOSED || tm_anim)
        return;
    fprintf(stderr, "ACT open\n");
    term_spawn();
    render_winbuf(ww, wh);
    int ix0, iy0, ix1, iy1;
    icon_rect(1.0, &ix0, &iy0, &ix1, &iy1);
    anim_start(TM_OPEN, false,
               ix0, iy0, ix1, iy1, 40,
               wx, wy, wx + ww - 1, wy + wh - 1, 256);
}

static void win_close_start(void)
{
    if (tm != TM_OPEN || tm_anim)
        return;
    fprintf(stderr, "ACT close\n");
    int ix0, iy0, ix1, iy1;
    icon_rect(1.0, &ix0, &iy0, &ix1, &iy1);
    int a, b, c, d;
    if (tm_zoomed)
        win_max_rect(&a, &b, &c, &d);
    else
        win_norm_rect(&a, &b, &c, &d);
    anim_start(TM_CLOSED, false,
               a, b, c, d, 256,
               ix0, iy0, ix1, iy1, 0);
}

static void win_minimize(void)
{
    if (tm != TM_OPEN || tm_anim)
        return;
    fprintf(stderr, "ACT minimize\n");
    int ix0, iy0, ix1, iy1;
    icon_rect(1.0, &ix0, &iy0, &ix1, &iy1);
    int a, b, c, d;
    if (tm_zoomed)
        win_max_rect(&a, &b, &c, &d);
    else
        win_norm_rect(&a, &b, &c, &d);
    anim_start(TM_MINIMIZED, tm_zoomed,
               a, b, c, d, 256,
               ix0, iy0, ix1, iy1, 0);
}

static void win_restore(void)
{
    if (tm != TM_MINIMIZED || tm_anim)
        return;
    fprintf(stderr, "ACT restore\n");
    render_winbuf(ww, wh);
    int ix0, iy0, ix1, iy1;
    icon_rect(1.0, &ix0, &iy0, &ix1, &iy1);
    anim_start(TM_OPEN, false,
               ix0, iy0, ix1, iy1, 0,
               wx, wy, wx + ww - 1, wy + wh - 1, 256);
}

static void win_zoom(void)
{
    if (tm != TM_OPEN || tm_anim)
        return;
    fprintf(stderr, "ACT zoom (zoomed=%d)\n", tm_zoomed);
    int a, b, c, d;
    if (!tm_zoomed) {
        win_max_rect(&a, &b, &c, &d);
        anim_start(TM_OPEN, true,
                   wx, wy, wx + ww - 1, wy + wh - 1, 256,
                   a, b, c, d, 256);
    } else {
        win_norm_rect(&a, &b, &c, &d);
        int mx0, my0, mx1, my1;
        win_max_rect(&mx0, &my0, &mx1, &my1);
        anim_start(TM_OPEN, false,
                   mx0, my0, mx1, my1, 256,
                   a, b, c, d, 256);
    }
}

/* ---------------- window drawing ----------------------------------------- */
/* the texture already carries the shadow and the rounded corners as
 * per-texel alpha, so every dirty-rect repaint restores both correctly */
static void draw_window(void)
{
    int x0, y0, x1, y1, alpha;
    if (!win_cur_rect(&x0, &y0, &x1, &y1, &alpha))
        return;
    if (!tm_anim && alpha >= 256 &&
        winbuf_w == (x1 - x0 + 1) + 2 * SMc &&
        winbuf_h == (y1 - y0 + 1) + 2 * SMc) {
        tex_blit_alpha(winbuf, x0 - SMc, y0 - SMc, winbuf_w, winbuf_h, 256);
        draw_term_content(x0, y0);
        return;
    }
    tex_blit_scaled_alpha(winbuf, winbuf_w, winbuf_h,
                          x0 - SMc, y0 - SMc, x1 + SMc, y1 + SMc, alpha);
}

/* ---------------- mouse --------------------------------------------------- */
static void handle_click(int x, int y);   /* defined below */

/* mild mouse acceleration for small deltas (PS/2 packets are tiny;
 * large deltas pass 1:1 so automated input stays precise) */
static int acc(int d)
{
    int a = d > 0 ? d : -d;
    int s = a + ((a > 4 && a <= 40) ? (a - 4) / 2 : 0);
    return d > 0 ? s : -s;
}

/* /dev/input/mice (mousedev): dy uses the device convention (+ = away from
 * the user = screen UP), so screen-space Y needs the negation */
static void pump_mouse(void)
{
    if (mouse_fd < 0) {
        double t = now();
        if (t - mouse_try >= 0.25) {
            mouse_try = t;
            mouse_fd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (mouse_fd >= 0)
                fprintf(stderr, "splash: mouse ready\n");
        }
        return;
    }
    uint8_t b[192];
    for (int k = 0; k < 8; k++) {
        ssize_t n = read(mouse_fd, b, sizeof(b));
        if (n < 3)
            break;
        for (ssize_t i = 0; i + 3 <= n; i += 3) {
            int f = b[i];
            int dx = b[i + 1], dy = b[i + 2];
            if (f & 0x10)
                dx -= 256;
            if (f & 0x20)
                dy -= 256;
            mx += acc(dx);
            my -= acc(dy);                        /* device Y is inverted */
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > W - 1) mx = W - 1;
            if (my > H - 1) my = H - 1;
            {
                static double last_pos_log = 0;
                double tn = now();
                if ((dx || dy) && tn - last_pos_log >= 1.0) {
                    last_pos_log = tn;
                    fprintf(stderr, "POS %d %d\n", mx, my);
                }
            }
            int l = f & 0x01;
            if (l && !btn_l) {
                double tnow = now();
                if (tnow - last_clk >= 0.08) {   /* debounce QEMU doubles */
                    last_clk = tnow;
                    fprintf(stderr, "CLK %d %d\n", mx, my);
                    handle_click(mx, my);
                }
            }
            btn_l = l;
        }
        if (n < (ssize_t)sizeof(b))
            break;
    }
}

/* ---------------- clicks --------------------------------------------------- */
static void dock_activate(void)
{
    if (tm_anim)
        return;
    if (tm == TM_CLOSED)
        win_open();
    else if (tm == TM_OPEN)
        win_minimize();
    else if (tm == TM_MINIMIZED)
        win_restore();
}

static bool click_in_buttons(int x, int y, int wx0, int wy0)
{
    double lr = fmax(5.5, 6.5 * u) + 4;       /* slop for easier clicks */
    double lcy = wy0 + (tw_title - 1) / 2.0 + 0.5;
    double lx[3] = { 19 * u, 39 * u, 59 * u };
    for (int i = 0; i < 3; i++)
        if (hypot(x - (wx0 + lx[i]), y - lcy) <= lr)
            return true;
    return false;
}

static void handle_click(int x, int y)
{
    if (!in_desktop)
        return;

    /* About panel: any click dismisses it */
    if (about_open) {
        about_open = false;
        about_dirty = true;
        return;
    }

    /* menu takes priority while open; any click closes it (macOS behaviour) */
    if (menu_open) {
        double e = menu_a * menu_a * (3 - 2 * menu_a);
        int oy = (int)((1.0 - e) * -10 * u);
        if (in_rect(x, y, it_x0, it_y0 + oy, it_x1, it_y1 + oy)) {
            fprintf(stderr, "ACT about\n");
            render_about_tex();
            about_open = true;
            about_dirty = true;
        }
        menu_open = false;
        menu_closing = true;
        return;
    }

    /* dock icon */
    int ix0, iy0, ix1, iy1;
    icon_rect(icon_s, &ix0, &iy0, &ix1, &iy1);
    int pad = (int)(9 * u);
    if (in_rect(x, y, ix0 - pad, iy0 - pad, ix1 + pad, iy1 + pad)) {
        fprintf(stderr, "HIT dock icon (rect %d,%d-%d,%d)\n", ix0, iy0, ix1, iy1);
        dock_activate();
        return;
    }

    /* window traffic lights (stable, visible window only) */
    if (tm == TM_OPEN && !tm_anim) {
        int a, b, c, d, al;
        win_cur_rect(&a, &b, &c, &d, &al);
        if (in_rect(x, y, a, b, c, d)) {
            if (click_in_buttons(x, y, a, b)) {
                double lr = fmax(5.5, 6.5 * u) + 4;
                double lcy = b + (tw_title - 1) / 2.0 + 0.5;
                double lx[3] = { 19 * u, 39 * u, 59 * u };
                double dist0 = hypot(x - (a + lx[0]), y - lcy);
                double dist1 = hypot(x - (a + lx[1]), y - lcy);
                double dist2 = hypot(x - (a + lx[2]), y - lcy);
                if (dist0 <= lr)
                    win_close_start();
                else if (dist1 <= lr)
                    win_minimize();
                else if (dist2 <= lr)
                    win_zoom();
            }
            return;                            /* clicks inside are consumed */
        }
    }

    /* droplet logo or the Settings item open the menu */
    if (in_rect(x, y, dr_x0 - 3, dr_y0 - 3, dr_x1 + 3, dr_y1 + 3) ||
        in_rect(x, y, ti_x0, ti_y0, ti_x1, ti_y1)) {
        fprintf(stderr, "HIT menu\n");
        render_menu_tex();
        menu_open = true;
        menu_closing = false;
        return;
    }
    fprintf(stderr, "MISS (%d,%d)\n", x, y);
}

/* ---------------- dirty-rect scene compositor ------------------------------- */
typedef struct { int x0, y0, x1, y1; } Rect;

static Rect dr[24];
static int drn;

static void add_rect(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    if (x0 > x1 || y0 > y1 || drn >= (int)(sizeof(dr) / sizeof(dr[0])))
        return;
    dr[drn].x0 = x0; dr[drn].y0 = y0;
    dr[drn].x1 = x1; dr[drn].y1 = y1;
    drn++;
}

static void copy_desktop_rect(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        memcpy(back + (size_t)y * W + x0,
               desktop + (size_t)y * W + x0, (size_t)(x1 - x0 + 1) * 4);
}

/* render one scene layer stack inside a rect and push it to the fb */
static void scene_render(int x0, int y0, int x1, int y1)
{
    cx0 = x0 < 0 ? 0 : x0; cy0 = y0 < 0 ? 0 : y0;
    cx1 = x1 > W - 1 ? W - 1 : x1; cy1 = y1 > H - 1 ? H - 1 : y1;
    copy_desktop_rect(cx0, cy0, cx1, cy1);
    draw_dock_icon();
    draw_window();
    draw_menu_animated();
    draw_about();
    draw_fps(1);
    draw_clock(1);
    draw_cursor();
    blit_rect(cx0, cy0, cx1, cy1);
}

/* boot scene pieces (black background) */
static void boot_clear(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        memset(back + (size_t)y * W + x0, 0, (size_t)(x1 - x0 + 1) * 4);
}

static void boot_log_rect(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = LOG_MARGIN - 2;
    *x1 = W - LOG_MARGIN + 2;
    *y1 = H - 1;
    *y0 = H - LOG_MARGIN - DISPLAY_LINES * font_h_s(1) - 2;
    if (*y0 < 0) *y0 = 0;
}

static void boot_spinner_rect(int *x0, int *y0, int *x1, int *y1)
{
    double R = fmax(16.0, H * 0.032);
    double sw = fmax(2.5, R * 0.15);
    int ccx = W / 2, ccy = H / 2;
    *x0 = ccx - (int)(R + sw * 2 + 2);
    *x1 = ccx + (int)(R + sw * 2 + 2);
    *y0 = ccy - (int)(R + sw * 2 + 2);
    *y1 = ccy + (int)(R + sw * 2 + 2);
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > W - 1) *x1 = W - 1;
    if (*y1 > H - 1) *y1 = H - 1;
}

static void draw_log_lines(void)
{
    int nl = reveal_n < DISPLAY_LINES ? reveal_n : DISPLAY_LINES;
    if (nl <= 0)
        return;
    int first = reveal_n - nl;
    int ty = H - LOG_MARGIN - nl * font_h_s(1);
    for (int i = 0; i < nl; i++)
        draw_text_s(LOG_MARGIN, ty + i * font_h_s(1), hist[first + i].text,
                    rgb(LOG_R, LOG_G, LOG_B), 1);
}

/* ---------------- main ------------------------------------------------------ */
enum St { ST_BOOT, ST_FADE, ST_WALL };

int main(void)
{
    signal(SIGHUP, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);

    /* keep the kernel VT out of the picture: graphics mode stops fbcon from
     * drawing console text / cursor over our framebuffer (like X.org does);
     * diagnostics go to the serial port instead of the screen */
    vt_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (vt_fd >= 0)
        ioctl(vt_fd, KDSETMODE, KD_GRAPHICS);
    freopen("/dev/null", "r", stdin);
    if (!freopen("/dev/ttyS0", "w", stderr))
        freopen("/dev/null", "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);      /* POS debug must not lag */

    kfd = open(KMSG_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kfd < 0)
        fprintf(stderr, "splash: cannot open kmsg: %s\n", strerror(errno));

    if (fb_open() != 0) {
        fprintf(stderr, "splash: no framebuffer\n");
        return 1;
    }
    if (wall_load() != 0)
        fprintf(stderr, "splash: wallpaper missing, will fade to black\n");
    pal_init();
    build_desktop();
    cursor_init();
    menu_geometry();
    icon_s_drawn = 1.0;

    double t0 = now(), last = t0, rot = 0, fade0 = 0;
    double fps_t0 = t0;
    int fps_frames = 0;
    enum St st = ST_BOOT;
    bool ready = false;
    bool scene_invalid = true;
    mx = W / 2;
    my = H / 2;
    pmx = mx; pmy = my;

    for (;;) {
        double t = now();
        double rdt = t - last;
        last = t;
        double dt = (rdt <= 0 || rdt > 0.25) ? FRAME_DT : rdt;
        rot += dt * 2.0 * M_PI / SPIN_PERIOD;

        /* real FPS meter: frames actually composed per second */
        fps_frames++;
        if (t - fps_t0 >= FPS_WINDOW) {
            fps_now = (int)(fps_frames / (t - fps_t0) + 0.5);
            fps_frames = 0;
            fps_t0 = t;
            fps_ready = true;
        }

        if (st == ST_BOOT) {
            if (kfd >= 0)
                pump_kmsg();
            reveal();
            if (access(DONE_PATH, F_OK) == 0 || t - t0 > SAFETY_SECONDS) {
                st = ST_FADE;
                fade0 = t;
            }

            int lx0, ly0, lx1, ly1, sx0, sy0, sx1, sy1, fx0, fy0, fx1, fy1;
            boot_log_rect(&lx0, &ly0, &lx1, &ly1);
            boot_spinner_rect(&sx0, &sy0, &sx1, &sy1);
            fps_bbox(&fx0, &fy0, &fx1, &fy1);

            boot_clear(lx0, ly0, lx1, ly1);
            clip_full();
            cx0 = lx0; cy0 = ly0; cx1 = lx1; cy1 = ly1;
            draw_log_lines();

            boot_clear(sx0, sy0, sx1, sy1);
            cx0 = sx0; cy0 = sy0; cx1 = sx1; cy1 = sy1;
            draw_spinner(W * 0.5, H * 0.5, fmax(16.0, H * 0.032), rot);

            boot_clear(fx0, fy0, fx1, fy1);
            cx0 = fx0; cy0 = fy0; cx1 = fx1; cy1 = fy1;
            draw_fps(0);

            blit_rect(lx0, ly0, lx1, ly1);
            blit_rect(sx0, sy0, sx1, sy1);
            blit_rect(fx0, fy0, fx1, fy1);
            clip_full();
        }

        if (st == ST_FADE) {
            /* full-frame compose: log + spinner over fading desktop */
            clip_full();
            memset(back, 0, (size_t)W * H * 4);
            draw_log_lines();
            draw_spinner(W * 0.5, H * 0.5, fmax(16.0, H * 0.032), rot);
            if (desktop) {
                double a = (t - fade0) / FADE_SECONDS;
                if (a >= 1.0) {
                    st = ST_WALL;
                    scene_invalid = true;
                    memcpy(back, desktop, (size_t)W * H * 4);
                } else {
                    a = a * a * (3 - 2 * a);      /* smoothstep easing */
                    int A = (int)(a * 256.0 + 0.5);
                    for (size_t i = 0; i < (size_t)W * H; i++) {
                        uint32_t v = back[i], w = desktop[i];
                        unsigned r = ((v >> 16 & 255) * (256 - A) + (w >> 16 & 255) * A) >> 8;
                        unsigned g = ((v >>  8 & 255) * (256 - A) + (w >>  8 & 255) * A) >> 8;
                        unsigned b = ((v       & 255) * (256 - A) + (w       & 255) * A) >> 8;
                        back[i] = rgb(r, g, b);
                    }
                }
            }
            draw_fps(0);
            blit_rect(0, 0, W - 1, H - 1);
        }

        if (st == ST_WALL) {
            in_desktop = true;
            pmx = mx; pmy = my;
            pump_mouse();
            pump_keyboard();
            if (keyq_n > 0 && sh_fd >= 0) {
                term_write(keyq, keyq_n);
                keyq_n = 0;
            } else if (keyq_n > 0) {
                keyq_n = 0;
            }
            dock_update(dt);
            clock_update();

            /* menu dropdown animation */
            if (menu_open) {
                menu_a += dt / MENU_MS;
                if (menu_a >= 1.0) { menu_a = 1.0; menu_closing = false; }
            } else if (menu_closing) {
                menu_a -= dt / MENU_MS;
                if (menu_a <= 0.0) { menu_a = 0.0; menu_closing = false; }
            }

            /* caret blink */
            if (tm == TM_OPEN && !tm_anim && sh_fd >= 0) {
                bool on = fmod(t, 1.06) < 0.53;
                if (on != caret_on) {
                    caret_on = on;
                    term_dirty = true;
                }
            }
            anim_update();
            term_poll();

            /* ---------- dirty rect collection ---------- */
            drn = 0;
            if (scene_invalid || force_full) {
                add_rect(0, 0, W - 1, H - 1);
                force_full = false;
            } else {
                if (force_dirty) {
                    int m = SMc + (int)(18 * u);
                    add_rect(force_x0 - m, force_y0 - m, force_x1 + m, force_y1 + m);
                    force_dirty = false;
                }
                /* cursor: cover old and new position; for teleports (fast
                 * multi-packet moves) repaint the whole frame once — cheap
                 * and guarantees no ghost trails */
                if (abs(mx - pmx) > 50 || abs(my - pmy) > 50) {
                    force_full = true;
                } else {
                    int ax0 = pmx < mx ? pmx : mx;
                    int ay0 = pmy < my ? pmy : my;
                    int ax1 = pmx > mx ? pmx : mx;
                    int ay1 = pmy > my ? pmy : my;
                    add_rect(ax0 - 4, ay0 - 4, ax1 + cur_w + 4, ay1 + cur_h + 4);
                }

                int fx0, fy0, fx1, fy1;
                fps_bbox(&fx0, &fy0, &fx1, &fy1);
                add_rect(fx0, fy0, fx1, fy1);

                /* menu */
                bool hover = in_rect(mx, my, ti_x0, ti_y0, ti_x1, ti_y1);
                static bool hover_prev = false;
                if (menu_open || menu_a > 0.001 || menu_closing || hover != hover_prev) {
                    add_rect(ti_x0 - 2, ti_y0, ti_x1 + 2, ti_y1);
                    if (menu_a > 0.001) {
                        double e = menu_a * menu_a * (3 - 2 * menu_a);
                        int oy = (int)((1.0 - e) * -10 * u);
                        add_rect(dd_x0 - DDMc - 2, dd_y0 + oy - DDMc - 2,
                                 dd_x1 + DDMc + 2, dd_y1 + oy + DDMc + 2);
                    }
                }
                hover_prev = hover;

                /* dock icon magnification / running dot */
                bool dot_on = tm != TM_CLOSED;
                if (fabs(icon_s - icon_s_drawn) > 0.002 || dot_on != dock_dot_drawn) {
                    int ix0, iy0, ix1, iy1;
                    icon_rect(fmax(icon_s, icon_s_drawn), &ix0, &iy0, &ix1, &iy1);
                    add_rect(ix0 - 4, iy0 - 4, ix1 + 4, ddy1 + 2);
                    icon_s_drawn = icon_s;
                    dock_dot_drawn = dot_on;
                }

                /* terminal window (texture includes the baked shadow) */
                if ((tm != TM_CLOSED && tm != TM_MINIMIZED) || tm_anim) {
                    int a, b, c, d, al;
                    if (win_cur_rect(&a, &b, &c, &d, &al)) {
                        int m = SMc + 2;
                        if (tm_anim) {
                            int am = SMc + (int)(18 * u);
                            add_rect(A_fx0 - am, A_fy0 - am, A_fx1 + am, A_fy1 + am);
                            add_rect(A_tx0 - am, A_ty0 - am, A_tx1 + am, A_ty1 + am);
                        } else {
                            static int px0 = -1, py0 = -1, px1 = -1, py1 = -1;
                            if (term_dirty ||
                                a != px0 || b != py0 || c != px1 || d != py1) {
                                add_rect(a - m, b - m, c + m, d + m);
                                px0 = a; py0 = b; px1 = c; py1 = d;
                            }
                        }
                        term_dirty = false;
                    }
                }

                /* About panel (repaint fully on open/close) */
                if (about_dirty) {
                    add_rect(ab_x0 - AMDc - 2, ab_y0 - AMDc - 2,
                             ab_x1 + AMDc + 2, ab_y1 + AMDc + 2);
                    about_dirty = false;
                }
            }
            scene_invalid = false;

            for (int i = 0; i < drn; i++)
                scene_render(dr[i].x0, dr[i].y0, dr[i].x1, dr[i].y1);
        }

        if (!ready) {
            int fd = open(READY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                write(fd, "1", 1);
                close(fd);
            }
            ready = true;
        }

        double rem = last + FRAME_DT - now();     /* frame pacing */
        if (rem > 0) {
            struct timespec ts = {
                .tv_sec = (time_t)rem,
                .tv_nsec = (long)((rem - (double)(time_t)rem) * 1e9)
            };
            nanosleep(&ts, 0);
        }
    }
}
