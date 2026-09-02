/*
 * AquaOS boot splash / desktop shell (v7).
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
#include <dirent.h>
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
static int wheel_fd = -1; /* evdev device carrying REL_WHEEL, or -1    */
static int vt_fd = -1;    /* /dev/tty0 kept open in KD_GRAPHICS mode    */
static double mouse_try = 0;
static double wheel_try = 0;
static int mx, my;        /* cursor position, hotspot at the tip       */
static int pmx, pmy;      /* cursor position on the previous frame     */
static int btn_l;         /* left button state                         */
static double last_clk = -1;  /* click debounce timer                      */

/* ---- macOS-style menu bar: several menus with working items ---- */
#define NMENUS 3
static const char *const mtitle[NMENUS] = { "Settings", "File", "Window" };
static const bool mtitle_bold[NMENUS] = { true, false, false };
#define MITEMS_MAX 4
static const char *const mitems[NMENUS][MITEMS_MAX] = {
    { "About AquaOS", NULL, NULL, NULL },
    { "New Window", "Close Window", NULL, NULL },
    { "Minimize", "Zoom", NULL, NULL },
};
static bool menu_open = false;
static int menu_idx = -1;         /* which title's dropdown is open      */
static double menu_a = 0;         /* dropdown animation 0..1             */
static bool menu_closing = false;
static bool in_desktop = false;
static int tr[NMENUS][4];                 /* title bar item rects        */
static int dr_x0, dr_y0, dr_x1, dr_y1;    /* droplet logo rect           */
static int dd_x0, dd_y0, dd_x1, dd_y1;    /* open dropdown rect          */
static int it_x0, it_y0, it_x1, it_y1;    /* open item strip (vertical)  */
static int it_r[MITEMS_MAX][4];           /* per-item rects (open menu)  */

/* window dragging (macOS-style, by the title bar) */
static bool dragging = false;
static int drag_gx, drag_gy;              /* grab offset inside window   */
static double last_title_clk = -1;        /* double-click detector       */
static uint32_t *lift_tex = NULL;         /* extra "lift" shadow in drag */
static int lift_tex_w, lift_tex_h;

/* macOS menu bar clock (real system time) */
static char clk_str[8];
static char dat_str[16];
static bool clk_valid = false;

/* battery / power (real values from /sys/class/power_supply; without a
 * battery the system is on external power - shown as full + bolt) */
static int bat_pct = -1;                  /* -1 = unknown hardware       */
static bool bat_charging = false;
static bool bat_full = false;
static bool ac_online = false;
static double bat_next = 0;
static int bat_drawn = -2;                /* last painted state          */

/* About panel (real data only: kernel, framebuffer, uptime, fps) */
static bool about_open = false;
static bool about_dirty = false;
static int ab_x0, ab_y0, ab_x1, ab_y1;
static char kver[48];          /* kernel release, parsed from real kmsg */

/* pre-baked frosted panels (dropdown / About) incl. soft shadow  */
static uint32_t *menu_tex;
static int menu_tex_w, menu_tex_h;
static int DDMc;                          /* dropdown shadow margin     */
static int menu_tex_alloc_w, menu_tex_alloc_h;
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

/* Bake a macOS-style soft shadow ring into an ARGB texture (alpha only).
 * The shadow follows the rounded silhouette with the same radius as the
 * window corners, is gently biased downwards (like macOS), and fades out
 * fully before the margin edge so no rectangular cut-off can ever show. */
static void bake_shadow_ring(uint32_t *tex, int tw_, int th_,
                             int ox, int oy, int w, int h,
                             double rad, double sig, double amax, double offy)
{
    double edge_w = 2.5;
    int M = tw_ - ox - w;                     /* margin on the right side */
    for (int y = 0; y < th_; y++) {
        uint32_t *d = tex + (size_t)y * tw_;
        for (int x = 0; x < tw_; x++) {
            if (x >= ox && x < ox + w && y >= oy && y < oy + h)
                continue;                     /* window area is untouched */
            double dd = sd_round(x + 0.5, y + 0.5 - offy, ox, oy,
                                 ox + w - 1, oy + h - 1, rad);
            if (dd < 0 || dd > sig * 2.4)
                continue;
            double fall = exp(-dd / sig);
            /* smooth fade to zero at the very margin (no hard cut) */
            double dm = (double)M - dd;
            if (dm < edge_w) {
                if (dm <= 0)
                    continue;
                fall *= dm / edge_w;
            }
            int A = (int)(amax * fall + 0.5);
            if (A < 2)
                continue;
            if (A > 255)
                A = 255;
            d[x] = (uint32_t)A << 24;
        }
    }
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
static int prx0 = -1, pry0 = -1, prx1 = -1, pry1 = -1;  /* prev win rect */

/* terminal states */
enum TmSt { TM_CLOSED, TM_OPENING, TM_OPEN, TM_MINIMIZING, TM_MINIMIZED,
            TM_RESTORE, TM_CLOSING, TM_ZOOMIN, TM_ZOOMOUT };
static enum TmSt tm = TM_CLOSED;
static bool tm_dead = false;      /* shell exited                         */
static bool caret_on = true;      /* caret blink phase                    */
static bool caret_dirty = false;  /* caret cell needs a repaint           */
static int carect[4];             /* caret cell rect                      */
static bool extra_dirty = false;  /* one-shot extra repaint rect          */
static int extra_rect[4];

#define ANIM_MS   0.24            /* window animation duration, s         */
#define MENU_MS   0.17            /* dropdown animation duration, s       */

/* ---------------- desktop panel --------------------------------------- */
static int ui_text_w(const char *s, int bold);   /* defined below */
static bool tm_anim;                     /* window animation in flight    */

static void build_desktop(void)
{
    panel_h = (int)fmax(25.0, 25.0 * u);      /* slim macOS menu bar */
    if (panel_h > H / 4)
        panel_h = H / 4;
    SMc = (int)(26 * u);                      /* window shadow margin */
    if (SMc < 18)
        SMc = 18;
    DDMc = (int)(20 * u);                     /* dropdown shadow margin */
    AMDc = (int)(20 * u);                     /* About panel shadow margin */
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
        box_blur_h(s2, s1, W, panel_h, 16);
        box_blur_v(s1, s2, W, panel_h, 4);
        /* frosted glass: 53% blurred wallpaper + 47% white (macOS-like) */
        for (int i = 0; i < W * panel_h; i++) {
            uint32_t v = s1[i];
            unsigned r = ((v >> 16 & 255) * 136 + 255 * 119) >> 8;
            unsigned g = ((v >>  8 & 255) * 136 + 255 * 119) >> 8;
            unsigned b = ((v       & 255) * 136 + 255 * 119) >> 8;
            desktop[i] = rgb(r, g, b);
        }
    }
    free(s1);
    free(s2);
    /* hairline along the bottom edge of the panel */
    for (int x = 0; x < W; x++) {
        uint32_t v = desktop[(size_t)(panel_h - 1) * W + x];
        unsigned r = ((v >> 16 & 255) * 226) >> 8;
        unsigned g = ((v >>  8 & 255) * 226) >> 8;
        unsigned b = ((v       & 255) * 226) >> 8;
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

    /* menu textures: big enough for the widest menu (items differ per menu) */
    {
        int mw_max = (int)(150 * u), mh_max = 0;
        for (int m = 0; m < NMENUS; m++) {
            int n = 0, w = 0;
            for (int i = 0; i < MITEMS_MAX && mitems[m][i]; i++) {
                int iw = ui_text_w(mitems[m][i], 0);
                if (iw > w)
                    w = iw;
                n++;
            }
            w += (int)(44 * u);
            if (w < (int)(150 * u))
                w = (int)(150 * u);
            int h = (int)(10 * u) + n * (int)(26 * u) + (int)(6 * u);
            if (w > mw_max)
                mw_max = w;
            if (h > mh_max)
                mh_max = h;
        }
        menu_tex_alloc_w = mw_max + 2 * DDMc + 2;
        menu_tex_alloc_h = mh_max + 2 * DDMc + 2;
        menu_tex = malloc((size_t)menu_tex_alloc_w * menu_tex_alloc_h * 4);
    }
    about_tex = malloc((size_t)((int)(400 * u)) * (size_t)((int)(252 * u)) * 4);

    /* "lift" shadow for window dragging: same silhouette, stronger and
     * wider than the resting shadow - the macOS picking-up feel */
    lift_tex_w = ww + 2 * SMc;
    lift_tex_h = wh + 2 * SMc;
    lift_tex = malloc((size_t)lift_tex_w * lift_tex_h * 4);
    if (lift_tex) {
        memset(lift_tex, 0, (size_t)lift_tex_w * lift_tex_h * 4);
        bake_shadow_ring(lift_tex, lift_tex_w, lift_tex_h,
                         SMc, SMc, ww, wh, (double)(WIN_R * u),
                         22.0 * u, 105.0, 6.0 * u);
    }

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

/* ---- real clock for the menu bar (system time, Europe/Moscow) ---- */
static const char *const MON_NAME[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static void clock_update(void)
{
    time_t rt = time(NULL);
    struct tm tmb;
    if (!localtime_r(&rt, &tmb))
        return;
    char buf[8], dbuf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", tmb.tm_hour, tmb.tm_min);
    snprintf(dbuf, sizeof(dbuf), "%s %d", MON_NAME[tmb.tm_mon % 12],
             tmb.tm_mday);
    if (!clk_valid || strcmp(buf, clk_str) != 0 || strcmp(dbuf, dat_str) != 0) {
        snprintf(clk_str, sizeof(clk_str), "%s", buf);
        snprintf(dat_str, sizeof(dat_str), "%s", dbuf);
        clk_valid = true;
    }
}

/* ---- battery / power supply: real values from /sys/class/power_supply ---- */
static bool sysfile_int(const char *base, const char *leaf, int *out)
{
    char p[128];
    snprintf(p, sizeof(p), "/sys/class/power_supply/%s/%s", base, leaf);
    FILE *f = fopen(p, "r");
    if (!f)
        return false;
    int v = -1;
    bool ok = fscanf(f, "%d", &v) == 1;
    fclose(f);
    if (ok)
        *out = v;
    return ok;
}

static void battery_poll(void)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d)
        return;
    int pct = -1;
    bool charging = false, full = false, ac = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (!strncmp(e->d_name, "BAT", 3) && pct < 0) {
            int v;
            if (sysfile_int(e->d_name, "capacity", &v)) {
                pct = v;
                char p[128], st[32] = "";
                snprintf(p, sizeof(p), "/sys/class/power_supply/%s/status",
                         e->d_name);
                FILE *f = fopen(p, "r");
                if (f) {
                    size_t n = fread(st, 1, sizeof(st) - 1, f);
                    st[n] = 0;
                    fclose(f);
                }
                if (strstr(st, "Charg"))
                    charging = true;
                if (strstr(st, "Full"))
                    full = true;
            }
        } else if (e->d_name[0] == 'A') {
            int v;
            if (sysfile_int(e->d_name, "online", &v) && v == 1)
                ac = true;
        }
    }
    closedir(d);
    bat_pct = pct;
    bat_charging = charging;
    bat_full = full;
    ac_online = ac;
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
    const double SC = 1.18;                   /* compact macOS cursor */
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

/* live status readout, top-right corner (inside the panel on the desktop):
 * FPS counter, battery icon, date and time - macOS status area layout */
static int bat_icon_w(void) { return (int)(25 * u); }

static void fps_bbox(int *bx0, int *by0, int *bx1, int *by1)
{
    int tw = ui_text_w("999 FPS", 0) + 12 + bat_icon_w() + 12 +
             ui_text_w("Sep 30", 0) + 10 + ui_text_w("23:59", 0);
    *bx1 = W - 12 + 4;
    *bx0 = *bx1 - tw - 8;
    *by0 = (panel_h - UIF_CELLH) / 2 - 2;
    if (*by0 < 0) *by0 = 0;
    *by1 = panel_h - 1;
}

/* filled anti-aliased triangle (barycentric) - for the battery bolt */
static void paint_tri(double x0, double y0, double x1, double y1,
                      double x2, double y2, uint32_t col, int A)
{
    int px0 = (int)floor(fmin(x0, fmin(x1, x2))) - 1;
    int px1 = (int)ceil(fmax(x0, fmax(x1, x2))) + 1;
    int py0 = (int)floor(fmin(y0, fmin(y1, y2))) - 1;
    int py1 = (int)ceil(fmax(y0, fmax(y1, y2))) + 1;
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > W - 1) px1 = W - 1;
    if (py1 > H - 1) py1 = H - 1;
    double ar = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (fabs(ar) < 1e-9)
        return;
    int cr = col >> 16 & 255, cg = col >> 8 & 255, cb = col & 255;
    for (int y = py0; y <= py1; y++) {
        if (y < cy0 || y > cy1)
            continue;
        for (int x = px0; x <= px1; x++) {
            if (x < cx0 || x > cx1)
                continue;
            double fx = x + 0.5, fy = y + 0.5;
            double w0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / ar;
            double w1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / ar;
            double w2 = 1.0 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            double cov = fmin(1.0, fmin(w0, fmin(w1, w2)) * 4.0);
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

/* neat macOS-like battery: rounded outline, proportional fill, nub, and a
 * tiny bolt when running on external/charging power */
static void draw_battery(int x_right, int cy, bool dark)
{
    double w_ = 21 * u, h_ = 10.5 * u;
    double x0 = x_right - bat_icon_w() + 2;
    double y0 = cy - h_ / 2;
    double nub_w = 2.5 * u;
    uint32_t ocol = dark ? rgb(60, 60, 67) : rgb(235, 235, 240);
    int OA = dark ? 210 : 220;
    /* outline (AA rounded rect edge) */
    paint_round((int)x0, (int)y0, (int)(x0 + w_), (int)(y0 + h_),
                (int)fmax(2, 3 * u), ocol, OA, 1);
    /* nub */
    paint_round((int)(x0 + w_ + 1), (int)(cy - 2 * u),
                (int)(x0 + w_ + nub_w), (int)(cy + 2 * u), 1, ocol, OA, 0);
    /* fill level */
    int pct = bat_pct;
    bool charging = bat_charging || (pct < 0 && (ac_online || bat_full));
    if (pct < 0)
        pct = 100;                    /* no battery hw: on external power */
    if (pct > 100)
        pct = 100;
    double iw = (w_ - 4 * u) * pct / 100.0;
    if (iw >= 1) {
        uint32_t fcol = dark ? rgb(45, 45, 50) : rgb(240, 240, 245);
        if (pct <= 20 && !charging)
            fcol = rgb(255, 59, 48);
        paint_round((int)(x0 + 2 * u), (int)(y0 + 2 * u),
                    (int)(x0 + 2 * u + iw), (int)(y0 + h_ - 2 * u),
                    1, fcol, 235, 0);
    }
    /* charging bolt (small yellow lightning, macOS-style) */
    if (charging) {
        double cx = x0 + w_ / 2;
        double cy0 = y0 + 1.2 * u, cy1 = y0 + h_ - 1.2 * u;
        double s = h_ * 0.62;
        paint_tri(cx + 0.5 * s, cy0, cx - 0.55 * s, cy0 + 0.62 * s,
                  cx + 0.12 * s, cy0 + 0.62 * s, rgb(255, 204, 0), 235);
        paint_tri(cx - 0.5 * s, cy1, cx + 0.55 * s, cy1 - 0.62 * s,
                  cx - 0.12 * s, cy1 - 0.62 * s, rgb(255, 204, 0), 235);
    }
}

static void draw_fps(int dark)
{
    char buf[32];
    if (!fps_ready)
        return;                               /* no completed window yet */
    snprintf(buf, sizeof(buf), "%d FPS", fps_now);
    int x = W - 12 - ui_text_w("23:59", 0) - 10 - ui_text_w("Sep 30", 0) -
            12 - bat_icon_w() - 12 - ui_text_w(buf, 0);
    int y = (panel_h - UIF_CELLH) / 2;
    if (y < 2)
        y = 2;
    ui_text(x, y, buf, dark ? rgb(60, 60, 67) : rgb(168, 170, 176), 0);
}

static void draw_status_right(int dark)
{
    int y = (panel_h - UIF_CELLH) / 2;
    if (y < 2)
        y = 2;
    if (clk_valid) {
        /* time, rightmost (macOS) */
        int xt = W - 12 - ui_text_w(clk_str, 0);
        ui_text(xt, y, clk_str, dark ? rgb(35, 35, 40) : rgb(240, 240, 245), 0);
        /* date left of the time */
        int xd = xt - 10 - ui_text_w(dat_str, 0);
        ui_text(xd, y, dat_str, dark ? rgb(60, 60, 67) : rgb(220, 220, 226), 0);
        /* battery icon left of the date */
        draw_battery(xd - 12, panel_h / 2, dark);
    }
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
/* title rects (horizontal layout: droplet, Settings, File, Window) */
static void menu_geometry(void)
{
    int drop_s = (int)fmax(13.0, 14.0 * u);
    dr_x0 = MENU_X;
    dr_y0 = (panel_h - drop_s) / 2;
    dr_x1 = dr_x0 + drop_s - 1;
    dr_y1 = dr_y0 + drop_s - 1;
    int x = dr_x1 + 1 + (int)(11 * u);
    for (int m = 0; m < NMENUS; m++) {
        int tww = ui_text_w(mtitle[m], mtitle_bold[m] ? 1 : 0);
        tr[m][0] = x - (int)(8 * u);
        tr[m][1] = (int)(2 * u);
        tr[m][2] = x + tww + (int)(8 * u);
        tr[m][3] = panel_h - 1 - (int)(2 * u);
        x = tr[m][2] + (int)(6 * u);
    }
}

/* dropdown geometry for a given menu (rects for items included) */
static void menu_geometry_for(int m)
{
    int n = 0, w = 0;
    for (int i = 0; i < MITEMS_MAX && mitems[m][i]; i++) {
        int iw = ui_text_w(mitems[m][i], 0);
        if (iw > w)
            w = iw;
        n++;
    }
    w += (int)(44 * u);
    if (w < (int)(150 * u))
        w = (int)(150 * u);
    dd_x0 = tr[m][0];
    if (w > W - dd_x0 - 8)
        w = W - dd_x0 - 8;
    dd_y0 = panel_h + (int)(2 * u);
    dd_x1 = dd_x0 + w - 1;
    int iy = dd_y0 + (int)(5 * u);
    for (int i = 0; i < n; i++) {
        it_r[i][0] = dd_x0 + (int)(5 * u);
        it_r[i][1] = iy;
        it_r[i][2] = dd_x1 - (int)(5 * u);
        it_r[i][3] = iy + (int)(26 * u) - 1;
        iy += (int)(26 * u);
    }
    it_x0 = it_r[0][0];
    it_x1 = it_r[0][2];
    it_y0 = it_r[0][1];
    it_y1 = it_r[n - 1][3];
    dd_y1 = it_y1 + (int)(5 * u);
}

static int menu_item_count(int m)
{
    int n = 0;
    for (int i = 0; i < MITEMS_MAX && mitems[m][i]; i++)
        n++;
    return n;
}

/* draw an arbitrary RGBA icon (premultiplied-less, straight alpha) */
static void draw_icon_rgba(const unsigned char *data, int iw, int ih,
                           int x, int y, int size);

/* alpha-aware texture blitters (defined further below) */
static void tex_blit_alpha(const uint32_t *tex, int tx, int ty, int tw_,
                           int th_, int galpha);

/* ---- frosted macOS panels (dropdown / About), baked with soft shadow ----
 * texel format: ARGB, alpha in the top byte; shadow texels are pure alpha.
 * The shadow hugs the rounded silhouette, is biased downwards like macOS
 * and fades out smoothly - no rectangular halo. */
static void bake_panel_tex(uint32_t *tex, int *ptw, int *pth,
                           int px0, int py0, int px1, int py1,
                           int M, double rad, double sigma, double amax)
{
    int pw = px1 - px0 + 1, ph = py1 - py0 + 1;
    int tw_ = pw + 2 * M, th_ = ph + 2 * M;
    *ptw = tw_;
    *pth = th_;
    memset(tex, 0, (size_t)tw_ * th_ * 4);
    double offy = 3.0 * u;

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
            box_blur_h(s2, s1, bw, bh, 6);
            box_blur_v(s1, s2, bw, bh, 3);
            have_blur = true;
        }
    }

    for (int y = 0; y < th_; y++) {
        uint32_t *d = tex + (size_t)y * tw_;
        for (int x = 0; x < tw_; x++) {
            double dS = sd_round(x + 0.5, y + 0.5 - offy, M, M, M + pw - 1,
                                 M + ph - 1, rad);
            if (dS <= 0.5) {                  /* panel body (AA edge) */
                double cov = 0.5 - dS;
                if (cov > 1)
                    cov = 1;
                unsigned r = 250, g = 250, b = 252;
                if (have_blur) {
                    int sx = px0 - M + x - bx0;
                    int sy = py0 - M + y - by0;
                    if (sx < 0) sx = 0;
                    if (sy < 0) sy = 0;
                    if (sx >= bw) sx = bw - 1;
                    if (sy >= bh) sy = bh - 1;
                    uint32_t v = s1[(size_t)sy * bw + sx];
                    r = (unsigned)(((v >> 16 & 255) * 33 + 250 * 223) >> 8);
                    g = (unsigned)(((v >>  8 & 255) * 33 + 250 * 223) >> 8);
                    b = (unsigned)(((v       & 255) * 36 + 252 * 220) >> 8);
                }
                double bcov = 0.5 - fabs(dS); /* hairline border */
                if (bcov > 0) {
                    if (bcov > 1)
                        bcov = 1;
                    int BA = (int)(bcov * 26 + 0.5);
                    r = (unsigned)((r * (255 - BA)) / 255);
                    g = (unsigned)((g * (255 - BA)) / 255);
                    b = (unsigned)((b * (255 - BA)) / 255);
                }
                int A = (int)(cov * 255.0 + 0.5);
                d[x] = ((uint32_t)A << 24) | (r << 16) | (g << 8) | b;
            } else if (dS <= sigma * 2.4 && dS <= M) {   /* soft shadow */
                double a = amax * exp(-(dS - 0.5) / sigma);
                double dm = (double)M - dS;   /* fade at margin */
                if (dm < 2.5) {
                    if (dm <= 0)
                        a = 0;
                    else
                        a *= dm / 2.5;
                }
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
    if (!menu_tex || !desktop || menu_idx < 0)
        return;
    if (menu_tex_alloc_w < 10 || menu_tex_alloc_h < 10)
        return;
    bake_panel_tex(menu_tex, &menu_tex_w, &menu_tex_h,
                   dd_x0, dd_y0, dd_x1, dd_y1, DDMc, 10.0 * u, 13.0 * u, 42.0);
}

static void render_about_tex(void)
{
    if (!about_tex || !desktop)
        return;
    char ln_kernel[80], ln_fb[80], ln_up[80], ln_fps[80], ln_tz[80];
    int n = 0;
    const char *lines[8];
    lines[n++] = "Version 1.7 (AquaOS desktop)";
    if (kver[0]) {
        snprintf(ln_kernel, sizeof(ln_kernel), "Kernel %s", kver);
        lines[n++] = ln_kernel;
    }
    snprintf(ln_fb, sizeof(ln_fb), "Framebuffer %dx%d @ %d bpp", W, H, BPP * 8);
    lines[n++] = ln_fb;
    snprintf(ln_tz, sizeof(ln_tz), "Time zone MSK (UTC+3)");
    lines[n++] = ln_tz;
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
    if (pw > (int)(340 * u))
        pw = (int)(340 * u);
    if (ph > (int)(230 * u))
        ph = (int)(230 * u);
    ab_x0 = (W - pw) / 2;
    ab_x1 = ab_x0 + pw - 1;
    ab_y0 = (H - ph) * 2 / 5;
    ab_y1 = ab_y0 + ph - 1;
    bake_panel_tex(about_tex, &about_tex_w, &about_tex_h,
                   ab_x0, ab_y0, ab_x1, ab_y1, AMDc, 12.0 * u, 13.0 * u, 46.0);
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

/* menu item availability (macOS grays out unavailable items) */
static bool item_enabled(int m, int i)
{
    if (m == 0)
        return true;                          /* About AquaOS */
    if (m == 1)
        return i == 0 ? true                   /* New Window: open/restore */
                      : (tm == TM_OPEN && !tm_anim);
    /* Window menu */
    return tm == TM_OPEN && !tm_anim && !dragging;
}

static void draw_menu_animated(void)
{
    /* droplet logo at the left (clickable, like the app menu) */
    draw_icon_rgba(DROP_ICON_DATA, DROP_ICON_W, DROP_ICON_H,
                   dr_x0, dr_y0, dr_x1 - dr_x0 + 1);
    /* menu titles: blue when their dropdown is open, soft gray on hover */
    for (int m = 0; m < NMENUS; m++) {
        bool hov = in_rect(mx, my, tr[m][0], tr[m][1], tr[m][2], tr[m][3]);
        if (menu_open && m == menu_idx)
            paint_round(tr[m][0], tr[m][1], tr[m][2], tr[m][3], 5,
                        rgb(10, 122, 255), 256, 0);
        else if (hov)
            paint_round(tr[m][0], tr[m][1], tr[m][2], tr[m][3], 5,
                        rgb(0, 0, 0), 30, 0);
        int ty = (panel_h - UIF_CELLH) / 2;
        if (ty < 2)
            ty = 2;
        ui_text(tr[m][0] + (int)(8 * u), ty, mtitle[m],
                (menu_open && m == menu_idx) ? rgb(255, 255, 255)
                                             : rgb(28, 28, 30),
                mtitle_bold[m] ? 1 : 0);
    }
    if (menu_a <= 0.003 || menu_idx < 0)
        return;

    /* eased open state: slide + fade, texture includes the soft shadow */
    double e = menu_a;
    e = e * e * (3 - 2 * e);                  /* smoothstep */
    int oy = (int)((1.0 - e) * -8 * u);
    int alpha = (int)(e * 256);
    if (alpha > 256)
        alpha = 256;
    tex_blit_alpha(menu_tex, dd_x0 - DDMc, dd_y0 - DDMc + oy,
                   menu_tex_w, menu_tex_h, alpha);
    if (alpha > 150) {
        int n = menu_item_count(menu_idx);
        for (int i = 0; i < n; i++) {
            bool en = item_enabled(menu_idx, i);
            bool ah = en && in_rect(mx, my, it_r[i][0], it_r[i][1] + oy,
                                    it_r[i][2], it_r[i][3] + oy);
            if (ah && alpha > 60)
                paint_round(it_r[i][0], it_r[i][1] + oy,
                            it_r[i][2], it_r[i][3] + oy, 5,
                            rgb(10, 122, 255), alpha, 0);
            int lty = it_r[i][1] + oy + ((it_r[i][3] - it_r[i][1]) - UIF_CELLH) / 2;
            uint32_t tcol = !en ? rgb(158, 158, 164)
                                : (ah ? rgb(255, 255, 255) : rgb(28, 28, 30));
            ui_text(it_r[i][0] + (int)(10 * u), lty, mitems[menu_idx][i],
                    tcol, 0);
        }
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
    /* macOS-style magnification: smooth distance-based falloff around the
     * dock, not a binary near/far switch */
    double target = 1.0;
    if (in_desktop) {
        int cx = (ddx0 + ddx1) / 2;
        int cy = (ddy0 + ddy1) / 2;
        if (my > ddy0 - (int)(ICON_BASE * u * 1.6)) {
            double dx = mx - cx, dy = my - cy;
            double sig = 150.0 * u;
            double f = exp(-(dx * dx + dy * dy) / (2.0 * sig * sig));
            target = 1.0 + 0.45 * f;
        }
    }
    double k = 1.0 - exp(-dt * 16.0);
    icon_s += (target - icon_s) * k;
    if (fabs(icon_s - 1.0) < 0.003 && target == 1.0)
        icon_s = 1.0;
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

/* ---- scrollback: history lines pushed when the grid scrolls ----
 * sb_off = how many lines the view is above the live bottom (0 = live).
 * Mouse wheel / PgUp / PgDn move the view; any keypress returns to live. */
#define SB_MAX 600
static uint8_t sb_ch[SB_MAX][TCOLS_MAX];
static uint8_t sb_fg[SB_MAX][TCOLS_MAX];
static uint8_t sb_bo[SB_MAX][TCOLS_MAX];
static int sb_head = 0, sb_n = 0;
static int sb_off = 0;
static bool sb_dirty = false;             /* view moved, needs repaint   */

static void sb_push_row(void)
{
    if (sb_n < SB_MAX) {
        sb_n++;
    } else {
        sb_head = (sb_head + 1) % SB_MAX;
    }
    int idx = (sb_head + sb_n - 1) % SB_MAX;
    memcpy(sb_ch[idx], tch, (size_t)t_cols);
    memcpy(sb_fg[idx], tfg, (size_t)t_cols);
    memcpy(sb_bo[idx], tbo, (size_t)t_cols);
    if (sb_off > 0 && sb_off < SB_MAX)
        sb_off++;                             /* keep the view anchored  */
}

static void sb_clear(void)
{
    sb_head = sb_n = sb_off = 0;
}

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
    sb_push_row();
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
    sb_off = 0;
}

/* scroll the history view; clamps and flags a repaint */
static void sb_scroll(int lines)
{
    int max_off = sb_n;
    int v = sb_off + lines;
    if (v > max_off)
        v = max_off;
    if (v < 0)
        v = 0;
    if (v != sb_off) {
        sb_off = v;
        sb_dirty = true;
    }
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
    sb_clear();
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
        setenv("TZ", "MSK-3", 0);        /* Europe/Moscow, no DST */
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
    case KEY_PAGEUP:
        sb_scroll(t_rows - 2);                /* scrollback, like Linux vt */
        return 0;
    case KEY_PAGEDOWN:
        sb_scroll(-(t_rows - 2));
        return 0;
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

    /* baked soft shadow: hugs the rounded silhouette (same radius as the
     * corners), biased slightly downwards like macOS, fading out fully
     * inside the margin - no rectangular frame around the roundings */
    {
        int r = (int)(WIN_R * u);
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        bake_shadow_ring(winbuf, tw_, th_, ox, oy, w, h,
                         (double)r, 15.0 * u, 50.0, 4.0 * u);
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
 * holds only chrome and backdrop, so typing never re-renders it).
 * The visible rows come from the scrollback when the view is scrolled up;
 * a macOS-style scrollbar appears while the view is not at the bottom. */
static void draw_term_content(int wx0, int wy0)
{
    if (sh_fd < 0)
        return;
    int gx = wx0 + (int)(8 * u);
    int gy = wy0 + tw_title + (int)(4 * u);
    for (int r = 0; r < t_rows; r++) {
        int py0 = gy + r * FONT_H;
        /* absolute line index of visible row r (sb_off lines above bottom) */
        int L = sb_n - sb_off + r;
        const uint8_t *ch_row, *fg_row, *bo_row;
        uint8_t blank_fg[TCOLS_MAX];
        if (L >= sb_n + t_rows || L < 0) {
            memset(blank_fg, 255, (size_t)t_cols);
            ch_row = NULL;
            fg_row = blank_fg;
            bo_row = NULL;
        } else if (L < sb_n) {
            int idx = (sb_head + L) % SB_MAX;
            ch_row = sb_ch[idx];
            fg_row = sb_fg[idx];
            bo_row = sb_bo[idx];
        } else {
            int gr = L - sb_n;
            ch_row = tch + (size_t)gr * t_cols;
            fg_row = tfg + (size_t)gr * t_cols;
            bo_row = tbo + (size_t)gr * t_cols;
        }
        if (!ch_row)
            continue;
        for (int c = 0; c < t_cols; c++) {
            uint8_t ch = ch_row[c];
            if (!ch)
                continue;
            if (ch < FONT_FIRST || ch > FONT_FIRST + FONT_COUNT - 1)
                ch = '?';
            const unsigned char (*gl)[FONT_W] =
                (const unsigned char (*)[FONT_W])FONT_BITMAP[ch - FONT_FIRST];
            int fi = fg_row[c];
            uint32_t col = fi == 255 ? TERM_FG_DEFAULT
                           : (bo_row && bo_row[c] && fi < 8 ? PAL[fi + 8]
                                                            : PAL[fi]);
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
    /* caret: thin light bar at the cursor cell (only in the live view) */
    if (caret_on && sb_off == 0) {
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
    /* macOS-style slim scrollbar, only while the view is scrolled up */
    if (sb_off > 0 && sb_n > 0) {
        int total = sb_n + t_rows;
        int area_h = t_rows * FONT_H;
        int tx0 = wx0 + ww - (int)(7 * u);
        int th_h = (int)((double)area_h * t_rows / total);
        if (th_h < (int)(18 * u))
            th_h = (int)(18 * u);
        int ty0 = gy + (int)((double)area_h * (sb_n - sb_off) / total);
        if (ty0 + th_h > gy + area_h)
            ty0 = gy + area_h - th_h;
        for (int y = ty0; y < ty0 + th_h; y++) {
            if (y < cy0 || y > cy1)
                continue;
            uint32_t *d = back + (size_t)y * W;
            for (int x = tx0; x < tx0 + (int)(4 * u); x++) {
                if (x < cx0 || x > cx1)
                    continue;
                uint32_t ob = d[x];
                unsigned rr = ((ob >> 16 & 255) * 97 + 255 * 158) >> 8;
                unsigned gg = ((ob >>  8 & 255) * 97 + 255 * 158) >> 8;
                unsigned bb = ((ob       & 255) * 97 + 255 * 158) >> 8;
                d[x] = rgb(rr, gg, bb);
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
        double e = 1 - (1 - t) * (1 - t) * (1 - t) * (1 - t) * (1 - t);
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
 * per-texel alpha, so every dirty-rect repaint restores both correctly.
 * While dragging, an extra "lift" shadow is drawn under the window - the
 * macOS picking-up feel. */
static void draw_window(void)
{
    int x0, y0, x1, y1, alpha;
    if (!win_cur_rect(&x0, &y0, &x1, &y1, &alpha))
        return;
    if (!tm_anim && alpha >= 256 &&
        winbuf_w == (x1 - x0 + 1) + 2 * SMc &&
        winbuf_h == (y1 - y0 + 1) + 2 * SMc) {
        if (dragging && lift_tex &&
            lift_tex_w == winbuf_w && lift_tex_h == winbuf_h)
            tex_blit_alpha(lift_tex, x0 - SMc, y0 - SMc,
                           lift_tex_w, lift_tex_h, 256);
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
 * the user = screen UP), so screen-space Y needs the negation.
 * The mousedev char format is FIXED 3-byte packets [flags, dx, dy];
 * the wheel arrives separately via evdev REL_WHEEL (wheel_fd). */
/* mousedev byte0 layout: 0x08 (always set) | 0x10 (dx<0) | 0x20 (dy<0)
 * | buttons 0x07.  The sign bits are a NORMAL part of every packet, so
 * only bits 0xC0 mark an impossible byte.  (The old 0xF0 mask rejected
 * every packet with a negative delta, desyncing the whole stream.) */
static bool pkt_start(uint8_t b)
{
    return (b & 0x08) && !(b & 0xC0);
}

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
        ssize_t i = 0;
        /* kernel mousedev (/dev/input/mice) emits FIXED 3-byte packets:
         * [0x08|signs|buttons, dx, dy].  There is no 4-byte wheel variant
         * here (wheel arrives via evdev REL_WHEEL on wheel_fd).  The old
         * "guess 4-byte" heuristic swallowed the next packet's flag byte
         * whenever dx was 0..7, which ate mouse clicks entirely. */
        while (i + 3 <= n) {
            if (!pkt_start(b[i])) {
                i++;                           /* resync on garbage */
                continue;
            }
            int f = b[i];
            int dx = b[i + 1], dy = b[i + 2];
            if (f & 0x10)
                dx -= 256;
            if (f & 0x20)
                dy -= 256;
            i += 3;

            mx += acc(dx);
            my -= acc(dy);                    /* device Y is inverted */
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > W - 1) mx = W - 1;
            if (my > H - 1) my = H - 1;

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

/* ---- mouse wheel via evdev (unambiguous REL_WHEEL events) ---- */
static void wheel_scan(void)
{
    if (wheel_fd >= 0)
        return;
    double t = now();
    if (t - wheel_try < 0.5)
        return;
    wheel_try = t;
    for (int i = 0; i < 8; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        uint8_t bits[(EV_CNT + 7) / 8];
        memset(bits, 0, sizeof(bits));
        if (ioctl(fd, EVIOCGBIT(0, sizeof(bits)), bits) >= 0 &&
            (bits[EV_REL >> 3] & (1 << (EV_REL & 7)))) {
            uint8_t relbits[(REL_CNT + 7) / 8];
            memset(relbits, 0, sizeof(relbits));
            if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) >= 0 &&
                (relbits[REL_WHEEL >> 3] & (1 << (REL_WHEEL & 7)))) {
                wheel_fd = fd;
                fprintf(stderr, "splash: wheel via %s\n", p);
                return;
            }
        }
        close(fd);
    }
}

static void pump_wheel(void)
{
    if (wheel_fd < 0) {
        wheel_scan();
        return;
    }
    uint8_t buf[sizeof(struct input_event) * 16];
    for (int k = 0; k < 8; k++) {
        ssize_t n = read(wheel_fd, buf, sizeof(buf));
        if (n < (ssize_t)sizeof(struct input_event))
            break;
        for (ssize_t off = 0; off + (ssize_t)sizeof(struct input_event) <= n;
             off += sizeof(struct input_event)) {
            struct input_event ev;
            memcpy(&ev, buf + off, sizeof(ev));
            if (ev.type == EV_REL && ev.code == REL_WHEEL && ev.value != 0 &&
                tm == TM_OPEN && !tm_anim) {
                sb_scroll(ev.value > 0 ? 3 : -3);   /* +1 = wheel up */
            }
        }
        if (n < (ssize_t)sizeof(buf))
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

/* menu item activation (all items do real work) */
static void menu_activate(int m, int i);

/* open the dropdown of a menu title */
static void menu_open_at(int m)
{
    menu_idx = m;
    menu_geometry_for(m);
    render_menu_tex();
    menu_open = true;
    menu_closing = false;
    menu_a = 0;
}

static void menu_activate(int m, int i)
{
    fprintf(stderr, "ACT menu %d,%d\n", m, i);
    if (m == 0 && i == 0) {                   /* About AquaOS */
        render_about_tex();
        about_open = true;
        about_dirty = true;
        return;
    }
    if (m == 1 && i == 0) {                   /* New Window */
        if (tm == TM_CLOSED)
            win_open();
        else if (tm == TM_MINIMIZED)
            win_restore();
    } else if (m == 1 && i == 1) {            /* Close Window */
        if (tm == TM_OPEN)
            win_close_start();
    } else if (m == 2 && i == 0) {            /* Minimize */
        if (tm == TM_OPEN)
            win_minimize();
    } else if (m == 2 && i == 1) {            /* Zoom */
        if (tm == TM_OPEN)
            win_zoom();
    }
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

    /* menu takes priority while open (macOS behaviour) */
    if (menu_open) {
        double e = menu_a * menu_a * (3 - 2 * menu_a);
        int oy = (int)((1.0 - e) * -8 * u);
        int n = menu_item_count(menu_idx);
        for (int i = 0; i < n; i++) {
            if (in_rect(x, y, it_r[i][0], it_r[i][1] + oy,
                        it_r[i][2], it_r[i][3] + oy)) {
                if (item_enabled(menu_idx, i)) {
                    menu_open = false;
                    menu_closing = true;
                    menu_activate(menu_idx, i);
                }
                return;                       /* clicked an item row */
            }
        }
        /* another title: switch to it (macOS hover-switching on click) */
        for (int m = 0; m < NMENUS; m++) {
            if (in_rect(x, y, tr[m][0], tr[m][1], tr[m][2], tr[m][3])) {
                if (m != menu_idx) {
                    menu_open_at(m);
                    menu_a = 1.0;             /* swap without re-animating */
                }
                return;
            }
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

    /* window: traffic lights, title-bar drag, double-click zoom */
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
            } else if (y < b + tw_title) {
                if (tm_zoomed) {
                    win_zoom();               /* dragging a zoomed window
                                                 un-zooms it (macOS) */
                } else {
                    double tnow = now();
                    if (last_title_clk > 0 && tnow - last_title_clk < 0.35) {
                        last_title_clk = -1;
                        win_zoom();           /* double-click: zoom */
                    } else {
                        dragging = true;      /* start macOS-style drag */
                        drag_gx = x - a;
                        drag_gy = y - b;
                        last_title_clk = tnow;
                        fprintf(stderr, "ACT drag start\n");
                    }
                }
            }
            return;                            /* clicks inside are consumed */
        }
    }

    /* droplet or any menu title opens its dropdown */
    if (in_rect(x, y, dr_x0 - 3, dr_y0 - 3, dr_x1 + 3, dr_y1 + 3)) {
        fprintf(stderr, "HIT menu (app)\n");
        menu_open_at(0);
        return;
    }
    for (int m = 0; m < NMENUS; m++) {
        if (in_rect(x, y, tr[m][0], tr[m][1], tr[m][2], tr[m][3])) {
            fprintf(stderr, "HIT menu (%s)\n", mtitle[m]);
            menu_open_at(m);
            return;
        }
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
    draw_status_right(1);
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

    /* Europe/Moscow time (MSK, UTC+3, no DST). init normally exports TZ
     * before starting us; keep a fallback for direct launches. */
    setenv("TZ", "MSK-3", 0);
    tzset();

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
    battery_poll();
    bat_next = 2.0;
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
            pump_wheel();
            pump_keyboard();
            if (keyq_n > 0) {
                if (sh_fd >= 0) {
                    sb_scroll(-SB_MAX);       /* typing returns to live */
                    term_write(keyq, keyq_n);
                }
                keyq_n = 0;
            }

            /* macOS-style window drag: 1:1 follow, clamped to the screen */
            if (dragging) {
                if (!btn_l || tm != TM_OPEN || tm_anim) {
                    dragging = false;
                    fprintf(stderr, "ACT drag end\n");
                } else {
                    int nx = mx - drag_gx, ny = my - drag_gy;
                    if (nx < -ww + (int)(80 * u)) nx = -ww + (int)(80 * u);
                    if (nx > W - (int)(80 * u)) nx = W - (int)(80 * u);
                    if (ny < panel_h) ny = panel_h;
                    if (ny > H - tw_title - (int)(6 * u))
                        ny = H - tw_title - (int)(6 * u);
                    if (nx != wx || ny != wy) {
                        wx = nx; wy = ny;
                    }
                }
            }

            /* macOS hover-switching: an open menu follows the pointer to
             * other titles without a click */
            if (menu_open && menu_a >= 0.999 && !about_open) {
                for (int m = 0; m < NMENUS; m++) {
                    if (m != menu_idx &&
                        in_rect(mx, my, tr[m][0], tr[m][1], tr[m][2], tr[m][3])) {
                        extra_rect[0] = dd_x0 - DDMc - 2;
                        extra_rect[1] = dd_y0 - DDMc - 2;
                        extra_rect[2] = dd_x1 + DDMc + 2;
                        extra_rect[3] = dd_y1 + DDMc + 2;
                        extra_dirty = true;
                        menu_open_at(m);
                        menu_a = 1.0;
                        break;
                    }
                }
            }

            dock_update(dt);
            clock_update();
            if (t >= bat_next) {
                battery_poll();
                bat_next = t + 2.0;
            }

            /* menu dropdown animation */
            if (menu_open) {
                menu_a += dt / MENU_MS;
                if (menu_a >= 1.0) { menu_a = 1.0; menu_closing = false; }
            } else if (menu_closing) {
                menu_a -= dt / MENU_MS;
                if (menu_a <= 0.0) { menu_a = 0.0; menu_closing = false; }
            }

            /* caret blink: repaint only the caret cell, not the window */
            if (tm == TM_OPEN && !tm_anim && sh_fd >= 0 && sb_off == 0) {
                bool on = fmod(t, 1.06) < 0.53;
                if (on != caret_on) {
                    caret_on = on;
                    int a, b, c, d, al;
                    if (win_cur_rect(&a, &b, &c, &d, &al)) {
                        int gx = a + (int)(8 * u);
                        int gy = b + tw_title + (int)(4 * u);
                        carect[0] = gx + tcx * FONT_W - 1;
                        carect[1] = gy + tcy * FONT_H - 1;
                        carect[2] = carect[0] + FONT_W + 2;
                        carect[3] = carect[1] + FONT_H + 2;
                        caret_dirty = true;
                    }
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
                if (extra_dirty) {
                    add_rect(extra_rect[0], extra_rect[1],
                             extra_rect[2], extra_rect[3]);
                    extra_dirty = false;
                }
                /* cursor: union of the old and new positions. The scene
                 * redraw inside any rect always recomposes every layer,
                 * so a union is provably ghost-free - even for teleports
                 * (no more full-screen repaints, no more mouse lag). */
                {
                    int ax0 = pmx < mx ? pmx : mx;
                    int ay0 = pmy < my ? pmy : my;
                    int ax1 = pmx > mx ? pmx : mx;
                    int ay1 = pmy > my ? pmy : my;
                    add_rect(ax0 - 3, ay0 - 3, ax1 + cur_w + 3, ay1 + cur_h + 3);
                }

                int fx0, fy0, fx1, fy1;
                fps_bbox(&fx0, &fy0, &fx1, &fy1);
                add_rect(fx0, fy0, fx1, fy1);

                /* menu bar: titles + dropdown */
                bool hover_any = false;
                for (int m = 0; m < NMENUS; m++)
                    if (in_rect(mx, my, tr[m][0], tr[m][1], tr[m][2], tr[m][3]))
                        hover_any = true;
                static bool hover_prev = false;
                if (menu_open || menu_a > 0.001 || menu_closing ||
                    hover_any != hover_prev) {
                    for (int m = 0; m < NMENUS; m++)
                        add_rect(tr[m][0] - 2, tr[m][1], tr[m][2] + 2, tr[m][3]);
                    if (menu_a > 0.001 && menu_idx >= 0) {
                        double e = menu_a * menu_a * (3 - 2 * menu_a);
                        int oy = (int)((1.0 - e) * -8 * u);
                        add_rect(dd_x0 - DDMc - 2, dd_y0 + oy - DDMc - 2,
                                 dd_x1 + DDMc + 2, dd_y1 + oy + DDMc + 2);
                    }
                }
                hover_prev = hover_any;

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
                            prx0 = a; pry0 = b; prx1 = c; pry1 = d;
                        } else {
                            bool rect_changed =
                                (a != prx0 || b != pry0 || c != prx1 || d != pry1);
                            if (rect_changed && prx0 >= 0) {
                                add_rect(prx0 - m, pry0 - m, prx1 + m, pry1 + m);
                                add_rect(a - m, b - m, c + m, d + m);
                            } else if (rect_changed) {
                                add_rect(a - m, b - m, c + m, d + m);
                            } else if (term_dirty) {
                                add_rect(a - m, b - m, c + m, d + m);
                            } else if (sb_dirty) {
                                add_rect(a + 2, b + tw_title, c - 2, d);
                            }
                            prx0 = a; pry0 = b; prx1 = c; pry1 = d;
                        }
                        term_dirty = false;
                        sb_dirty = false;
                    }
                } else {
                    prx0 = pry0 = prx1 = pry1 = -1;
                    sb_dirty = false;
                }

                /* caret blink: tiny repaint of just the caret cell */
                if (caret_dirty) {
                    add_rect(carect[0], carect[1], carect[2], carect[3]);
                    caret_dirty = false;
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
