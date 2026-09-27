/* ===========================================================================
 *  raycaster.c -- a pseudo-3D maze you can walk around, rendered entirely
 *                 inside a text terminal.
 *
 *  Everything is written from scratch on top of libc + POSIX:
 *
 *    * a DDA grid raycaster (Wolfenstein-3D style): textured walls with
 *      distance shading and fog, a per-pixel perspective floor/ceiling cast,
 *      and alpha-tested billboard sprites sorted back-to-front against a
 *      per-column z-buffer,
 *    * all textures are generated procedurally at start-up (64x64 pixels),
 *      so there are no asset files at all,
 *    * a recursive-backtracker maze generator with braiding, a central
 *      pillared hall and per-zone wall materials,
 *    * an ANSI renderer that packs TWO vertical pixels into ONE character
 *      cell using the UTF-8 UPPER HALF BLOCK glyph, with 24-bit or
 *      xterm-256 colour and escape-sequence elision,
 *    * a raw-mode terminal front end (termios) with non-blocking input,
 *      SIGWINCH resize handling and guaranteed terminal restore on exit.
 *
 *  Build:
 *      cc -O2 -std=c99 -Wall -Wextra -o raycaster raycaster.c -lm
 *      (or simply: make)
 *
 *  Run:
 *      ./raycaster                     play
 *      ./raycaster --help              list every option
 *      ./raycaster --demo              watch the autopilot walk the maze
 *      ./raycaster --headless --frames 300     benchmark (no tty needed)
 *      ./raycaster --seed 7 --size 41 --fov 75
 * ===========================================================================
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>          /* web port: rAF main loop + JS exports     */
#endif

/* ------------------------------------------------------------------ shapes */
#define TEXW      64                 /* texture width  (power of two)       */
#define TEXH      64                 /* texture height (power of two)       */
#define TEXMASK   (TEXW - 1)         /* wrap mask for texture coordinates   */
#define MAPMAX    64                 /* largest maze we can store           */
#define MAXSPR    192                /* largest number of billboard sprites */
#define KEYHOLD   0.16               /* seconds a key event counts as "held"*/
#define PI        3.14159265358979323846

/* fog colour the world fades into */
#define FOG_R 11.0
#define FOG_G 15.0
#define FOG_B 21.0

typedef uint32_t u32;
typedef uint8_t  u8;

static inline double dclamp(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }
static inline int    iclamp(int v, int a, int b)          { return v < a ? a : (v > b ? b : v); }

static inline u32 pack_rgb(int r, int g, int b)
{
    return ((u32)iclamp(r, 0, 255) << 16)
         | ((u32)iclamp(g, 0, 255) << 8)
         |  (u32)iclamp(b, 0, 255);
}

/* --------------------------------------------------------------- clock ---- */
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#ifndef __EMSCRIPTEN__
static void sleep_sec(double s)
{
    struct timespec ts;
    if (s <= 0.0) return;
    ts.tv_sec  = (time_t)s;
    ts.tv_nsec = (long)((s - (double)(time_t)s) * 1e9);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;                                   /* finish the nap if interrupted */
}
#endif                                   /* web: rAF paces the frame          */

/* --------------------------------------------------------------- random --- */
static u32 g_rng = 1u;

static void rng_seed(u32 s)   { g_rng = s ? s : 0x9e3779b9u; }
static u32  rng_next(void)    { u32 x = g_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return g_rng = x; }
static double rng_d(void)     { return (double)(rng_next() >> 8) * (1.0 / 16777216.0); }
static int  rng_range(int a, int b) { return a + (int)(rng_d() * (double)(b - a)); }

/* --------------------------------------------------------------- noise ---- */
static u32 hash_u32(u32 x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static double noise2(int x, int y, u32 seed)
{
    u32 h = hash_u32((u32)x * 0x9e3779b1u ^ (u32)y * 0x85ebca6bu ^ seed * 0xc2b2ae35u);
    return (double)(h & 0xffffu) / 65535.0;
}

/* smooth (bilinearly interpolated) value noise, returns 0..1 */
static double fnoise2(double x, double y, u32 seed)
{
    int    xi = (int)floor(x), yi = (int)floor(y);
    double xf = x - (double)xi, yf = y - (double)yi;
    double u  = xf * xf * (3.0 - 2.0 * xf);
    double v  = yf * yf * (3.0 - 2.0 * yf);
    double a  = noise2(xi,     yi,     seed);
    double b  = noise2(xi + 1, yi,     seed);
    double c  = noise2(xi,     yi + 1, seed);
    double d  = noise2(xi + 1, yi + 1, seed);
    return (a * (1.0 - u) + b * u) * (1.0 - v) + (c * (1.0 - u) + d * u) * v;
}

/* --------------------------------------------------------------- colour --- */
/* Distance shading + fog.  `lum` may exceed 1.0 to model emissive surfaces. */
static u32 apply_light(u32 c, double lum, double fog)
{
    double r = (double)((c >> 16) & 255u);
    double g = (double)((c >>  8) & 255u);
    double b = (double)( c        & 255u);

    lum = dclamp(lum, 0.0, 1.8);
    r *= lum; g *= lum; b *= lum;

    if (fog > 0.0) {
        fog = dclamp(fog, 0.0, 1.0);
        r += (FOG_R - r) * fog;
        g += (FOG_G - g) * fog;
        b += (FOG_B - b) * fog;
    }
    return pack_rgb((int)(r + 0.5), (int)(g + 0.5), (int)(b + 0.5));
}

/* 24-bit RGB -> nearest xterm-256 palette entry (6x6x6 cube / grey ramp). */
static int to_ansi256(u32 c)
{
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    int r = (int)((c >> 16) & 255u), g = (int)((c >> 8) & 255u), b = (int)(c & 255u);
    int ir, ig, ib, dr, dg, db, grey, gv, gr, gg, gb;
    long cube, ramp;

    ir = r < 48 ? 0 : (r < 115 ? 1 : (r - 35) / 40);
    ig = g < 48 ? 0 : (g < 115 ? 1 : (g - 35) / 40);
    ib = b < 48 ? 0 : (b < 115 ? 1 : (b - 35) / 40);
    ir = iclamp(ir, 0, 5); ig = iclamp(ig, 0, 5); ib = iclamp(ib, 0, 5);

    dr = r - lv[ir]; dg = g - lv[ig]; db = b - lv[ib];
    cube = (long)dr * dr + (long)dg * dg + (long)db * db;

    grey = iclamp((int)(((r + g + b) / 3.0 - 8.0) / 10.0 + 0.5), 0, 23);
    gv   = 8 + 10 * grey;
    gr = r - gv; gg = g - gv; gb = b - gv;
    ramp = (long)gr * gr + (long)gg * gg + (long)gb * gb;

    if (ramp < cube) return 232 + grey;
    return 16 + 36 * ir + 6 * ig + ib;
}

/* ------------------------------------------------------- growable buffer -- */
typedef struct { char *data; size_t len, cap; } Buf;

static void buf_init(Buf *b, size_t cap)
{
    b->data = (char *)malloc(cap);
    if (!b->data) { fprintf(stderr, "raycaster: out of memory\n"); exit(1); }
    b->len = 0; b->cap = cap;
}
static void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static void buf_reserve(Buf *b, size_t extra)
{
    size_t nc;
    char  *nd;
    if (b->len + extra <= b->cap) return;
    nc = b->cap ? b->cap : 1024;
    while (nc < b->len + extra) nc *= 2;
    nd = (char *)realloc(b->data, nc);
    if (!nd) { fprintf(stderr, "raycaster: out of memory\n"); exit(1); }
    b->data = nd; b->cap = nc;
}
static void buf_put(Buf *b, const char *s, size_t n) { buf_reserve(b, n); memcpy(b->data + b->len, s, n); b->len += n; }
static void buf_puts(Buf *b, const char *s)          { buf_put(b, s, strlen(s)); }
static void buf_putc(Buf *b, char c)                 { buf_reserve(b, 1); b->data[b->len++] = c; }

static void buf_printf(Buf *b, const char *fmt, ...)
{
    char    tmp[256];
    va_list ap;
    int     n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { buf_put(b, tmp, (size_t)n); return; }
    buf_reserve(b, (size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

/* ==========================================================================
 *  PROCEDURAL TEXTURES
 * ========================================================================== */
enum { T_NONE = 0, T_BRICK, T_STONE, T_WOOD, T_TECH, T_GLOW, T_TYPES };
enum { S_ORB = 0, S_PILLAR, S_TORCH, S_EXIT, S_TYPES };

static u32 g_wall[T_TYPES][TEXW * TEXH];
static u32 g_floor[TEXW * TEXH];
static u32 g_ceil[TEXW * TEXH];
static u32 g_sprite[S_TYPES][TEXW * TEXH];   /* 0 == transparent pixel */

static void gen_wall_brick(void)
{
    int x, y;
    for (y = 0; y < TEXH; y++) {
        int row = y >> 4;                        /* bricks are 16 px tall   */
        int off = (row & 1) ? 16 : 0;            /* running bond offset     */
        for (x = 0; x < TEXW; x++) {
            int    bx = (x + off) & 31;          /* 32 px wide              */
            int    by = y & 15;
            int    mortar = (by < 2) || (bx < 2);
            double n = fnoise2(x * 0.35, y * 0.35, 11u);
            int    brick_id = ((x + off) >> 5) + row * 5;
            double v = 0.80 + 0.40 * noise2(brick_id, row, 5u);
            double dseam, shade;
            int    r, g, b;

            if (mortar) {
                int m = 62 + (int)(n * 28.0);
                g_wall[T_BRICK][y * TEXW + x] = pack_rgb(m, m, m - 5);
                continue;
            }
            /* distance from the nearest mortar line gives the bricks depth */
            dseam = (double)((by - 2) < (bx - 2) ? (by - 2) : (bx - 2));
            shade = 0.60 + 0.40 * dclamp(dseam / 5.0, 0.0, 1.0);

            r = (int)(152.0 * v * shade + n * 16.0);
            g = (int)( 66.0 * v * shade + n * 12.0);
            b = (int)( 54.0 * v * shade + n * 10.0);
            g_wall[T_BRICK][y * TEXW + x] = pack_rgb(r, g, b);
        }
    }
}

static void gen_wall_stone(void)
{
    int x, y;
    for (y = 0; y < TEXH; y++) {
        for (x = 0; x < TEXW; x++) {
            int    bx = x & 15, by = y & 15;
            int    seam = (bx < 2) || (by < 2);
            double n = fnoise2(x * 0.55, y * 0.55, 23u) * 0.55
                     + fnoise2(x * 0.13, y * 0.13, 29u) * 0.45;
            double dseam = (double)(by < 2 ? by : (bx < 2 ? bx : 8));
            double shade = 0.72 + 0.28 * dclamp(dseam / 4.0, 0.0, 1.0);
            int    base  = seam ? 46 : (int)(88.0 + 78.0 * n);
            int    r = (int)(base * shade * 0.97);
            int    g = (int)(base * shade);
            int    b = (int)(base * shade * 1.06);
            g_wall[T_STONE][y * TEXW + x] = pack_rgb(r, g, b);
        }
    }
}

static void gen_wall_wood(void)
{
    int x, y;
    for (y = 0; y < TEXH; y++) {
        for (x = 0; x < TEXW; x++) {
            int    px    = x & 15;                        /* plank width    */
            int    plank = x >> 4;
            double n     = fnoise2(x * 0.25, y * 0.9, 41u);
            double grain = 0.5 + 0.5 * sin(y * 0.55 + n * 6.0 + plank * 2.1);
            double shade = (px < 1 || px > 14) ? 0.42 : (0.80 + 0.30 * grain);
            int    r, g, b;

            if ((y & 31) < 2) shade *= 0.62;              /* plank joint     */
            r = (int)(146.0 * shade + n * 14.0);
            g = (int)( 94.0 * shade + n * 10.0);
            b = (int)( 50.0 * shade + n *  8.0);
            g_wall[T_WOOD][y * TEXW + x] = pack_rgb(r, g, b);
        }
    }
}

static void gen_wall_tech(void)
{
    static const int rvx[4] = { 4, 27, 4, 27 };
    static const int rvy[4] = { 4, 4, 27, 27 };
    int x, y, i;
    for (y = 0; y < TEXH; y++) {
        int by = y & 31;
        for (x = 0; x < TEXW; x++) {
            int    bx = x & 31;
            double n  = fnoise2(x * 0.30, y * 0.30, 61u);
            int    base;
            u32    c;

            if (bx < 2 || by < 2 || bx > 29 || by > 29) {
                c = pack_rgb(72 + (int)(n * 10.0), 84 + (int)(n * 10.0), 102 + (int)(n * 12.0));
            } else {
                base = 40 + (int)(n * 16.0);
                c = pack_rgb(base, base + 9, base + 24);
                if (by >= 13 && by <= 17 && bx >= 7 && bx <= 24)     /* light strip */
                    c = pack_rgb(34, 138, 176);
            }
            for (i = 0; i < 4; i++) {                                /* rivets      */
                double dx = (double)(bx - rvx[i]), dy = (double)(by - rvy[i]);
                if (dx * dx + dy * dy <= 3.2) {
                    int m = 150 + (int)(n * 20.0);
                    c = pack_rgb(m, m + 6, m + 18);
                }
            }
            g_wall[T_TECH][y * TEXW + x] = c;
        }
    }
}

static void gen_wall_glow(void)
{
    int x, y;
    for (y = 0; y < TEXH; y++) {
        for (x = 0; x < TEXW; x++) {
            double dx   = (double)x - 31.5, dy = (double)y - 31.5;
            double dist = sqrt(dx * dx + dy * dy);
            double pat  = sin(x * 0.30) * sin(y * 0.30) + sin((x + y) * 0.21) * 0.6;
            double ring = fabs(dist - 22.0);
            double n    = fnoise2(x * 0.6, y * 0.6, 131u);
            u32    c;

            if (ring < 2.0 || pat > 0.35) {
                /* runes glow brighter the closer they are to the centre */
                double i = 0.55 + 0.45 * (1.0 - dclamp(dist / 32.0, 0.0, 1.0));
                c = pack_rgb((int)(46 * i + 20), (int)(228 * i + 24), (int)(150 * i + 30));
            } else {
                int base = 22 + (int)(n * 16.0);
                c = pack_rgb(base, base + 6, base + 2);
            }
            g_wall[T_GLOW][y * TEXW + x] = c;
        }
    }
}

static void gen_floor_tile(void)
{
    int x, y;
    for (y = 0; y < TEXH; y++) {
        for (x = 0; x < TEXW; x++) {
            int    bx = x & 31, by = y & 31;
            int    grout = (bx < 2) || (by < 2);
            int    alt   = ((x >> 5) + (y >> 5)) & 1;
            double n     = fnoise2(x * 0.7, y * 0.7, 77u);
            int    base  = grout ? 28 : (alt ? 60 : 49);

            g_floor[y * TEXW + x] = pack_rgb(base + (int)(n * 16.0),
                                             base + (int)(n * 16.0),
                                             base + 7 + (int)(n * 18.0));
        }
    }
}

static void gen_ceil_tile(void)
{
    int x, y;
    for (y = 0; y < TEXH; y++) {
        for (x = 0; x < TEXW; x++) {
            double n    = fnoise2(x * 0.25, y * 0.25, 91u);
            double spec = noise2(x, y, 97u);
            int    base = 15 + (int)(n * 12.0);
            u32    c    = pack_rgb(base, base + 2, base + 9);

            if (spec > 0.9935) c = pack_rgb(140, 150, 185);   /* faint speck */
            g_ceil[y * TEXW + x] = c;
        }
    }
}

/* --- billboards ---------------------------------------------------------- */

static void gen_sprite_orb(void)
{
    int x, y;
    for (y = 0; y < TEXH; y++) {
        for (x = 0; x < TEXW; x++) {
            double dx = (double)x - 31.5, dy = (double)y - 31.5;
            double d  = sqrt(dx * dx + dy * dy) / 30.0;
            u32    c  = 0;                                    /* transparent */
            if (d < 1.0) {
                double i   = pow(1.0 - d, 1.7);
                double rim = 0.55 + 0.45 * (0.5 + 0.5 * sin(atan2(dy, dx) * 3.0 + 0.6));
                c = pack_rgb(30 + (int)(90.0 * i * rim),
                             150 + (int)(100.0 * i),
                             200 + (int)(55.0 * i));
            }
            g_sprite[S_ORB][y * TEXW + x] = c;
        }
    }
}

static void gen_sprite_pillar(void)
{
    int x, y;
    for (y = 0; y < TEXH; y++) {
        double cap   = (y < 5 || y > 58) ? 5.0 : 2.0;         /* flares      */
        int    halfw = (int)(11.0 + cap);
        for (x = 0; x < TEXW; x++) {
            double t = fabs((double)(x - 32)) / (double)halfw;
            u32    c = 0;
            if (t <= 1.0) {
                double n     = fnoise2(x * 0.4, y * 0.4, 55u);
                double shade = 0.50 + 0.50 * (1.0 - t * t);   /* round column*/
                if ((x & 7) == 0) shade *= 0.80;              /* fluting     */
                if (y > 58)       shade *= 0.80;              /* base shadow */
                c = pack_rgb((int)(132.0 * shade + n * 26.0),
                             (int)(126.0 * shade + n * 24.0),
                             (int)(116.0 * shade + n * 22.0));
            }
            g_sprite[S_PILLAR][y * TEXW + x] = c;
        }
    }
}

static void gen_sprite_torch(void)
{
    int x, y;
    for (y = 0; y < TEXH; y++) {
        for (x = 0; x < TEXW; x++) {
            double fx = (double)x - 31.5;                     /* flame centre */
            double fy = ((double)y - 26.0) * 1.35;
            double d  = sqrt(fx * fx + fy * fy) / 24.0;
            double n  = fnoise2(x * 0.5, y * 0.5, 171u);
            u32    c  = 0;

            if (y > 46 && y < 58 && x > 27 && x < 37) {        /* handle      */
                int m = 70 + (int)(n * 40.0);
                c = pack_rgb(m, m - 14, m - 30);
            } else if (y >= 58 && x > 24 && x < 40) {          /* bracket     */
                int m = 96 + (int)(n * 30.0);
                c = pack_rgb(m, m - 6, m - 18);
            } else if (d < 1.0) {
                double i = pow(1.0 - d, 0.85) * (0.82 + 0.36 * n);
                c = pack_rgb((int)(250.0 * dclamp(i, 0.0, 1.0) + 5.0),
                             (int)(210.0 * dclamp(i * i, 0.0, 1.0) + 12.0 * i),
                             (int)( 90.0 * dclamp(i * i * i, 0.0, 1.0)));
                if (c == 0) c = 1;
            }
            g_sprite[S_TORCH][y * TEXW + x] = c;
        }
    }
}

static void gen_sprite_exit(void)
{
    int x, y;
    for (y = 0; y < TEXH; y++) {
        for (x = 0; x < TEXW; x++) {
            double fx = fabs((double)x - 31.5);
            u32    c = 0;                                    /* transparent */
            if (fx < 6.0) {                                 /* bright core   */
                double i = 1.0 - fx / 6.0;
                c = pack_rgb((int)(60.0 + 60.0 * i),
                             (int)(200.0 + 55.0 * i),
                             (int)(120.0 + 60.0 * i));
            } else if (fx < 14.0) {                         /* green posts   */
                double n = fnoise2(x * 0.5, y * 0.5, 199u);
                int m = 40 + (int)(n * 40.0);
                c = pack_rgb(m / 3, m + 60, m / 2 + 40);
            }
            if (c == 0) c = 0;                              /* keep alpha 0  */
            g_sprite[S_EXIT][y * TEXW + x] = c;
        }
    }
}

static void gen_textures(void)
{
    gen_wall_brick();
    gen_wall_stone();
    gen_wall_wood();
    gen_wall_tech();
    gen_wall_glow();
    gen_floor_tile();
    gen_ceil_tile();
    gen_sprite_orb();
    gen_sprite_pillar();
    gen_sprite_torch();
    gen_sprite_exit();
}

/* ==========================================================================
 *  WORLD: maze generation, spawn point, sprite placement
 * ========================================================================== */
typedef struct {
    double x, y;        /* map position                                */
    int    type;        /* S_ORB / S_PILLAR / S_TORCH                  */
    double scale;       /* size multiplier                             */
    double vmove;       /* vertical offset (screen px at distance 1)   */
    double glow;        /* 0 = plain, >0 = emissive                    */
} Sprite;

typedef struct {
    int    w, h;                               /* active maze dimensions   */
    u8     grid[MAPMAX][MAPMAX];               /* 0 = walkable, else wall  */
    Sprite spr[MAXSPR];
    int    nspr;
    double spawn_x, spawn_y, spawn_dir;
    int    exit_x, exit_y;                     /* walkable cell just inside the gate */
    double exit_wx, exit_wy;                   /* world pos of the exit gate */
    int    exit_dist;                          /* BFS steps spawn -> exit  */
} World;

static int cell_solid(const World *wo, int x, int y)
{
    if (x < 0 || y < 0 || x >= wo->w || y >= wo->h) return 1;
    return wo->grid[y][x] != 0;
}

static int cell_solid_exit(const World *wo, int x, int y)
{
    /* the exit gate cell itself is always enterable (even on the border) */
    if (wo->exit_x > 0 && x == (int)wo->exit_wx && y == (int)wo->exit_wy) return 0;
    if (wo->exit_x > 0 && x == wo->exit_x && y == wo->exit_y) return 0;
    if (x < 0 || y < 0 || x >= wo->w || y >= wo->h) return 1;
    return wo->grid[y][x] != 0;
}

/* Can a disc of radius r sit centred on (x,y)?  Samples the 8 extreme points
 * of the bounding box, which is accurate enough for r < 0.5.  The exit gate
 * border cell counts as open so the player can step through it.            */
static int can_move_exit(const World *wo, double x, double y, double r)
{
    static const double ox[8] = { -1.0,  1.0, -1.0,  1.0,  0.0,  0.0, -1.0,  1.0 };
    static const double oy[8] = { -1.0, -1.0,  1.0,  1.0, -1.0,  1.0,  0.0,  0.0 };
    int i;
    for (i = 0; i < 8; i++)
        if (cell_solid_exit(wo, (int)floor(x + ox[i] * r), (int)floor(y + oy[i] * r)))
            return 0;
    return 1;
}

static void world_generate(World *wo, int w, int h, u32 seed)
{
    static const u8  lut[4] = { T_STONE, T_BRICK, T_TECH, T_WOOD };
    static const int dx[4]  = { 0, 0, -2, 2 };
    static const int dy[4]  = { -2, 2, 0, 0 };
    static int sx[MAPMAX * MAPMAX], sy[MAPMAX * MAPMAX];
    int    sp = 0, x, y, k, cx, cy, attempt;
    int    rw, rh, rx, ry, want;
    int    bx, by;
    double best, cxr, cyr;

    /* keep the maze odd sized and inside our storage */
    w = iclamp(w, 9, MAPMAX);
    h = iclamp(h, 9, MAPMAX);
    if (!(w & 1)) w--;
    if (!(h & 1)) h--;
    wo->w = w;
    wo->h = h;
    rng_seed(seed);
    memset(wo->grid, T_STONE, sizeof wo->grid);          /* solid stone     */

    /* ---- 1. recursive backtracker on the odd lattice -------------------- */
    wo->grid[1][1] = 0;
    sx[sp] = 1; sy[sp] = 1; sp++;
    while (sp > 0) {
        int cand[4], nc = 0, pick, nx, ny;
        cx = sx[sp - 1];
        cy = sy[sp - 1];
        for (k = 0; k < 4; k++) {
            nx = cx + dx[k]; ny = cy + dy[k];
            if (nx > 0 && ny > 0 && nx < w - 1 && ny < h - 1 && wo->grid[ny][nx] != 0)
                cand[nc++] = k;
        }
        if (nc == 0) { sp--; continue; }
        pick = cand[rng_range(0, nc)];
        nx = cx + dx[pick]; ny = cy + dy[pick];
        wo->grid[(cy + ny) / 2][(cx + nx) / 2] = 0;      /* knock the wall  */
        wo->grid[ny][nx] = 0;
        sx[sp] = nx; sy[sp] = ny; sp++;
    }

    /* ---- 2. braid: punch extra holes so the maze has loops -------------- */
    for (cy = 1; cy < h - 1; cy += 2) {
        for (cx = 1; cx < w - 1; cx += 2) {
            int closed[4], ncl = 0, open = 0;
            if (wo->grid[cy][cx] != 0) continue;
            for (k = 0; k < 4; k++) {
                int nx = cx + dx[k] / 2, ny = cy + dy[k] / 2;
                if (nx <= 0 || ny <= 0 || nx >= w - 1 || ny >= h - 1) continue;
                if (wo->grid[ny][nx] != 0) closed[ncl++] = k;
                else open++;
            }
            if (open <= 1 && ncl > 0 && rng_d() < 0.55) {
                int pick = closed[rng_range(0, ncl)];
                wo->grid[cy + dy[pick] / 2][cx + dx[pick] / 2] = 0;
            }
        }
    }

    /* ---- 3. central pillared hall --------------------------------------- */
    rw = ((w - 4) / 3) | 1;
    rh = ((h - 4) / 3) | 1;
    rx = (w - rw) / 2;
    ry = (h - rh) / 2;
    for (y = ry; y < ry + rh; y++)
        for (x = rx; x < rx + rw; x++)
            wo->grid[y][x] = 0;
    for (y = ry + 1; y < ry + rh - 1; y += 2)
        for (x = rx + 1; x < rx + rw - 1; x += 2)
            wo->grid[y][x] = T_STONE;

    /* ---- 4. pick wall materials ----------------------------------------- */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            u8  t;
            int visible = 0;
            if (wo->grid[y][x] == 0) continue;
            for (k = 0; k < 4; k++) {
                int nx = x + dx[k] / 2, ny = y + dy[k] / 2;
                if (nx >= 0 && ny >= 0 && nx < w && ny < h && wo->grid[ny][nx] == 0)
                    visible = 1;
            }
            if (!visible) { wo->grid[y][x] = T_STONE; continue; }
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
                t = T_BRICK;                                   /* outer shell  */
            else
                t = lut[((x / 7) + (y / 7) * 2) & 3];          /* material zone*/
            if (x >= rx - 1 && x <= rx + rw && y >= ry - 1 && y <= ry + rh
                && noise2(x, y, 123u) > 0.42)
                t = T_GLOW;                                    /* hall runes   */
            wo->grid[y][x] = t;
        }
    }

    /* ---- 5. spawn in the hall, facing somewhere open -------------------- */
    best = 1e18; bx = rx + rw / 2; by = ry + rh / 2;
    cxr = (double)rx + rw / 2.0;
    cyr = (double)ry + rh / 2.0;
    for (y = ry; y < ry + rh; y++) {
        for (x = rx; x < rx + rw; x++) {
            double d;
            if (wo->grid[y][x] != 0) continue;
            d = ((double)x - cxr) * ((double)x - cxr) + ((double)y - cyr) * ((double)y - cyr);
            if (d < best) { best = d; bx = x; by = y; }
        }
    }
    wo->spawn_x   = (double)bx + 0.5;
    wo->spawn_y   = (double)by + 0.5;
    wo->spawn_dir = 0.0;
    for (k = 0; k < 4; k++) {
        int nx = bx + dx[k] / 2 * 3, ny = by + dy[k] / 2 * 3;
        if (!cell_solid(wo, nx, ny)) {
            wo->spawn_dir = atan2((double)dy[k], (double)dx[k]);
            break;
        }
    }

    /* ---- 6a. carve an exit: farthest reachable border-adjacent cell ----- */
    {
        static int dist[MAPMAX][MAPMAX];
        static int qx[MAPMAX * MAPMAX], qy[MAPMAX * MAPMAX];
        int qh = 0, qt = 0;
        int sx0 = (int)wo->spawn_x, sy0 = (int)wo->spawn_y;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                dist[y][x] = -1;
        if (sx0 >= 0 && sy0 >= 0 && sx0 < w && sy0 < h && wo->grid[sy0][sx0] == 0) {
            dist[sy0][sx0] = 0;
            qx[qt] = sx0; qy[qt] = sy0; qt++;
        }
        while (qh < qt) {
            int cx0 = qx[qh], cy0 = qy[qh]; qh++;
            for (k = 0; k < 4; k++) {
                int nx = cx0 + dx[k] / 2, ny = cy0 + dy[k] / 2;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                if (wo->grid[ny][nx] != 0) continue;
                if (dist[ny][nx] != -1) continue;
                dist[ny][nx] = dist[cy0][cx0] + 1;
                qx[qt] = nx; qy[qt] = ny; qt++;
            }
        }
        /* pick the farthest open cell that touches the outer shell */
        wo->exit_x = -1; wo->exit_y = -1; wo->exit_dist = 0;
        for (y = 1; y < h - 1; y++) {
            for (x = 1; x < w - 1; x++) {
                int touches = (x == 1 || y == 1 || x == w - 2 || y == h - 2);
                if (!touches) continue;
                if (wo->grid[y][x] != 0) continue;
                if (dist[y][x] < 0) continue;
                if (dist[y][x] > wo->exit_dist) {
                    wo->exit_dist = dist[y][x];
                    wo->exit_x = x; wo->exit_y = y;
                }
            }
        }
        /* open the outer wall right beside it so the gate leads outside.
         * Retarget the exit onto the BORDER cell itself: that is the cell
         * the player must step into to win, and the BFS target. */
        if (wo->exit_x >= 0) {
            int bx2 = wo->exit_x, by2 = wo->exit_y;
            if (wo->exit_x == 1)            bx2 = 0;
            else if (wo->exit_x == w - 2)   bx2 = w - 1;
            else if (wo->exit_y == 1)       by2 = 0;
            else                            by2 = h - 1;
            wo->grid[by2][bx2] = 0;                    /* the gate itself */
            wo->grid[wo->exit_y][wo->exit_x] = 0;
            wo->exit_x = bx2; wo->exit_y = by2;
            wo->exit_wx = (double)bx2 + 0.5;
            wo->exit_wy = (double)by2 + 0.5;
        } else {   /* tiny degenerate maze: fall back to spawn */
            wo->exit_x = sx0; wo->exit_y = sy0;
            wo->exit_wx = wo->spawn_x; wo->exit_wy = wo->spawn_y;
        }
    }

    /* ---- 6. scatter billboards ------------------------------------------ */
    wo->nspr = 0;
    want = (w * h) / 70 + 6;
    for (attempt = 0; attempt < want * 14 && wo->nspr < MAXSPR; attempt++) {
        int     mx = rng_range(1, w - 1), my = rng_range(1, h - 1);
        double  dxw, dyw, r;
        Sprite *s;
        int     dup = 0, i;

        if (wo->grid[my][mx] != 0) continue;
        dxw = (double)mx + 0.5 - wo->spawn_x;
        dyw = (double)my + 0.5 - wo->spawn_y;
        if (dxw * dxw + dyw * dyw < 16.0) continue;          /* keep it clear */
        dxw = (double)mx - (double)wo->exit_x;
        dyw = (double)my - (double)wo->exit_y;
        if (dxw * dxw + dyw * dyw < 9.0) continue;           /* keep gate clear */
        for (i = 0; i < wo->nspr; i++)
            if ((int)wo->spr[i].x == mx && (int)wo->spr[i].y == my) dup = 1;
        if (dup) continue;

        s = &wo->spr[wo->nspr++];
        s->x = (double)mx + 0.5 + (rng_d() - 0.5) * 0.3;
        s->y = (double)my + 0.5 + (rng_d() - 0.5) * 0.3;
        r = rng_d();
        if (r < 0.44) {
            s->type = S_ORB;    s->scale = 0.62; s->vmove = 16.0; s->glow = 0.80;
        } else if (r < 0.76) {
            s->type = S_TORCH;  s->scale = 0.72; s->vmove = 26.0; s->glow = 0.95;
        } else {
            s->type = S_PILLAR; s->scale = 1.10; s->vmove =  0.0; s->glow = 0.0;
        }
    }
    /* the exit gate itself gets an unmissable green beacon */
    if (wo->nspr < MAXSPR && wo->exit_x >= 0) {
        Sprite *s = &wo->spr[wo->nspr++];
        s->x = wo->exit_wx; s->y = wo->exit_wy;
        s->type = S_EXIT; s->scale = 1.30; s->vmove = 0.0; s->glow = 1.0;
    }
}

/* ==========================================================================
 *  PLAYER, SCREEN, TERMINAL
 * ========================================================================== */
typedef struct { double x, y, dirx, diry, planex, planey; } Player;

enum { CH_HALF = 0 };     /* ch == CH_HALF means "draw the upper half block" */

typedef struct {
    unsigned char ch;
    u32           fg, bg;
} Cell;

typedef struct {
    int     cols, rows;      /* terminal dimensions in character cells      */
    int     pw, ph;          /* pixel dimensions (ph == rows * 2)           */
    u32    *pix;             /* pw * ph RGB pixels                          */
    double *zbuf;            /* pw wall distances (per-column depth buffer) */
    Cell   *cells;           /* cols * rows character cells                 */
    Buf     out;             /* ANSI byte stream for one frame              */
} Screen;

typedef enum { CM_ANSI256 = 0, CM_TRUECOLOR = 1 } ColorMode;

typedef struct {
    int    mapw, maph;       /* maze size                                   */
    double fov;              /* horizontal field of view in degrees         */
    u32    seed;
    int    target_fps;
    long   frames;           /* stop after N frames (0 = run forever)       */
    int    headless;         /* render without a terminal, then report      */
    int    head_cols, head_rows;  /* off-screen canvas for --headless       */
    int    selftest;         /* run the built-in verification, then exit    */
    int    new_maze;         /* set by [R] to rebuild the world             */
    int    demo;             /* autopilot                                   */
    int    floor_cast;       /* perspective floor/ceiling                   */
    int    minimap;
    int    crosshair;
    int    help;
    int    paused;
    int    quit;
    int    sprint;
    double sprint_t;
    double bright;
    int    pitch;            /* vertical look offset, in pixels             */
    int    won;              /* reached the exit gate                       */
    double elapsed;         /* seconds since maze start (excludes pause)   */
    long   steps;           /* walk distance accumulator (milli-cells)     */
    ColorMode mode;
} Options;

static void screen_resize(Screen *sc, int cols, int rows)
{
    if (cols < 20) cols = 20;
    if (rows < 8)  rows = 8;
    if (cols == sc->cols && rows == sc->rows) return;

    sc->cols = cols;
    sc->rows = rows;
    sc->pw   = cols;
    sc->ph   = rows * 2;                     /* two pixels per character row */

    free(sc->pix);  free(sc->zbuf); free(sc->cells);
    sc->pix   = (u32 *)malloc(sizeof(u32) * (size_t)sc->pw * (size_t)sc->ph);
    sc->zbuf  = (double *)malloc(sizeof(double) * (size_t)sc->pw);
    sc->cells = (Cell *)malloc(sizeof(Cell) * (size_t)sc->cols * (size_t)sc->rows);
    if (!sc->pix || !sc->zbuf || !sc->cells) {
        fprintf(stderr, "raycaster: out of memory\n");
        exit(1);
    }
    buf_free(&sc->out);
    buf_init(&sc->out, (size_t)sc->cols * (size_t)sc->rows * 8 + 4096);
}

static void screen_init(Screen *sc)
{
    memset(sc, 0, sizeof *sc);
    buf_init(&sc->out, 65536);
    screen_resize(sc, 80, 24);
}

static void screen_free(Screen *sc)
{
    free(sc->pix); free(sc->zbuf); free(sc->cells);
    buf_free(&sc->out);
    memset(sc, 0, sizeof *sc);
}

/* ------------------------------------------------------------ terminal --- */
#ifdef __EMSCRIPTEN__
/* Browser build: no tty, no signals, no alternate screen.  The JS host owns
 * the canvas, pushes key bytes through web_key() and resizes via web_resize().*/
static volatile sig_atomic_t g_quit = 0;
static int g_web_want_cols = 0, g_web_want_rows = 0;   /* pending JS resize   */

static void term_setup(void)   { }
static void term_restore(void) { }
static void term_size(int *cols, int *rows) { *cols = 120; *rows = 45; }

/* The JS resize handler calls this; the new grid is applied at the top of the
 * next frame (the same place SIGWINCH is handled natively).                  */
EMSCRIPTEN_KEEPALIVE void web_resize(int cols, int rows)
{
    g_web_want_cols = cols < 20 ? 20 : (cols > 400 ? 400 : cols);
    g_web_want_rows = rows < 8  ? 8  : (rows > 200 ? 200 : rows);
}
#else
static struct termios g_saved_termios;
static int            g_have_termios = 0;
static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_winch = 0;

static void on_signal(int sig)
{
    if (sig == SIGWINCH) g_winch = 1;
    else                 g_quit  = 1;
}

static void term_write(const char *s)
{
    size_t n = strlen(s), off = 0;
    while (off < n) {
        ssize_t w = write(STDOUT_FILENO, s + off, n - off);
        if (w <= 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
}

static void term_restore(void)
{
    if (g_have_termios) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
        g_have_termios = 0;
    }
    term_write("\x1b[0m\x1b[?25h\x1b[?1049l");   /* colours off, cursor on, main screen */
}

static void term_setup(void)
{
    struct termios t;
    struct sigaction sa;

    if (tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
        t = g_saved_termios;
        t.c_lflag &= (tcflag_t)~(ICANON | ECHO | IEXTEN);   /* keep ISIG: ^C works */
        t.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        t.c_cc[VMIN]  = 0;                                  /* non-blocking reads  */
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
        g_have_termios = 1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);

    /* enter the alternate screen and hide the cursor */
    term_write("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H");
}

static void term_size(int *cols, int *rows)
{
    struct winsize ws;
    *cols = 80; *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = (int)ws.ws_col;
        *rows = (int)ws.ws_row;
    }
    if (*cols < 20) *cols = 20;
    if (*rows < 8)  *rows = 8;
}
#endif                                   /* !__EMSCRIPTEN__                  */

/* ==========================================================================
 *  PLAYER + INPUT
 * ========================================================================== */
static void player_rotate(Player *p, double a)
{
    double cs = cos(a), sn = sin(a);
    double dx = p->dirx, px = p->planex;
    p->dirx   = dx * cs - p->diry * sn;
    p->diry   = dx * sn + p->diry * cs;
    p->planex = px * cs - p->planey * sn;
    p->planey = px * sn + p->planey * cs;
}

static void player_set_fov(Player *p, double fov_deg)
{
    double l = tan(dclamp(fov_deg, 20.0, 120.0) * PI / 360.0);
    /* plane is the RIGHT-hand vector (-diry, dirx): cam=-1 is the left
     * edge, cam=+1 the right edge.  (The old (diry, -dirx) pointed left
     * and mirrored the world, so a right turn looked like a left turn.) */
    p->planex = -p->diry * l;
    p->planey =  p->dirx * l;
}

static void player_spawn(Player *p, const World *wo, double fov_deg)
{
    p->x = wo->spawn_x;
    p->y = wo->spawn_y;
    p->dirx = cos(wo->spawn_dir);
    p->diry = sin(wo->spawn_dir);
    p->planex = -p->diry;                /* placeholder, fov fixes it up */
    p->planey =  p->dirx;
    player_set_fov(p, fov_deg);
}

/* Terminals report key *presses* (and auto-repeat), never releases, so each
 * key event refreshes a short hold timer.  The axis then decays smoothly to
 * zero once the events stop, which feels like momentum instead of stutter. */
typedef struct {
    double fwd, strafe, turn, pitch;
    double fwd_t, strafe_t, turn_t, pitch_t;
} Axes;

static void axis_press(double *v, double *t, double amount)
{
    *v = dclamp(*v + amount, -1.0, 1.0);
    *t = KEYHOLD;
}

static void axes_decay(Axes *a, double dt)
{
    double k = dclamp(dt * 9.0, 0.0, 1.0);
    if (a->fwd_t    > 0.0) { a->fwd_t    -= dt; } else { a->fwd    += (0.0 - a->fwd)    * k; }
    if (a->strafe_t > 0.0) { a->strafe_t -= dt; } else { a->strafe += (0.0 - a->strafe) * k; }
    if (a->turn_t   > 0.0) { a->turn_t   -= dt; } else { a->turn   += (0.0 - a->turn)   * k; }
    if (a->pitch_t  > 0.0) { a->pitch_t  -= dt; } else { a->pitch  += (0.0 - a->pitch)  * k; }
}

static void update_player(Options *o, Axes *a, Player *p, const World *wo, double dt)
{
    double speed = (o->sprint ? 4.4 : 2.5) * dt;
    double turn  = 2.3 * dt;
    double radius = 0.24;
    double dd, ss, nx, ny;

    player_rotate(p, a->turn * turn);
    player_set_fov(p, o->fov);                       /* FOV can change live */
    o->pitch = iclamp(o->pitch + (int)(a->pitch * dt * 260.0), -500, 500);

    dd = a->fwd * speed;
    ss = a->strafe * speed;
    if (dd != 0.0 || ss != 0.0) {
        nx = p->x + p->dirx * dd + p->planex * ss;
        ny = p->y + p->diry * dd + p->planey * ss;
        if (can_move_exit(wo, nx, p->y, radius)) p->x = nx;  /* slide on walls */
        if (can_move_exit(wo, p->x, ny, radius)) p->y = ny;
        o->steps += (long)(fabs(dd + ss) * 250.0);
    }

    if (!o->won) {   /* reached the exit gate? */
        double dx = p->x - wo->exit_wx, dy = p->y - wo->exit_wy;
        if (dx * dx + dy * dy < 0.60 * 0.60) o->won = 1;
    }
}

#ifdef __EMSCRIPTEN__
/* The browser has no tty: the JS host queues key bytes here.  A queued byte
 * stream looks exactly like a terminal read (including CSI arrow sequences),
 * so the parser below is shared with the native build.  Holding a key is
 * emulated by re-sending from JS every ~75 ms, mirroring terminal auto-repeat. */
static unsigned char g_inq[1024];
static int           g_inq_n = 0;

EMSCRIPTEN_KEEPALIVE void web_key(int byte)
{
    if (g_inq_n < (int)sizeof g_inq)
        g_inq[g_inq_n++] = (unsigned char)(byte & 0xff);
}

static int input_read(unsigned char *dst, int cap)
{
    int n = g_inq_n < cap ? g_inq_n : cap;
    if (n <= 0) return 0;
    memcpy(dst, g_inq, (size_t)n);
    if (n < g_inq_n) memmove(g_inq, g_inq + n, (size_t)(g_inq_n - n));
    g_inq_n -= n;
    return n;
}
#else
static int input_read(unsigned char *dst, int cap)
{
    return (int)read(STDIN_FILENO, dst, (size_t)cap);
}
#endif

/* Read everything the terminal has buffered and turn it into axis impulses. */
static void handle_input(Options *o, Axes *a)
{
    unsigned char buf[128];
    int n, i;
    n = input_read(buf, (int)sizeof buf);
    if (n <= 0) return;

    for (i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == 0x1b) {                       /* ESC or a CSI arrow key */
            if (i + 1 < n && (buf[i + 1] == '[' || buf[i + 1] == 'O')) {
                if (i + 2 < n) {
                    switch (buf[i + 2]) {
                    case 'A': axis_press(&a->pitch,  &a->pitch_t,   1.0); break;
                    case 'B': axis_press(&a->pitch,  &a->pitch_t,  -1.0); break;
                    case 'C': axis_press(&a->turn,   &a->turn_t,    1.0); break;
                    case 'D': axis_press(&a->turn,   &a->turn_t,   -1.0); break;
                    default: break;
                    }
                    i += 2;
                } else {
                    i = n;                      /* sequence split across reads */
                }
            } else {
                o->quit = 1;
            }
            continue;
        }
        switch (c) {
        case 'w': axis_press(&a->fwd,    &a->fwd_t,     1.0); break;
        case 's': axis_press(&a->fwd,    &a->fwd_t,    -1.0); break;
        case 'a': axis_press(&a->strafe, &a->strafe_t, -1.0); break;
        case 'd': axis_press(&a->strafe, &a->strafe_t,  1.0); break;
        case 'W': case 'S': case 'A': case 'D':
            o->sprint = 1; o->sprint_t = 0.6;
            if (c == 'W') axis_press(&a->fwd,    &a->fwd_t,     1.0);
            if (c == 'S') axis_press(&a->fwd,    &a->fwd_t,    -1.0);
            if (c == 'A') axis_press(&a->strafe, &a->strafe_t, -1.0);
            if (c == 'D') axis_press(&a->strafe, &a->strafe_t,  1.0);
            break;
        case 'j': case ',': axis_press(&a->turn, &a->turn_t, -1.0); break;
        case 'l': case '.': axis_press(&a->turn, &a->turn_t,  1.0); break;
        case 'i': case 'k': axis_press(&a->pitch, &a->pitch_t, c == 'i' ? 1.0 : -1.0); break;
        case 'm': o->minimap   = !o->minimap;   break;
        case 'f': o->floor_cast = !o->floor_cast; break;
        case 'c': o->crosshair = !o->crosshair; break;
        case 'g': o->demo      = !o->demo;      break;
        case 'p': o->paused    = !o->paused;    break;
        case 'h': o->help      = !o->help;      break;
        case 't': o->mode      = (o->mode == CM_TRUECOLOR) ? CM_ANSI256 : CM_TRUECOLOR; break;
        case 'r': case 'R': o->new_maze = 1; break;
        case 'q': case 3: o->quit = 1; break;
        case '-': o->bright = dclamp(o->bright - 0.08, 0.3, 2.0); break;
        case '=': o->bright = dclamp(o->bright + 0.08, 0.3, 2.0); break;
        case '[': o->fov = dclamp(o->fov - 4.0, 30.0, 110.0); break;
        case ']': o->fov = dclamp(o->fov + 4.0, 30.0, 110.0); break;
        default: break;
        }
    }
}

/* ==========================================================================
 *  RENDERER
 * ========================================================================== */
/* Fallback background: a flat vertical gradient (used with --no-floor). */
static void fill_backdrop(Screen *sc, const Options *o, int horizon)
{
    int x, y;
    for (y = 0; y < sc->ph; y++) {
        u32    *row = sc->pix + (size_t)y * sc->pw;
        u32     c;
        if (y < horizon) {
            double f = (double)(horizon - y) / (double)(horizon + 1);
            c = apply_light(pack_rgb(22, 24, 33), o->bright * (0.75 + 0.45 * (1.0 - f)), f * 0.40);
        } else {
            double f = (double)(y - horizon) / (double)(sc->ph - horizon + 1);
            c = apply_light(pack_rgb(60, 60, 72), o->bright * (1.10 - 0.55 * f), f * 0.45);
        }
        for (x = 0; x < sc->pw; x++) row[x] = c;
    }
}

/* Per-pixel floor and ceiling casting.  For every screen row below the
 * horizon the world distance is constant, so a single division per row gives
 * the world-space step between neighbouring pixels: proper perspective.    */
static void cast_floor_ceiling(Screen *sc, const Player *p, const Options *o, int horizon)
{
    double rd0x = p->dirx - p->planex, rd0y = p->diry - p->planey;
    double rd1x = p->dirx + p->planex, rd1y = p->diry + p->planey;
    double pos_z = 0.5 * (double)sc->ph;         /* eye height = 0.5 units  */
    const double inv_pw = 1.0 / (double)sc->pw;
    int    y, x;

    for (y = horizon + 1; y < sc->ph; y++) {     /* ---------- floor ---- */
        double rowd  = pos_z / (double)(y - horizon);
        double stepx = rowd * (rd1x - rd0x) * inv_pw;
        double stepy = rowd * (rd1y - rd0y) * inv_pw;
        double fx = p->x + rowd * rd0x;
        double fy = p->y + rowd * rd0y;
        double lum = o->bright / (1.0 + 0.10 * rowd + 0.010 * rowd * rowd);
        double fog = dclamp((rowd - 3.0) * 0.055, 0.0, 0.62);
        u32   *row = sc->pix + (size_t)y * sc->pw;
        for (x = 0; x < sc->pw; x++) {
            /* two's-complement wrap keeps this cheap and still correct for
             * every map coordinate the player can actually reach        */
            int tx = ((int)(fx * (double)TEXW)) & TEXMASK;
            int ty = ((int)(fy * (double)TEXH)) & TEXMASK;
            row[x] = apply_light(g_floor[ty * TEXW + tx], lum, fog);
            fx += stepx;
            fy += stepy;
        }
    }
    for (y = horizon - 1; y >= 0; y--) {         /* --------- ceiling --- */
        double rowd  = pos_z / (double)(horizon - y);
        double stepx = rowd * (rd1x - rd0x) * inv_pw;
        double stepy = rowd * (rd1y - rd0y) * inv_pw;
        double fx = p->x + rowd * rd0x;
        double fy = p->y + rowd * rd0y;
        double lum = o->bright * 0.82 / (1.0 + 0.12 * rowd + 0.012 * rowd * rowd);
        double fog = dclamp((rowd - 2.0) * 0.070, 0.0, 0.70);
        u32   *row = sc->pix + (size_t)y * sc->pw;
        for (x = 0; x < sc->pw; x++) {
            int tx = ((int)(fx * (double)TEXW)) & TEXMASK;
            int ty = ((int)(fy * (double)TEXH)) & TEXMASK;
            row[x] = apply_light(g_ceil[ty * TEXW + tx], lum, fog);
            fx += stepx;
            fy += stepy;
        }
    }
}

/* ---------------------------------------------------------------- rays --- */
typedef struct {
    int    mx, my;    /* the map cell that was hit                       */
    int    side;      /* 0 = hit an X-facing (vertical) wall, 1 = Y-side  */
    double perp;      /* perpendicular distance to that wall              */
} RayHit;

/* One DDA ray march over the grid, in the style of the classic
 * Wolfenstein-3D code: step to the next cell boundary, whichever comes
 * first, until a solid cell is entered.                                      */
static RayHit trace_ray(const World *wo, double px, double py, double rx, double ry)
{
    RayHit hit;
    int    mx = (int)floor(px), my = (int)floor(py);
    double ddx = (rx == 0.0) ? 1e30 : fabs(1.0 / rx);   /* x step per unit  */
    double ddy = (ry == 0.0) ? 1e30 : fabs(1.0 / ry);   /* y step per unit  */
    int    stepx, stepy, guard;
    double sdx, sdy;

    hit.side = 0;
    if (rx < 0.0) { stepx = -1; sdx = (px - (double)mx) * ddx; }
    else          { stepx =  1; sdx = ((double)mx + 1.0 - px) * ddx; }
    if (ry < 0.0) { stepy = -1; sdy = (py - (double)my) * ddy; }
    else          { stepy =  1; sdy = ((double)my + 1.0 - py) * ddy; }

    for (guard = 0; guard < 4 * MAPMAX; guard++) {
        if (sdx < sdy) { sdx += ddx; mx += stepx; hit.side = 0; }
        else           { sdy += ddy; my += stepy; hit.side = 1; }
        if (mx < 0 || my < 0 || mx >= wo->w || my >= wo->h) break;
        if (wo->grid[my][mx] != 0) break;
    }
    hit.mx = mx;
    hit.my = my;
    hit.perp = (hit.side == 0) ? (sdx - ddx) : (sdy - ddy);
    if (hit.perp < 1e-4) hit.perp = 1e-4;
    return hit;
}

/* Classic DDA ray marching over the grid: one ray per screen column. */
static void cast_walls(Screen *sc, const World *wo, const Player *p, const Options *o,
                       double t, int horizon)
{
    int x;
    for (x = 0; x < sc->pw; x++) {
        double cam = 2.0 * (double)x / (double)sc->pw - 1.0;
        double rx  = p->dirx + p->planex * cam;
        double ry  = p->diry + p->planey * cam;
        RayHit hit = trace_ray(wo, p->x, p->y, rx, ry);
        double perp = hit.perp, lineh, wallx, step, texpos, lum, fog;
        int    mx = hit.mx, my = hit.my, side = hit.side;
        int    h, ds, de, texx, y, wtype;
        const u32 *tex;

        sc->zbuf[x] = perp;

        wtype = T_STONE;
        if (mx >= 0 && my >= 0 && mx < wo->w && my < wo->h) {
            wtype = (int)wo->grid[my][mx];
            if (wtype <= 0 || wtype >= T_TYPES) wtype = T_STONE;
        }

        lineh = (double)sc->ph / perp;
        h     = (int)lineh;
        ds    = horizon - h / 2;
        de    = horizon + h / 2;

        /* where along the wall did we hit it?  -> texture column */
        wallx = (side == 0) ? (p->y + perp * ry) : (p->x + perp * rx);
        wallx -= floor(wallx);
        texx = (int)(wallx * (double)TEXW);
        if ((side == 0 && rx > 0.0) || (side == 1 && ry < 0.0)) texx = TEXW - texx - 1;
        texx = iclamp(texx, 0, TEXW - 1);

        lum = o->bright / (1.0 + 0.09 * perp + 0.008 * perp * perp);
        if (side == 1) lum *= 0.72;                   /* fake directional light */
        fog = dclamp((perp - 2.5) * 0.060, 0.0, 0.60);
        if (wtype == T_GLOW) {                        /* animated emissive wall */
            lum = dclamp(lum + 0.55 + 0.30 * sin(t * 2.6 + wallx * 6.0), 0.0, 1.8);
            fog *= 0.35;
        }

        tex    = g_wall[wtype];
        step   = (double)TEXH / lineh;
        texpos = 0.0;                                 /* top of the wall quad   */
        if (ds < 0) { texpos += (double)(-ds) * step; ds = 0; }
        if (de > sc->ph) de = sc->ph;
        for (y = ds; y < de; y++) {
            int ty = ((int)texpos) & TEXMASK;
            texpos += step;
            sc->pix[(size_t)y * sc->pw + x] = apply_light(tex[ty * TEXW + texx], lum, fog);
        }
    }
}

/* Billboard sprites: transformed into camera space, sorted back to front and
 * drawn through the wall z-buffer so they can be occluded column by column.  */
static void cast_sprites(Screen *sc, const World *wo, const Player *p, const Options *o,
                         double t, int horizon)
{
    static int    order[MAXSPR];
    static double dist[MAXSPR];
    double invdet;
    int    i, j, n = wo->nspr;

    if (n <= 0) return;
    for (i = 0; i < n; i++) {
        double dx = wo->spr[i].x - p->x, dy = wo->spr[i].y - p->y;
        dist[i]  = dx * dx + dy * dy;
        order[i] = i;
    }
    for (i = 1; i < n; i++) {                    /* insertion sort, far -> near */
        int    id = order[i];
        double d  = dist[id];
        j = i - 1;
        while (j >= 0 && dist[order[j]] < d) { order[j + 1] = order[j]; j--; }
        order[j + 1] = id;
    }

    invdet = 1.0 / (p->planex * p->diry - p->dirx * p->planey);
    for (i = 0; i < n; i++) {
        const Sprite *s = &wo->spr[order[i]];
        double sx = s->x - p->x, sy = s->y - p->y;
        double tx = invdet * ( p->diry * sx - p->dirx * sy);
        double ty = invdet * (-p->planey * sx + p->planex * sy);
        double lum, fog;
        const u32 *tex;
        int    scrx, hgt, wid, x0, x1, y0, y1, vmove, x, y;

        if (ty <= 0.20) continue;                        /* behind the camera */
        scrx  = (int)((double)sc->pw * 0.5 * (1.0 + tx / ty));
        hgt   = (int)(((double)sc->ph / ty) * s->scale);
        wid   = hgt;
        if (hgt < 2 || wid < 2) continue;
        vmove = (int)(s->vmove / ty);
        y0    = horizon - hgt / 2 + vmove;
        y1    = horizon + hgt / 2 + vmove;
        x0    = scrx - wid / 2;
        x1    = scrx + wid / 2;
        if (x1 <= 0 || x0 >= sc->pw || y1 <= 0 || y0 >= sc->ph) continue;

        lum = o->bright / (1.0 + 0.09 * ty + 0.008 * ty * ty);
        if (s->glow > 0.0)                               /* torches flicker    */
            lum = dclamp(lum + s->glow * (0.45 + 0.18 * sin(t * 5.0 + (double)order[i])),
                         0.0, 1.8);
        fog = dclamp((ty - 3.0) * 0.055, 0.0, 0.55);
        tex = g_sprite[s->type];

        for (x = x0; x < x1; x++) {
            int texx, ys, ye;
            if (x < 0 || x >= sc->pw) continue;
            if (ty >= sc->zbuf[x]) continue;             /* hidden by a wall   */
            texx = (int)(((double)(x - x0) * (double)TEXW) / (double)wid);
            if (texx < 0 || texx >= TEXW) continue;
            ys = y0 < 0 ? 0 : y0;
            ye = y1 > sc->ph ? sc->ph : y1;
            for (y = ys; y < ye; y++) {
                int texy = (int)(((double)(y - y0) * (double)TEXH) / (double)hgt);
                u32 c;
                if (texy < 0 || texy >= TEXH) continue;
                c = tex[texy * TEXW + texx];
                if (c == 0) continue;                    /* alpha test         */
                sc->pix[(size_t)y * sc->pw + x] = apply_light(c, lum, fog);
            }
        }
    }
}

/* Render one complete frame into the pixel buffer. */
static void render_view(Screen *sc, const World *wo, const Player *p, const Options *o, double t)
{
    int lim = (int)((double)sc->ph * 0.45);
    int horizon = sc->ph / 2 + iclamp(o->pitch, -lim, lim);

    if (o->floor_cast) cast_floor_ceiling(sc, p, o, horizon);
    else               fill_backdrop(sc, o, horizon);
    cast_walls(sc, wo, p, o, t, horizon);
    cast_sprites(sc, wo, p, o, t, horizon);
}

/* ==========================================================================
 *  TEXT: pixels -> half-block cells, HUD, minimap, help
 * ========================================================================== */
static void pix_to_cells(Screen *sc)
{
    int cy, x;
    for (cy = 0; cy < sc->rows; cy++) {
        const u32 *top = sc->pix + (size_t)(cy * 2) * sc->pw;
        const u32 *bot = sc->pix + (size_t)(cy * 2 + 1) * sc->pw;
        Cell      *dst = sc->cells + (size_t)cy * sc->cols;
        for (x = 0; x < sc->cols; x++) {
            dst[x].ch = CH_HALF;      /* upper half block: fg paints the top    */
            dst[x].fg = top[x];       /* pixel, bg paints the bottom pixel      */
            dst[x].bg = bot[x];
        }
    }
}

static void emit_fg(Buf *b, u32 c, ColorMode m)
{
    if (m == CM_TRUECOLOR)
        buf_printf(b, "\x1b[38;2;%d;%d;%dm",
                   (int)((c >> 16) & 255u), (int)((c >> 8) & 255u), (int)(c & 255u));
    else
        buf_printf(b, "\x1b[38;5;%dm", to_ansi256(c));
}

static void emit_bg(Buf *b, u32 c, ColorMode m)
{
    if (m == CM_TRUECOLOR)
        buf_printf(b, "\x1b[48;2;%d;%d;%dm",
                   (int)((c >> 16) & 255u), (int)((c >> 8) & 255u), (int)(c & 255u));
    else
        buf_printf(b, "\x1b[48;5;%dm", to_ansi256(c));
}

/* Serialise the cell grid into ANSI escapes.  Colour escapes are only emitted
 * when they change, which cuts the byte count of a frame dramatically.      */
static void present_build(Screen *sc, const Options *o)
{
    static const char HALF[4] = { (char)0xe2, (char)0x96, (char)0x80, 0 };  /* U+2580 */
    Buf *b = &sc->out;
    u32  last_fg = 0xffffffffu, last_bg = 0xffffffffu;
    int  idx_fg = -1, idx_bg = -1;
    int  y, x;

    b->len = 0;
    buf_puts(b, "\x1b[H");
    for (y = 0; y < sc->rows; y++) {
        const Cell *row = sc->cells + (size_t)y * sc->cols;
        for (x = 0; x < sc->cols; x++) {
            const Cell *c = &row[x];
            int  need_fg = (c->fg != c->bg);   /* a blank never needs a fg  */

            if (need_fg && c->fg != last_fg) {
                last_fg = c->fg;
                if (o->mode == CM_TRUECOLOR) {
                    emit_fg(b, c->fg, o->mode);
                } else {
                    int idx = to_ansi256(c->fg);     /* many shades collapse */
                    if (idx != idx_fg) { idx_fg = idx; buf_printf(b, "\x1b[38;5;%dm", idx); }
                }
            }
            if (c->bg != last_bg) {
                last_bg = c->bg;
                if (o->mode == CM_TRUECOLOR) {
                    emit_bg(b, c->bg, o->mode);
                } else {
                    int idx = to_ansi256(c->bg);
                    if (idx != idx_bg) { idx_bg = idx; buf_printf(b, "\x1b[48;5;%dm", idx); }
                }
            }
            if (c->ch == CH_HALF)      buf_put(b, HALF, 3);
            else if (c->fg == c->bg)   buf_putc(b, ' ');
            else                       buf_putc(b, (char)c->ch);
        }
    }
}

#ifdef __EMSCRIPTEN__
/* ---- web present: RGBA scene + text-cell list exported to the JS canvas --- */
static unsigned char *g_web_rgba     = NULL;
static size_t         g_web_rgba_cap = 0;   /* allocated bytes               */
static int           *g_web_txt      = NULL; /* 5 ints per cell: x,y,ch,fg,bg */
static int            g_web_txt_cap  = 0;   /* capacity in cells             */
static int            g_web_ntxt     = 0;
static int            g_web_cols = 0, g_web_rows = 0;

EMSCRIPTEN_KEEPALIVE unsigned char *web_rgba(void)   { return g_web_rgba; }
EMSCRIPTEN_KEEPALIVE int *web_text_ptr(void)         { return g_web_txt; }
EMSCRIPTEN_KEEPALIVE int  web_text_count(void)       { return g_web_ntxt; }
EMSCRIPTEN_KEEPALIVE int  web_cols(void)             { return g_web_cols; }
EMSCRIPTEN_KEEPALIVE int  web_rows(void)             { return g_web_rows; }

static void present_flush(Screen *sc)
{
    size_t npix = (size_t)sc->pw * (size_t)sc->ph, i;
    int y, x;

    if (npix * 4 > g_web_rgba_cap) {            /* scene -> RGBA bytes        */
        unsigned char *p = (unsigned char *)realloc(g_web_rgba, npix * 4);
        if (p) { g_web_rgba = p; g_web_rgba_cap = npix * 4; }
    }
    if (g_web_rgba) {
        unsigned char *d = g_web_rgba;
        for (i = 0; i < npix; i++) {
            u32 c = sc->pix[i];                 /* 0xRRGGBB -> R,G,B,A        */
            d[0] = (unsigned char)(c >> 16);
            d[1] = (unsigned char)(c >> 8);
            d[2] = (unsigned char)c;
            d[3] = 255u;
            d += 4;
        }
    }
    if (sc->cols * sc->rows > g_web_txt_cap) {  /* HUD/minimap glyphs -> list */
        int *p = (int *)realloc(g_web_txt,
                                sizeof(int) * 5 * (size_t)(sc->cols * sc->rows));
        if (p) { g_web_txt = p; g_web_txt_cap = sc->cols * sc->rows; }
    }
    g_web_ntxt = 0;
    if (g_web_txt) {
        for (y = 0; y < sc->rows; y++) {
            const Cell *row = sc->cells + (size_t)y * sc->cols;
            for (x = 0; x < sc->cols; x++) {
                int *e;
                if (row[x].ch == CH_HALF) continue;   /* scene pixel, not text */
                e = g_web_txt + 5 * (size_t)g_web_ntxt;
                e[0] = x; e[1] = y; e[2] = (int)row[x].ch;
                e[3] = (int)row[x].fg; e[4] = (int)row[x].bg;
                g_web_ntxt++;
            }
        }
    }
    g_web_cols = sc->cols;
    g_web_rows = sc->rows;
}
#else
static void present_flush(Screen *sc)
{
    size_t off = 0;
    while (off < sc->out.len) {
        ssize_t w = write(STDOUT_FILENO, sc->out.data + off, sc->out.len - off);
        if (w <= 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
}
#endif

/* Perceptual luminance of a packed colour (used by reports and self tests). */
static double lum_of(u32 c)
{
    return 0.299 * (double)((c >> 16) & 255u)
         + 0.587 * (double)((c >>  8) & 255u)
         + 0.114 * (double)( c        & 255u);
}

static void draw_text(Screen *sc, int x, int y, const char *s, u32 fg, u32 bg)
{
    for (; *s; s++, x++) {
        Cell *c;
        if (x < 0 || y < 0 || x >= sc->cols || y >= sc->rows) continue;
        c = sc->cells + (size_t)y * sc->cols + x;
        c->ch = (unsigned char)*s;
        c->fg = fg;
        c->bg = bg;
    }
}

static void fill_cells(Screen *sc, int x, int y, int w, int h, u32 bg)
{
    int i, j;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            Cell *c;
            if (x + i < 0 || y + j < 0 || x + i >= sc->cols || y + j >= sc->rows) continue;
            c = sc->cells + (size_t)(y + j) * sc->cols + (x + i);
            c->ch = ' '; c->fg = bg; c->bg = bg;
        }
    }
}

static u32 minimap_wall_color(int t)
{
    switch (t) {
    case T_BRICK: return 0xa04d38;
    case T_STONE: return 0x66727f;
    case T_WOOD:  return 0x8d6435;
    case T_TECH:  return 0x3d6d92;
    case T_GLOW:  return 0x35e0a4;
    default:      return 0x505860;
    }
}

/* Top-right floor plan.  Shrinks (sampling 2x2 / 3x3 tiles per cell) until it
 * fits, and disappears entirely on very small terminals.                    */
static void draw_minimap(Screen *sc, const World *wo, const Player *p)
{
    int scale = 1, cw, ch, ox, oy, tx, ty, sx, sy;

    while (scale < 4 && (wo->h + scale - 1) / scale > sc->rows - 10) scale++;
    while (scale < 4 && (wo->w + scale - 1) / scale > sc->cols / 3) scale++;
    cw = (wo->w + scale - 1) / scale;
    ch = (wo->h + scale - 1) / scale;
    ox = sc->cols - cw - 2;
    oy = 2;
    if (ox < 2 || oy + ch + 1 > sc->rows - 1) return;

    draw_text(sc, ox + 1, oy - 1, "[ map ]", 0xa8bcdd, 0x0b0e13);

    for (ty = 0; ty < ch; ty++) {
        for (tx = 0; tx < cw; tx++) {
            int wall = 0, open = 0, wtype = T_STONE;
            Cell *c;
            for (sy = 0; sy < scale; sy++) {
                for (sx = 0; sx < scale; sx++) {
                    int mx = tx * scale + sx, my = ty * scale + sy;
                    if (mx >= wo->w || my >= wo->h) continue;
                    if (wo->grid[my][mx]) { wall = 1; wtype = (int)wo->grid[my][mx]; }
                    else open = 1;
                }
            }
            c = sc->cells + (size_t)(oy + ty) * sc->cols + (ox + tx);
            c->fg = c->bg = wall ? minimap_wall_color(wtype) : (open ? 0x141a24 : 0x04060a);
            c->ch = ' ';
        }
    }

    {   /* player position, plus a stub showing where we are looking */
        int mx = (int)(p->x / scale), my = (int)(p->y / scale);
        int wx = (int)(p->x + p->dirx * 1.6), wy = (int)(p->y + p->diry * 1.6);
        int fx = wx / scale, fy = wy / scale;
        int ex = (int)wo->exit_wx / scale, ey = (int)wo->exit_wy / scale;
        Cell *c;
        if (wx >= 0 && wy >= 0 && wx < wo->w && wy < wo->h && wo->grid[wy][wx] == 0 &&
            fx >= 0 && fx < cw && fy >= 0 && fy < ch) {
            c = sc->cells + (size_t)(oy + fy) * sc->cols + (ox + fx);
            c->ch = '+'; c->fg = 0xffcf5e; c->bg = 0x2a1d08;
        }
        if (ex >= 0 && ex < cw && ey >= 0 && ey < ch) {
            c = sc->cells + (size_t)(oy + ey) * sc->cols + (ox + ex);
            c->ch = 'E'; c->fg = 0x0b1410; c->bg = 0x35e0a4;
        }
        if (mx >= 0 && mx < cw && my >= 0 && my < ch) {
            c = sc->cells + (size_t)(oy + my) * sc->cols + (ox + mx);
            c->ch = '@'; c->fg = 0xfff2c0; c->bg = 0x7a4a12;
        }
    }
}

static void draw_hud(Screen *sc, const World *wo, const Player *p, const Options *o,
                     double fps, double ms)
{
    char line[160];
    int  y = sc->rows;

    snprintf(line, sizeof line, " RAYCASTER %5.1f fps %5.2f ms ", fps, ms);
    draw_text(sc, 0, 0, line, 0x0b0e14, 0xbcd0f2);
    {
        /* compass arrow toward the exit: angle between view dir and exit dir */
        double dx = wo->exit_wx - p->x, dy = wo->exit_wy - p->y;
        double dist = sqrt(dx * dx + dy * dy);
        double ang = atan2(p->dirx * dy - p->diry * dx,
                           p->dirx * dx + p->diry * dy);
        const char *arrow;
        if (ang > -0.4 && ang <= 0.4) arrow = "^";
        else if (ang > 0.4 && ang <= 1.2) arrow = "^>";
        else if (ang > 1.2 && ang <= 2.0) arrow = ">";
        else if (ang > 2.0 || ang <= -2.7) arrow = "v";
        else if (ang > -2.7 && ang <= -2.0) arrow = "<";
        else if (ang > -2.0 && ang <= -1.2) arrow = "<";
        else arrow = "^<";
        if (o->won)
            snprintf(line, sizeof line, " ESCAPED!  time %02d:%05.2f  steps ~%ld ",
                     (int)(o->elapsed / 60.0), fmod(o->elapsed, 60.0), o->steps / 250);
        else
            snprintf(line, sizeof line, " exit %s %.1fm  cell %d,%d  fov %d  sprites %d ",
                     arrow, dist, wo->exit_x, wo->exit_y,
                     (int)(o->fov + 0.5), wo->nspr);
        draw_text(sc, 0, 1, line, o->won ? 0x0b1410 : 0x93a6c6,
                  o->won ? 0x35e0a4 : 0x0a0d12);
    }

    if (y >= 3) {
        snprintf(line, sizeof line,
                 " W/S walk  A/D strafe  </> or J/L turn  ^/v look  SHIFT run "
                 " Q quit ");
        draw_text(sc, 0, y - 2, line, 0x8698b8, 0x0a0d12);
        snprintf(line, sizeof line,
                 " [M]ap:%s  [F]loor:%s  [C]rosshair:%s  [G]autopilot:%s  [T]colour:%s "
                 " [P]ause:%s  [H]elp  [-] [+] bright  '[' ']' fov  [R] new maze ",
                 o->minimap ? "on" : "off", o->floor_cast ? "on" : "off",
                 o->crosshair ? "on" : "off", o->demo ? "on" : "off",
                 o->mode == CM_TRUECOLOR ? "true" : "256",
                 o->paused ? "on" : "off");
        draw_text(sc, 0, y - 1, line, 0x6f7f9c, 0x0a0d12);
    }
    if (o->crosshair) {
        Cell *c = sc->cells + (size_t)(sc->rows / 2) * sc->cols + sc->cols / 2;
        c->ch = '+';
        c->fg = 0xe8f0ff;
        c->bg = 0x202634;
    }
    if (o->paused) draw_text(sc, sc->cols / 2 - 4, sc->rows / 2 - 4, " PAUSED ", 0x11151c, 0xffd76a);
    if (o->won && sc->cols >= 46 && sc->rows >= 9) {
        char w1[64], w2[64];
        int  ww = 42, wx = (sc->cols - ww) / 2, wy = sc->rows / 2 - 3;
        snprintf(w1, sizeof w1, "  YOU ESCAPED in %02d:%05.2f!  ",
                 (int)(o->elapsed / 60.0), fmod(o->elapsed, 60.0));
        snprintf(w2, sizeof w2, "  R = new maze    Q = quit  ");
        fill_cells(sc, wx, wy, ww, 5, 0x0b1a12);
        draw_text(sc, wx + (ww - (int)strlen(w1)) / 2, wy + 1, w1, 0x35e0a4, 0x0b1a12);
        draw_text(sc, wx + (ww - (int)strlen(w2)) / 2, wy + 3, w2, 0xb9c8e2, 0x0b1a12);
    }
}

static void draw_help(Screen *sc)
{
    static const char *lines[] = {
        "RAYCASTER - controls",
        "",
        "  W / S            walk forward / back",
        "  A / D            strafe left / right",
        "  LEFT / RIGHT     turn     (or J L , .)",
        "  UP / DOWN        look up / down  (or I K)",
        "  SHIFT + move     sprint",
        "  M                toggle minimap",
        "  F                toggle floor/ceiling casting",
        "  C                toggle crosshair",
        "  T                toggle 24-bit / 256 colour output",
        "  [ / ]            narrow / widen the field of view",
        "  - / =            darker / brighter",
        "  G                toggle the autopilot (demo mode)",
        "  P                pause",
        "  R                generate a fresh maze",
        "  H                hide this help",
        "  Q or ESC         quit",
        "",
        "The scene is a DDA raycast over a grid maze: walls are textured with",
        "procedural 64x64 bitmaps, the floor and ceiling are cast per pixel and",
        "sprites are drawn as billboards through a per-column depth buffer.",
        "Keys carry a little momentum: the terminal only reports presses, never",
        "releases, so each key event refreshes a short hold timer."
    };
    const int n = (int)(sizeof lines / sizeof lines[0]);
    int       w = 0, i, x, y;
    for (i = 0; i < n; i++) {
        int len = (int)strlen(lines[i]);
        if (len > w) w = len;
    }
    w += 4;
    if (w > sc->cols - 2) w = sc->cols - 2;
    if (n + 2 > sc->rows - 2) return;                 /* no room: skip it */
    x = (sc->cols - w) / 2;
    y = (sc->rows - (n + 2)) / 2;
    fill_cells(sc, x, y, w, n + 2, 0x0a0d13);
    for (i = 0; i < n; i++)
        draw_text(sc, x + 2, y + 1 + i, lines[i], i == 0 ? 0xffd76a : 0xb9c8e2, 0x0a0d13);
    draw_text(sc, x, y, "", 0x0a0d13, 0x0a0d13);
}

/* Headless report: an ASCII luminance preview of the same cell grid that the
 * ANSI renderer would have produced, so the engine can be eyeballed without a
 * real terminal (and regression-tested in a script).                        */
static void print_ascii_preview(const Screen *sc)
{
    static const char ramp[] = " .:-=+*#%@";
    const int n = (int)(sizeof ramp - 1);
    int y, x;

    printf("+");
    for (x = 0; x < sc->cols; x++) putchar('-');
    printf("+\n");
    for (y = 0; y < sc->rows; y++) {
        putchar('|');
        for (x = 0; x < sc->cols; x++) {
            const Cell *c = sc->cells + (size_t)y * sc->cols + x;
            if (c->ch == CH_HALF) {
                double l = 0.5 * (lum_of(c->fg) + lum_of(c->bg));
                l *= 1.35;                             /* a little contrast */
                if (l > 255.0) l = 255.0;
                putchar(ramp[(int)(l / 255.0 * (double)(n - 1) + 0.5)]);
            } else {
                putchar((char)c->ch);
            }
        }
        printf("|\n");
    }
    printf("+");
    for (x = 0; x < sc->cols; x++) putchar('-');
    printf("+\n");
}

/* Autopilot: BFS path follower.  Recomputes the shortest grid path from the
 * player's cell to the exit gate every 0.3 s (cached in between), steers
 * toward the next waypoint centre, and slides along walls as a fallback so
 * it never clips or rams head-first.  Looks smart and always escapes.     */
static void autopilot(World *wo, Player *p, double dt)
{
    /* cached path: cell list from player toward exit, recomputed periodically */
    static int pathx[MAPMAX * MAPMAX], pathy[MAPMAX * MAPMAX];
    static int pathlen = 0;
    static double timer = 0.0;
    static int stuck_t = 0;
    static double last_x = 0.0, last_y = 0.0;
    static int plan_tx = -999, plan_ty = -999;   /* exit the plan was built for */
    double step = 2.6 * dt;
    double nx, ny;

    timer -= dt;
    /* replan when the timer expires, the path ran out, the player left the
     * planned corridor (knocked off course), or a new maze moved the exit */
    if (timer <= 0.0 || pathlen <= 0 ||
        wo->exit_x != plan_tx || wo->exit_y != plan_ty) {
        static int dist[MAPMAX][MAPMAX];
        static int qx[MAPMAX * MAPMAX], qy[MAPMAX * MAPMAX];
        static int px[MAPMAX][MAPMAX], py[MAPMAX][MAPMAX];
        int qh = 0, qt = 0, found = 0;
        int sx = (int)p->x, sy = (int)p->y;
        int tx = wo->exit_x, ty = wo->exit_y;
        int x, y, k;
        static const int ox4[4] = { 1, -1, 0, 0 };
        static const int oy4[4] = { 0, 0, 1, -1 };

        for (y = 0; y < wo->h; y++)
            for (x = 0; x < wo->w; x++)
                dist[y][x] = -1;
        /* BFS backwards: exit -> player, so parent pointers lead to exit */
        if (tx >= 0 && ty >= 0 && tx < wo->w && ty < wo->h) {
            dist[ty][tx] = 0;
            qx[qt] = tx; qy[qt] = ty; qt++;
        }
        while (qh < qt) {
            int cx = qx[qh], cy = qy[qh]; qh++;
            if (cx == sx && cy == sy) { found = 1; break; }
            for (k = 0; k < 4; k++) {
                int ax = cx + ox4[k], ay = cy + oy4[k];
                if (ax < 0 || ay < 0 || ax >= wo->w || ay >= wo->h) continue;
                /* walkable, or the gate border cell itself */
                if (wo->grid[ay][ax] != 0 &&
                    !(ax == (int)wo->exit_wx && ay == (int)wo->exit_wy))
                    continue;
                if (dist[ay][ax] != -1) continue;
                dist[ay][ax] = dist[cy][cx] + 1;
                px[ay][ax] = cx; py[ay][ax] = cy;
                qx[qt] = ax; qy[qt] = ay; qt++;
            }
        }
        pathlen = 0;
        if (found) {   /* walk parent chain player -> exit */
            int cx = sx, cy = sy, guard = 0;
            while (!(cx == tx && cy == ty) && guard++ < MAPMAX * MAPMAX) {
                if (pathlen >= MAPMAX * MAPMAX) break;
                pathx[pathlen] = cx; pathy[pathlen] = cy; pathlen++;
                { int nx2 = px[cy][cx], ny2 = py[cy][cx]; cx = nx2; cy = ny2; }
            }
            /* path[0] is the player's own cell: drop it, head for path[1] */
            if (pathlen > 0) {
                int i;
                for (i = 0; i + 1 < pathlen; i++) {
                    pathx[i] = pathx[i + 1]; pathy[i] = pathy[i + 1];
                }
                pathlen--;
            }
        }
        timer = 0.3;
        plan_tx = wo->exit_x; plan_ty = wo->exit_y;
    }

    /* steer toward the next waypoint centre (or the gate itself) */
    {
        double wx, wy, want, have, d, align;
        if (pathlen > 0) {
            wx = (double)pathx[0] + 0.5;
            wy = (double)pathy[0] + 0.5;
            /* reached this waypoint? pop it (by distance OR by cell) */
            if ((fabs(p->x - wx) < 0.35 && fabs(p->y - wy) < 0.35) ||
                ((int)p->x == pathx[0] && (int)p->y == pathy[0])) {
                int i;
                for (i = 0; i + 1 < pathlen; i++) {
                    pathx[i] = pathx[i + 1]; pathy[i] = pathy[i + 1];
                }
                pathlen--;
                if (pathlen > 0) { wx = (double)pathx[0] + 0.5; wy = (double)pathy[0] + 0.5; }
                else             { wx = wo->exit_wx; wy = wo->exit_wy; }
            }
        } else {
            wx = wo->exit_wx; wy = wo->exit_wy;
        }
        want = atan2(wy - p->y, wx - p->x);
        have = atan2(p->diry, p->dirx);
        d = want - have;
        while (d > PI) d -= 2.0 * PI;
        while (d < -PI) d += 2.0 * PI;
        player_rotate(p, dclamp(d * 4.0, -1.0, 1.0) * 3.2 * dt);
        /* slow down when facing away: prevents wide orbits past waypoints */
        align = cos(d);
        step *= dclamp(align, 0.25, 1.0);
    }

    /* walk forward, sliding along walls; unstuck: sidestep + replan */
    nx = p->x + p->dirx * step;
    ny = p->y + p->diry * step;
    if (can_move_exit(wo, nx, ny, 0.24)) {
        p->x = nx;
        p->y = ny;
    } else {
        /* blocked: sidestep perpendicular and force a replan */
        double sx = -p->diry * step, sy = p->dirx * step;
        if (can_move_exit(wo, p->x + sx, p->y + sy, 0.22)) {
            p->x += sx; p->y += sy;
        } else if (can_move_exit(wo, p->x - sx, p->y - sy, 0.22)) {
            p->x -= sx; p->y -= sy;
        } else {
            player_rotate(p, 1.8 * dt);
        }
        timer = 0.0;
    }

    /* stuck detector: barely moved in ~1 s -> replan + turn */
    stuck_t++;
    if (stuck_t >= 60) {
        double dx = p->x - last_x, dy = p->y - last_y;
        if (dx * dx + dy * dy < 0.05 * 0.05) {
            player_rotate(p, 1.2);
            timer = 0.0;
        }
        last_x = p->x; last_y = p->y;
        stuck_t = 0;
    }
}

/* ==========================================================================
 *  COMMAND LINE + MAIN LOOP
 * ========================================================================== */
static void usage(FILE *f)
{
    fprintf(f,
        "raycaster -- a terminal raycasting engine\n"
        "\n"
        "usage: raycaster [options]\n"
        "\n"
        "  --size WxH       maze size, forced odd (default 31x31, max 63x63)\n"
        "  --seed N         maze seed (default 20260927)\n"
        "  --fov DEG        horizontal field of view (default 66)\n"
        "  --fps N          render rate cap (default 60)\n"
        "  --frames N       stop after N frames (handy with --headless)\n"
        "  --headless       render off screen, print stats + an ASCII preview\n"
        "  --selftest       verify the raycaster against a brute-force oracle\n"
        "  --screen WxH     off-screen canvas for --headless (default 100x40)\n"
        "  --demo           start with the autopilot switched on\n"
        "  --no-floor       skip floor/ceiling casting (faster on slow terminals)\n"
        "  --no-map         start without the minimap\n"
        "  --truecolor      force 24-bit colour (default: xterm-256)\n"
        "  --256            force xterm-256 colour\n"
        "  --help           this text\n"
        "\n"
        "keys: W/S walk, A/D strafe, arrows or J/L turn, I/K look up-down,\n"
        "      SHIFT+move run, M map, F floor, C crosshair, T colour, [ ] fov,\n"
        "      - = brightness, G autopilot, P pause, R new maze, H help, Q quit\n");
}

static int parse_args(Options *o, int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 1; }
        else if (!strcmp(a, "--headless"))  o->headless = 1;
        else if (!strcmp(a, "--selftest"))  o->selftest = 1;
        else if (!strcmp(a, "--demo"))      o->demo = 1;
        else if (!strcmp(a, "--no-floor"))  o->floor_cast = 0;
        else if (!strcmp(a, "--no-map"))    o->minimap = 0;
        else if (!strcmp(a, "--truecolor")) o->mode = CM_TRUECOLOR;
        else if (!strcmp(a, "--256"))       o->mode = CM_ANSI256;
        else if (!strcmp(a, "--size")   && i + 1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2) { o->mapw = w; o->maph = h; }
        }
        else if (!strcmp(a, "--screen") && i + 1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2) { o->head_cols = w; o->head_rows = h; }
        }
        else if (!strcmp(a, "--seed")   && i + 1 < argc) o->seed = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--fov")    && i + 1 < argc) o->fov = atof(argv[++i]);
        else if (!strcmp(a, "--fps")    && i + 1 < argc) o->target_fps = atoi(argv[++i]);
        else if (!strcmp(a, "--frames") && i + 1 < argc) o->frames = atol(argv[++i]);
        else fprintf(stderr, "raycaster: ignoring unknown option '%s'\n", a);
    }
    return 0;
}

static u32 fb_checksum(const Screen *sc)
{
    u32 h = 2166136261u;
    size_t i, n = (size_t)sc->pw * (size_t)sc->ph;
    for (i = 0; i < n; i++) {
        h ^= sc->pix[i];
        h *= 16777619u;
    }
    return h;
}

static int count_escapes(const Screen *sc)
{
    size_t i;
    int    n = 0;
    for (i = 0; i < sc->out.len; i++)
        if (sc->out.data[i] == 0x1b) n++;
    return n;
}

/* ==========================================================================
 *  SELF TEST
 *  Builds a hand-made world with known geometry and checks the engine against
 *  an independent brute-force oracle (march the ray in 2 mm steps, then bisect
 *  the first wall crossing) plus a handful of pixel-level facts.
 *  Run with --selftest; the process exits non-zero if a check fails.
 * ========================================================================== */
static int g_checks = 0, g_fails = 0;

static void check(int cond, const char *what, double got, double want)
{
    g_checks++;
    if (!cond) g_fails++;
    printf("  [%s] %-44s got %10.4f  want %10.4f\n",
           cond ? "ok" : "FAIL", what, got, want);
}

static void check_ray(int wmx, int wmy, int wside, double wperp, RayHit h, const char *what)
{
    int ok = (h.mx == wmx && h.my == wmy && h.side == wside && fabs(h.perp - wperp) < 1e-9);
    g_checks++;
    if (!ok) g_fails++;
    printf("  [%s] %-44s got cell(%d,%d) side %d d %.6f\n",
           ok ? "ok" : "FAIL", what, h.mx, h.my, h.side, h.perp);
}

static int selftest(void)
{
    World   wo;
    Screen  sc;
    Player  p;
    Options o;
    int     x, y, k, i;
    double  px, py, a;

    printf("raycaster self test\n");

    /* ---- hand-made world: 16x16, open room, a brick slab at x=6 --------- */
    memset(&wo, 0, sizeof wo);
    wo.w = 16; wo.h = 16;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)
            wo.grid[y][x] = T_STONE;               /* solid everywhere      */
    for (y = 1; y <= 13; y++)
        for (x = 1; x <= 13; x++)
            wo.grid[y][x] = 0;                     /* open room x,y = 1..13 */
    for (y = 4; y <= 8; y++)
        wo.grid[y][6] = T_BRICK;                   /* slab at x = 6         */

    gen_textures();

    printf("\n* exact rays, hand computed\n");
    check_ray(6, 5, 0, 3.5, trace_ray(&wo, 2.5, 5.5,  1.0,  0.0), "+x ray hits the brick slab");
    check_ray(0, 5, 0, 1.5, trace_ray(&wo, 2.5, 5.5, -1.0,  0.0), "-x ray hits the west wall");
    check_ray(2, 0, 1, 4.5, trace_ray(&wo, 2.5, 5.5,  0.0, -1.0), "-y ray hits the north wall");
    check_ray(2, 14, 1, 8.5, trace_ray(&wo, 2.5, 5.5, 0.0,  1.0), "+y ray hits the south wall");
    check_ray(6, 6, 0, 2.5, trace_ray(&wo, 9.5, 6.5, -1.0,  0.0), "-x ray hits the slab's far side");

    /* ---- 400 random rays against the brute-force oracle ------------------ */
    printf("\n* 400 random rays against a brute-force oracle\n");
    {
        int    bad = 0, tested = 0;
        double worst = 0.0;
        rng_seed(0xc0ffeeu);
        for (k = 0; k < 400; k++) {
            double rx, ry, lo, hi, ref, diff, tc, hx, hy;
            RayHit hit;
            int    cell_mismatch = 0, found = 0;
            px = 1.2 + 11.6 * rng_d();
            py = 1.2 + 11.6 * rng_d();
            if (wo.grid[(int)py][(int)px] != 0) continue;   /* never start in a wall */
            tested++;
            a  = rng_d() * 2.0 * PI;
            rx = cos(a);
            ry = sin(a);
            hit = trace_ray(&wo, px, py, rx, ry);

            for (i = 0; i < 12500; i++) {          /* walk until we hit     */
                double t = (double)i * 0.002;
                int    cx = (int)floor(px + rx * t), cy = (int)floor(py + ry * t);
                if (cx < 0 || cy < 0 || cx >= wo.w || cy >= wo.h || wo.grid[cy][cx]) {
                    found = 1;
                    lo = t > 0.002 ? t - 0.002 : 0.0;
                    hi = t;
                    break;
                }
            }
            if (!found) { bad++; continue; }
            for (i = 0; i < 60; i++) {             /* bisect the crossing   */
                double mid = 0.5 * (lo + hi);
                int    cx = (int)floor(px + rx * mid), cy = (int)floor(py + ry * mid);
                if (cx < 0 || cy < 0 || cx >= wo.w || cy >= wo.h || wo.grid[cy][cx]) hi = mid;
                else                                                                  lo = mid;
            }
            tc  = 0.5 * (lo + hi);
            /* the rays are unit length and the ray basis is orthonormal, so
             * the ray parameter at the crossing IS the perpendicular camera
             * distance the renderer is supposed to use */
            ref = tc;
            diff = fabs(ref - hit.perp);
            if (diff > worst) worst = diff;
            hx = px + rx * tc;
            hy = py + ry * tc;
            /* which cell owns a point that sits exactly on a grid line is a
             * matter of rounding, so only judge the cell when it is not */
            if (fabs(hx - floor(hx + 0.5)) > 1e-6 && fabs(hy - floor(hy + 0.5)) > 1e-6) {
                if ((int)floor(hx) != hit.mx || (int)floor(hy) != hit.my) cell_mismatch = 1;
            }
            if (diff > 1e-7 || cell_mismatch) bad++;
        }
        check(bad == 0, "rays agreeing with the oracle", (double)bad, 0.0);
        check(tested >= 380, "rays actually tested", (double)tested, 400.0);
        check(worst < 1e-7, "worst perpendicular-distance error", worst, 0.0);
    }

    /* ---- pixel level: wall band, ceiling and floor ----------------------- */
    printf("\n* one rendered frame of the hand-made room\n");
    screen_init(&sc);
    screen_resize(&sc, 40, 20);                    /* 40 x 40 pixels        */
    memset(&o, 0, sizeof o);
    o.fov = 66.0; o.bright = 1.0; o.floor_cast = 1; o.crosshair = 1;
    memset(&p, 0, sizeof p);
    p.x = 2.5; p.y = 5.5; p.dirx = 1.0; p.diry = 0.0;
    player_set_fov(&p, o.fov);
    render_view(&sc, &wo, &p, &o, 0.0);
    {
        /* the slab is 3.5 units away, so it projects to 40/3.5 = 11.4 px and
         * straddles the horizon: rows 15..24 of the 40 px centre column.
         * Bricks are reddish, floors and ceilings are not, so R-G > 24 is a
         * reliable "this pixel came from the brick texture" test.          */
        int    top = -1, bot = -1;
        double ceil_lum = lum_of(sc.pix[2 * sc.pw + sc.pw / 2]);
        double wall_lum = lum_of(sc.pix[20 * sc.pw + sc.pw / 2]);
        for (y = 0; y < sc.ph; y++) {
            u32    c = sc.pix[(size_t)y * sc.pw + sc.pw / 2];
            int    rg = (int)((c >> 16) & 255u) - (int)((c >> 8) & 255u);
            if (rg > 24) { if (top < 0) top = y; bot = y; }
        }
        check(top >= 14 && top <= 17, "first brick pixel row", (double)top, 15.0);
        check((bot - top + 1) >= 8 && (bot - top + 1) <= 12, "wall band height in px",
              (double)(bot - top + 1), 10.0);
        check(((top + bot) / 2) >= 19 && ((top + bot) / 2) <= 21, "wall band centred on the horizon",
              (double)((top + bot) / 2), 20.0);
        check(wall_lum > 45.0, "wall brighter than the fogged ceiling", wall_lum, 60.0);
        check(ceil_lum < 30.0, "ceiling is dark", ceil_lum, 10.0);
    }

    /* ---- sprites: visibility, then occlusion by the z-buffer ------------- */
    printf("\n* billboard sprites\n");
    wo.nspr = 1;
    wo.spr[0].x = 5.5; wo.spr[0].y = 5.5;            /* 3 units ahead       */
    wo.spr[0].type = S_ORB;  wo.spr[0].scale = 1.0;
    wo.spr[0].vmove = 0.0;   wo.spr[0].glow = 0.9;
    render_view(&sc, &wo, &p, &o, 0.0);
    {
        double best = -1.0;
        int    bc = 0, br = 0;
        for (y = 0; y < sc.ph; y++)
            for (x = 0; x < sc.pw; x++) {
                double l = lum_of(sc.pix[(size_t)y * sc.pw + x]);
                if (l > best) { best = l; bc = x; br = y; }
            }
        check(best > 150.0, "glowing orb is the brightest pixel", best, 200.0);
        check(bc >= 18 && bc <= 22, "orb centred in the middle column", (double)bc, 20.0);
        check(br >= 14 && br <= 26, "orb centred on the horizon", (double)br, 20.0);
    }
    wo.spr[0].x = 9.5;                               /* now behind the slab */
    render_view(&sc, &wo, &p, &o, 0.0);
    {
        double best = -1.0;
        for (y = 0; y < sc.ph; y++)
            for (x = 0; x < sc.pw; x++) {
                double l = lum_of(sc.pix[(size_t)y * sc.pw + x]);
                if (l > best) best = l;
            }
        check(best < 120.0, "orb correctly hidden by the z-buffer", best, 60.0);
    }

    /* ---- text layers must survive silly canvas sizes --------------------- */
    printf("\n* text layers on assorted canvas sizes\n");
    {
        static const int sizes[6][2] = { {20,8}, {21,9}, {37,13}, {64,18}, {80,24}, {200,60} };
        int ok = 1;
        for (k = 0; k < 6; k++) {
            screen_resize(&sc, sizes[k][0], sizes[k][1]);
            pix_to_cells(&sc);
            if (o.minimap) draw_minimap(&sc, &wo, &p);
            draw_hud(&sc, &wo, &p, &o, 60.0, 1.0);
            draw_help(&sc);
            present_build(&sc, &o);
            if (sc.out.len < (size_t)sc.cols * (size_t)sc.rows) ok = 0;
        }
        check(ok, "ANSI stream built on 6 canvas sizes", ok ? 1.0 : 0.0, 1.0);
    }

    /* ---- exit gate: exactly one border opening, reachable from spawn ----- */
    printf("\n* maze exit gate\n");
    {
        World w2;
        int seeds[3] = { 1, 7, 20260927 };
        int ok = 1, si;
        for (si = 0; si < 3; si++) {
            static int dist2[MAPMAX][MAPMAX];
            static int qx2[MAPMAX * MAPMAX], qy2[MAPMAX * MAPMAX];
            int qh = 0, qt = 0, openings = 0, reached = 0;
            world_generate(&w2, 31, 31, (u32)seeds[si]);
            for (y = 0; y < w2.h; y++) {
                for (x = 0; x < w2.w; x++) {
                    int border = (x == 0 || y == 0 || x == w2.w - 1 || y == w2.h - 1);
                    if (border && w2.grid[y][x] == 0) openings++;
                    dist2[y][x] = -1;
                }
            }
            if (openings != 1) ok = 0;
            /* flood fill from spawn over walkables incl. the gate */
            qh = 0; qt = 0;
            qx2[qt] = (int)w2.spawn_x; qy2[qt] = (int)w2.spawn_y; qt++;
            dist2[(int)w2.spawn_y][(int)w2.spawn_x] = 0;
            while (qh < qt) {
                int cx = qx2[qh], cy = qy2[qh]; qh++;
                static const int ox4[4] = { 1, -1, 0, 0 };
                static const int oy4[4] = { 0, 0, 1, -1 };
                for (i = 0; i < 4; i++) {
                    int nx = cx + ox4[i], ny = cy + oy4[i];
                    if (nx < 0 || ny < 0 || nx >= w2.w || ny >= w2.h) continue;
                    if (w2.grid[ny][nx] != 0) continue;
                    if (dist2[ny][nx] != -1) continue;
                    dist2[ny][nx] = dist2[cy][cx] + 1;
                    qx2[qt] = nx; qy2[qt] = ny; qt++;
                }
            }
            if (w2.exit_x < 0 || w2.exit_y < 0) ok = 0;
            else if (dist2[w2.exit_y][w2.exit_x] < 0) ok = 0;
            else if (dist2[(int)w2.exit_wy][(int)w2.exit_wx] < 0) ok = 0;
            else reached = 1;
            if (!reached) ok = 0;
        }
        check(ok, "one reachable border exit on 3 seeds", ok ? 1.0 : 0.0, 1.0);
    }

    /* ---- autopilot: BFS follower must reach the exit on 3 seeds ---------- */
    printf("\n* autopilot escapes the maze\n");
    {
        int seeds[3] = { 1, 7, 20260927 };
        int ok = 1, si;
        for (si = 0; si < 3; si++) {
            World w2;
            Player p2;
            Options o2;
            Axes ax2;
            int f;
            memset(&o2, 0, sizeof o2);
            memset(&ax2, 0, sizeof ax2);
            o2.fov = 66.0;
            world_generate(&w2, 31, 31, (u32)seeds[si]);
            player_spawn(&p2, &w2, o2.fov);
            for (f = 0; f < 60 * 120 && !o2.won; f++) {  /* up to 120 s */
                autopilot(&w2, &p2, 1.0 / 60.0);
                update_player(&o2, &ax2, &p2, &w2, 1.0 / 60.0);
            }
            if (!o2.won) ok = 0;
        }
        check(ok, "autopilot escaped on 3 seeds in <120s", ok ? 1.0 : 0.0, 1.0);
    }

#ifdef __EMSCRIPTEN__
    /* ---- web present: RGBA + text-cell exports for the JS canvas ----------- */
    printf("\n* web present exports\n");
    {
        int ok = 1;
        present_flush(&sc);                   /* fills g_web_rgba / g_web_txt */
        if (!g_web_rgba || g_web_rgba_cap < (size_t)sc.pw * (size_t)sc.ph * 4)
            ok = 0;
        if (g_web_cols != sc.cols || g_web_rows != sc.rows) ok = 0;
        if (ok) {                             /* first RGBA pixel == fb pixel  */
            u32 c0 = sc.pix[0];
            if (g_web_rgba[0] != (unsigned char)((c0 >> 16) & 255u) ||
                g_web_rgba[1] != (unsigned char)((c0 >>  8) & 255u) ||
                g_web_rgba[2] != (unsigned char)( c0        & 255u) ||
                g_web_rgba[3] != 255u) ok = 0;
        }
        check(ok, "web RGBA export matches the framebuffer", ok ? 1.0 : 0.0, 1.0);
    }
    {
        int ok = 0, i, n;
        const int *t;
        draw_text(&sc, 0, 0, "AB", 0x112233u, 0x445566u);
        present_flush(&sc);
        n = web_text_count();
        t = web_text_ptr();
        for (i = 0; i + 4 < n * 5; i += 5)
            if (t[i] == 0 && t[i + 1] == 0 && t[i + 2] == 'A' &&
                (u32)t[i + 3] == 0x112233u && (u32)t[i + 4] == 0x445566u)
                ok = 1;
        check(ok, "web text-cell export carries glyphs", ok ? 1.0 : 0.0, 1.0);
    }
#endif

    screen_free(&sc);
    printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}


/* ==========================================================================
 *  GAME LOOP
 *  One simulation + render + present step, extracted so the native build can
 *  drive it from a plain while() loop and the web build from rAF.  Returns 1
 *  when the game should stop (quit, signal, or --frames reached).
 * ========================================================================== */
typedef struct {
    Options o;
    World   wo;
    Player  p;
    Screen  sc;
    Axes    ax;
    double  t0, last;              /* epoch of start / previous frame        */
    double  fps, frame_ms;         /* smoothed stats for the HUD             */
    long    frame;
} Game;

static int game_step(Game *g)
{
    Options *o = &g->o;
    Screen  *sc = &g->sc;
    double   now, dt, rstart;
#ifndef __EMSCRIPTEN__
    int cols = 80, rows = 24;
#endif

    if (o->quit || g_quit) return 1;

    now = now_sec();
    dt  = now - g->last;
    g->last = now;
    if (dt > 0.10) dt = 0.10;                 /* don't lurch after a stall    */
    if (dt < 0.0)  dt = 0.0;
    if (o->headless) dt = 1.0 / 60.0;         /* deterministic in reports     */

#ifdef __EMSCRIPTEN__
    if (g_web_want_cols > 0) {                /* JS resize, applied here      */
        screen_resize(sc, g_web_want_cols, g_web_want_rows);
        g_web_want_cols = 0;
    }
#else
    if (g_winch) {
        g_winch = 0;
        if (!o->headless) {
            term_size(&cols, &rows);
            screen_resize(sc, cols, rows);
        }
    }
#endif
    if (o->sprint_t > 0.0) {
        o->sprint_t -= dt;
        if (o->sprint_t <= 0.0) { o->sprint = 0; o->sprint_t = 0.0; }
    }
    if (o->new_maze) {
        o->new_maze = 0;
        world_generate(&g->wo, o->mapw, o->maph, rng_next() ^ o->seed);
        player_spawn(&g->p, &g->wo, o->fov);
        memset(&g->ax, 0, sizeof g->ax);
        o->won = 0; o->elapsed = 0.0; o->steps = 0;
    }

    if (!o->paused && !o->won) o->elapsed += dt;

    if (o->headless) {
        if (o->demo) autopilot(&g->wo, &g->p, dt);
    } else {
        if (o->demo && !o->won) autopilot(&g->wo, &g->p, dt);
        handle_input(o, &g->ax);
    }
    axes_decay(&g->ax, dt);
    if (!o->paused) update_player(o, &g->ax, &g->p, &g->wo, dt);
    if (o->won) o->demo = 0;   /* stop the autopilot on the gate */
    {   /* keep the pitch inside what the screen can show */
        int lim = (int)((double)sc->ph * 0.44);
        if (o->pitch >  lim) o->pitch =  lim;
        if (o->pitch < -lim) o->pitch = -lim;
    }

    rstart = now_sec();
    render_view(sc, &g->wo, &g->p, o, now - g->t0);
    pix_to_cells(sc);
    if (o->minimap) draw_minimap(sc, &g->wo, &g->p);
    if (o->help)    draw_help(sc);
    draw_hud(sc, &g->wo, &g->p, o, g->fps, g->frame_ms);
    present_build(sc, o);
    if (!o->headless) present_flush(sc);

    {
        double spent = now_sec() - rstart;
        g->frame_ms = g->frame_ms * 0.88 + spent * 1000.0 * 0.12;
    }
    if (dt > 0.0) g->fps = g->fps * 0.88 + (1.0 / dt) * 0.12;

    g->frame++;
    if (o->frames > 0 && g->frame >= o->frames) return 1;

#ifndef __EMSCRIPTEN__
    if (!o->headless) {                       /* frame cap; web is paced by rAF */
        double budget = 1.0 / (double)o->target_fps;
        double spent  = now_sec() - now;
        if (spent < budget) sleep_sec(budget - spent);
    }
#endif
    return o->quit || g_quit;
}

#ifdef __EMSCRIPTEN__
static void web_tick(void *arg)
{
    Game *g = (Game *)arg;
    if (game_step(g)) emscripten_cancel_main_loop();
}
#endif

int main(int argc, char **argv)
{
    Game    g;
    int     cols = 80, rows = 24;
    int     open_cells = 0;
    int     i;

    memset(&g, 0, sizeof g);
    g.o.mapw = 31;  g.o.maph = 31;
    g.o.fov = 66.0; g.o.seed = 20260927u; g.o.target_fps = 60;
    g.o.floor_cast = 1; g.o.minimap = 1; g.o.crosshair = 1; g.o.bright = 1.0;
    g.o.mode = CM_ANSI256;
    g.o.head_cols = 100; g.o.head_rows = 40;

    {   /* most modern terminals advertise 24-bit colour here */
        const char *ct = getenv("COLORTERM");
        if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit")))
            g.o.mode = CM_TRUECOLOR;
    }

    if (parse_args(&g.o, argc, argv)) return 0;
    if (g.o.selftest) return selftest();
    g.o.target_fps = iclamp(g.o.target_fps, 1, 1000);
    if (g.o.head_cols < 20) g.o.head_cols = 20;
    if (g.o.head_rows < 8)  g.o.head_rows = 8;

#ifndef __EMSCRIPTEN__
    if (!g.o.headless && !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "raycaster: stdout is not a terminal, switching to --headless\n");
        g.o.headless = 1;
    }
#endif

    gen_textures();
    world_generate(&g.wo, g.o.mapw, g.o.maph, g.o.seed);
    player_spawn(&g.p, &g.wo, g.o.fov);
    screen_init(&g.sc);

    if (g.o.headless) {
        screen_resize(&g.sc, g.o.head_cols, g.o.head_rows);
    } else {
        term_size(&cols, &rows);
        screen_resize(&g.sc, cols, rows);
        term_setup();
        atexit(term_restore);
    }

    for (i = 0; i < g.wo.w * g.wo.h; i++) {             /* spacing sanity */
        int x = i % g.wo.w, y = i / g.wo.w;
        if (g.wo.grid[y][x] == 0) open_cells++;
    }

    g.t0 = g.last = now_sec();

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(web_tick, &g, 0, 1);   /* rAF; never returns */
#else
    while (!g.o.quit && !g_quit)
        if (game_step(&g)) break;
#endif
    double total_ms = (now_sec() - g.t0) * 1000.0;

    if (g.o.headless) {
        printf("raycaster -- headless report\n");
        printf("  maze            %dx%d, seed %u, %d walkable cells, %d sprites\n",
               g.wo.w, g.wo.h, g.o.seed, open_cells, g.wo.nspr);
        printf("  canvas          %d x %d cells  (%d x %d pixels, half-block)\n",
               g.sc.cols, g.sc.rows, g.sc.pw, g.sc.ph);
        printf("  frames          %ld\n", g.frame);
        printf("  total           %.2f ms\n", total_ms);
        printf("  per frame       %.3f ms  ->  %.1f fps\n",
               g.frame > 0 ? total_ms / (double)g.frame : 0.0,
               g.frame > 0 ? (double)g.frame * 1000.0 / total_ms : 0.0);
        printf("  ANSI frame      %zu bytes, %d escape sequences\n",
               g.sc.out.len, count_escapes(&g.sc));
        printf("  end position    %.3f, %.3f   dir %.3f, %.3f  pitch %d\n",
               g.p.x, g.p.y, g.p.dirx, g.p.diry, g.o.pitch);
        printf("  frame checksum  0x%08x\n", fb_checksum(&g.sc));
        print_ascii_preview(&g.sc);
    }

    screen_free(&g.sc);
    return 0;
}















