/*
 * AquaOS boot splash / desktop shell.
 *
 * Renders directly on the Linux framebuffer (/dev/fb0):
 *   - bottom-left: live kernel log lines, read from /dev/kmsg (real data only)
 *   - center:      small white arc spinner, macOS-style
 *   - when init signals /run/bootdone: smooth crossfade to the desktop
 *   - desktop:     wallpaper + empty macOS-style top panel (frosted glass)
 *   - top-right:   live FPS counter (really measured, updated twice a second)
 *
 * No fake data: every log line is a genuine kernel record with its genuine
 * kernel timestamp, the FPS number is a genuine measurement of frames
 * actually rendered. If the system has nothing to say, nothing is displayed.
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
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>

#include "font.h"

/* ---------------- tuning ------------------------------------------- */
#define WALL_PATH        "/usr/share/splash/wallpaper.raw"
#define DONE_PATH        "/run/bootdone"
#define READY_PATH       "/run/splash.ready"
#define KMSG_PATH        "/dev/kmsg"
#define WALL_W           1024
#define WALL_H           768
#define FADE_SECONDS     1.4     /* crossfade duration                */
#define SPIN_PERIOD      1.25    /* seconds per spinner revolution    */
#define REVEAL_PER_FRAME 3       /* log lines revealed per frame      */
#define DISPLAY_LINES    14      /* log lines visible simultaneously  */
#define LOG_MARGIN       16      /* log area margin, px               */
#define LOG_LINE_MAX     160
#define HIST_MAX         512
#define SAFETY_SECONDS   45.0    /* fade even if init never signals   */
#define FRAME_DT         (1.0 / 60.0)
#define FPS_WINDOW       0.5     /* FPS averaging window, seconds     */

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
    fprintf(stderr, "splash: fb0 %dx%d %dbpp (fast32=%d)\n", W, H, BPP * 8, fast32);
    return 0;
}

static inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void blit(void)
{
    if (fast32) {
        memcpy(fb_map, back, (size_t)W * H * 4);
        return;
    }
    for (int y = 0; y < H; y++) {
        const uint32_t *src = back + (size_t)y * W;
        uint8_t *row = fb_map + (size_t)y * finfo.line_length;
        if (BPP == 4) {
            uint32_t *d = (uint32_t *)row;
            for (int x = 0; x < W; x++) {
                uint32_t v = src[x];
                d[x] = (((v >> 16 & 255) >> (8 - rf.length)) << rf.offset) |
                       (((v >>  8 & 255) >> (8 - gf.length)) << gf.offset) |
                       (((v       & 255) >> (8 - bf.length)) << bf.offset);
            }
        } else if (BPP == 3) {
            for (int x = 0; x < W; x++) {
                uint32_t v = src[x];
                uint32_t val = (((v >> 16 & 255) >> (8 - rf.length)) << rf.offset) |
                               (((v >>  8 & 255) >> (8 - gf.length)) << gf.offset) |
                               (((v       & 255) >> (8 - bf.length)) << bf.offset);
                uint8_t *d = row + (size_t)x * 3;
                d[0] = val & 0xff; d[1] = (val >> 8) & 0xff; d[2] = (val >> 16) & 0xff;
            }
        } else if (BPP == 2) {
            uint16_t *d = (uint16_t *)row;
            for (int x = 0; x < W; x++) {
                uint32_t v = src[x];
                d[x] = (((v >> 16 & 255) >> (8 - rf.length)) << rf.offset) |
                       (((v >>  8 & 255) >> (8 - gf.length)) << gf.offset) |
                       (((v       & 255) >> (8 - bf.length)) << bf.offset);
            }
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
        int sy = y * WALL_H / H;
        const uint8_t *s = raw + (size_t)sy * WALL_W * 3;
        uint32_t *d = wall + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            int sx = x * WALL_W / W;
            d[x] = rgb(s[sx * 3], s[sx * 3 + 1], s[sx * 3 + 2]);
        }
    }
    free(raw);
    return 0;
}

/* ---------------- desktop panel --------------------------------------- */
/* macOS-style menu bar: a frosted-glass strip (blurred + lightened wallpaper)
 * with a hairline at the bottom. Empty for now. */

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

static void build_desktop(void)
{
    panel_h = (int)fmax(24.0, H * 0.036);
    if (panel_h > H / 4)
        panel_h = H / 4;
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
    fprintf(stderr, "splash: desktop ready (panel %d px)\n", panel_h);
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

/* ---------------- drawing --------------------------------------------- */
static void draw_char(int x, int y, char ch, uint32_t col)
{
    if (ch < FONT_FIRST || ch > FONT_FIRST + FONT_COUNT - 1)
        return;
    const unsigned char *gl = FONT_BITMAP[ch - FONT_FIRST];
    for (int ry = 0; ry < FONT_H; ry++) {
        unsigned char rowb = gl[ry];
        if (!rowb)
            continue;
        uint32_t *d = back + (size_t)(y + ry) * W + x;
        for (int rx = 0; rx < FONT_W; rx++)
            if (rowb & (0x80 >> rx))
                d[rx] = col;
    }
}

static void draw_text(int x, int y, const char *s, uint32_t col)
{
    int maxc = (W - 2 * LOG_MARGIN) / FONT_W;
    for (int i = 0; s[i] && i < maxc; i++)
        draw_char(x + i * FONT_W, y, s[i], col);
}

/* macOS-like white arc spinner with anti-aliasing and a fading tail */
static void draw_spinner(double cx, double cy, double R, double rot)
{
    double sw = fmax(2.5, R * 0.15);          /* stroke width  */
    double span = M_PI * 0.60;                /* arc length    */
    double track_a = 0.14;
    double lx = cx + cos(rot) * R, ly = cy + sin(rot) * R;
    int x0 = (int)floor(cx - R - sw * 2), x1 = (int)ceil(cx + R + sw * 2);
    int y0 = (int)floor(cy - R - sw * 2), y1 = (int)ceil(cy + R + sw * 2);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            double dx = px + 0.5 - cx, dy = py + 0.5 - cy;
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

static void blend_tex(uint32_t *tex, int A)   /* A: 0..256 */
{
    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint32_t v = back[i], wv = tex[i];
        unsigned r = ((v >> 16 & 255) * (256 - A) + (wv >> 16 & 255) * A) >> 8;
        unsigned g = ((v >>  8 & 255) * (256 - A) + (wv >>  8 & 255) * A) >> 8;
        unsigned b = ((v       & 255) * (256 - A) + (wv       & 255) * A) >> 8;
        back[i] = rgb(r, g, b);
    }
}

/* live FPS readout, top-right corner (inside the panel on the desktop) */
static void draw_fps(int dark)
{
    char buf[32];
    if (!fps_ready)
        return;                               /* no completed window yet */
    snprintf(buf, sizeof(buf), "%d FPS", fps_now);
    int len = 0;
    while (buf[len])
        len++;
    int x = W - LOG_MARGIN - len * FONT_W;
    int y = (panel_h - FONT_H) / 2;
    if (y < 2)
        y = 2;
    draw_text(x, y, buf, dark ? rgb(60, 60, 67) : rgb(168, 170, 176));
}

/* ---------------- main -------------------------------------------------- */
enum St { ST_BOOT, ST_FADE, ST_WALL };

int main(void)
{
    signal(SIGHUP, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);

    kfd = open(KMSG_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kfd < 0)
        fprintf(stderr, "splash: cannot open kmsg: %s\n", strerror(errno));

    if (fb_open() != 0) {
        fprintf(stderr, "splash: no framebuffer\n");
        return 1;
    }
    if (wall_load() != 0)
        fprintf(stderr, "splash: wallpaper missing, will fade to black\n");
    build_desktop();

    double t0 = now(), last = t0, rot = 0, fade0 = 0;
    double fps_t0 = t0;
    int fps_frames = 0;
    enum St st = ST_BOOT;
    bool ready = false;

    for (;;) {
        double t = now();
        double rdt = t - last;
        last = t;
        double dt = (rdt <= 0 || rdt > 0.25) ? FRAME_DT : rdt;
        rot += dt * 2.0 * M_PI / SPIN_PERIOD;

        /* real FPS meter: frames actually rendered per second */
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
        }

        /* compose frame */
        for (size_t i = 0; i < (size_t)W * H; i++)
            back[i] = 0;

        int nl = reveal_n < DISPLAY_LINES ? reveal_n : DISPLAY_LINES;
        int first = reveal_n - nl;
        int ty = H - LOG_MARGIN - nl * FONT_H;
        for (int i = 0; i < nl; i++)
            draw_text(LOG_MARGIN, ty + i * FONT_H, hist[first + i].text,
                      rgb(LOG_R, LOG_G, LOG_B));

        draw_spinner(W * 0.5, H * 0.5, fmax(16.0, H * 0.032), rot);

        if (st == ST_FADE && desktop) {
            double a = (t - fade0) / FADE_SECONDS;
            if (a >= 1.0) {
                st = ST_WALL;
                blend_tex(desktop, 256);
            } else {
                a = a * a * (3 - 2 * a);          /* smoothstep easing */
                blend_tex(desktop, (int)(a * 256.0 + 0.5));
            }
        } else if (st == ST_WALL && desktop) {
            memcpy(back, desktop, (size_t)W * H * 4);
        }

        draw_fps(st == ST_BOOT ? 0 : 1);

        blit();

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
