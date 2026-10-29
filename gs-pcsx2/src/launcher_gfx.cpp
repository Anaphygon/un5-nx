// SPDX-License-Identifier: GPL-3.0+
// The launcher's drawing: a 1280x720 framebuffer with rectangles and text in
// the console's shared font. Taken from switch-devtools (MIT,
// github.com/Lisvindanu/switch-devtools, source/gfx.c), as C++.
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "launcher_gfx.h"

static Framebuffer g_fb;
static u32 *g_px;
static u32 g_stride;  /* in pixels */
static FT_Library g_ft;
static FT_Face g_face, g_ext;
static bool g_pl;
static int g_clip0 = 0, g_clip1 = GFX_H;

const char *GLYPH_A = "\uE0E0", *GLYPH_B = "\uE0E1", *GLYPH_X = "\uE0E2", *GLYPH_Y = "\uE0E3",
           *GLYPH_L = "\uE0E4", *GLYPH_R = "\uE0E5", *GLYPH_PLUS = "\uE0EF", *GLYPH_MINUS = "\uE0F0";

bool gfx_has_glyphs(void) { return g_ext != NULL; }
void gfx_clip(int y0, int y1) { g_clip0 = y0 < 0 ? 0 : y0; g_clip1 = y1 > GFX_H ? GFX_H : y1; }
void gfx_unclip(void) { g_clip0 = 0; g_clip1 = GFX_H; }

/* ---- glyph cache ----
 * Rendered coverage bitmaps keyed by (codepoint, pixel size). The UI uses a
 * handful of sizes and mostly ASCII, so a small open-addressed table is
 * plenty; on overflow it simply starts over. */

typedef struct {
    u32 cp;
    u16 px;
    s16 left, top, adv;
    u16 w, h;
    u8 *bits;
} Glyph;

#define CACHE_SIZE 1024
static Glyph g_cache[CACHE_SIZE];
static int g_cached;

static void cache_flush(void)
{
    for (int i = 0; i < CACHE_SIZE; i++) free(g_cache[i].bits);
    memset(g_cache, 0, sizeof g_cache);
    g_cached = 0;
}

static Glyph *glyph(u32 cp, int px)
{
    u32 h = (cp * 2654435761u) ^ (u32)px;
    for (int i = 0; i < CACHE_SIZE; i++) {
        Glyph *g = &g_cache[(h + i) % CACHE_SIZE];
        if (g->px == px && g->cp == cp) return g;
        if (g->px == 0) {
            if (g_cached > CACHE_SIZE * 3 / 4) { cache_flush(); return glyph(cp, px); }
            /* Button glyphs live in the NintendoExt font's private-use range. */
            FT_Face face = (cp >= 0xE000 && cp <= 0xF8FF && g_ext) ? g_ext : g_face;
            FT_Set_Pixel_Sizes(face, 0, px);
            if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) return NULL;
            FT_GlyphSlot s = face->glyph;
            g->cp = cp;
            g->px = px;
            g->left = s->bitmap_left;
            g->top = s->bitmap_top;
            g->adv = s->advance.x >> 6;
            g->w = s->bitmap.width;
            g->h = s->bitmap.rows;
            g->bits = static_cast<u8 *>(malloc((size_t)g->w * g->h + 1));
            for (int y = 0; y < g->h; y++)
                memcpy(g->bits + y * g->w, s->bitmap.buffer + y * s->bitmap.pitch, g->w);
            g_cached++;
            return g;
        }
    }
    return NULL;
}

bool gfx_init(void)
{
    if (R_FAILED(framebufferCreate(&g_fb, nwindowGetDefault(), GFX_W, GFX_H, PIXEL_FORMAT_RGBA_8888, 2)))
        return false;
    framebufferMakeLinear(&g_fb);

    PlFontData font;
    if (R_FAILED(plInitialize(PlServiceType_User))) return false;
    g_pl = true;
    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) return false;
    if (FT_Init_FreeType(&g_ft)) return false;
    if (FT_New_Memory_Face(g_ft, static_cast<const FT_Byte *>(font.address), font.size, 0, &g_face)) return false;
    PlFontData ext;
    if (R_SUCCEEDED(plGetSharedFontByType(&ext, PlSharedFontType_NintendoExt)) &&
        FT_New_Memory_Face(g_ft, static_cast<const FT_Byte *>(ext.address), ext.size, 0, &g_ext))
        g_ext = NULL;
    return true;
}

void gfx_exit(void)
{
    cache_flush();
    if (g_ext) FT_Done_Face(g_ext);
    if (g_face) FT_Done_Face(g_face);
    if (g_ft) FT_Done_FreeType(g_ft);
    if (g_pl) plExit();
    framebufferClose(&g_fb);
}

void gfx_begin(void)
{
    u32 stride_bytes;
    g_px = static_cast<u32 *>(framebufferBegin(&g_fb, &stride_bytes));
    g_stride = stride_bytes / sizeof(u32);
}

void gfx_end(void)
{
    framebufferEnd(&g_fb);
}

void gfx_clear(Color c)
{
    for (int y = 0; y < GFX_H; y++) {
        u32 *row = g_px + y * g_stride;
        for (int x = 0; x < GFX_W; x++) row[x] = c;
    }
}

void gfx_rect(int x, int y, int w, int h, Color c)
{
    int x0 = x < 0 ? 0 : x, y0 = y < g_clip0 ? g_clip0 : y;
    int x1 = x + w > GFX_W ? GFX_W : x + w, y1 = y + h > g_clip1 ? g_clip1 : y + h;
    for (int yy = y0; yy < y1; yy++) {
        u32 *row = g_px + yy * g_stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

static inline u32 blend(u32 dst, Color src, u32 a);

void gfx_shade(int x, int y, int w, int h, Color c, int a)
{
    int x0 = x < 0 ? 0 : x, y0 = y < g_clip0 ? g_clip0 : y;
    int x1 = x + w > GFX_W ? GFX_W : x + w, y1 = y + h > g_clip1 ? g_clip1 : y + h;
    for (int yy = y0; yy < y1; yy++) {
        u32 *row = g_px + yy * g_stride;
        for (int xx = x0; xx < x1; xx++) row[xx] = blend(row[xx], c, (u32)a);
    }
}

static Color mix(Color a, Color b, int t, int n)
{
    u32 out = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= (u32)(ca + (cb - ca) * t / (n ? n : 1)) << sh;
    }
    return out;
}

void gfx_border(int x, int y, int w, int h, int t, Color top, Color bottom)
{
    gfx_rect(x, y, w, t, top);
    gfx_rect(x, y + h - t, w, t, bottom);
    for (int i = t; i < h - t; i++) {
        Color c = mix(top, bottom, i, h);
        gfx_rect(x, y + i, t, 1, c);
        gfx_rect(x + w - t, y + i, t, 1, c);
    }
}

static inline u32 blend(u32 dst, Color src, u32 a)
{
    u32 ia = 255 - a;
    u32 r = ((src & 0xFF) * a + (dst & 0xFF) * ia) / 255;
    u32 g = (((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia) / 255;
    u32 b = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* Coverage of pixel (dx, dy) in the top-left corner square of a radius-r
 * rounded shape: 255 inside, 0 outside, a one-pixel ramp along the edge. */
static u32 corner_alpha(int dx, int dy, int r)
{
    float fx = r - dx - 0.5f, fy = r - dy - 0.5f;
    float d = fx * fx + fy * fy, rr = (float)r * r, in = (float)(r - 1) * (r - 1);
    return d <= in ? 255 : d >= rr ? 0 : (u32)(255 * (rr - d) / (rr - in));
}

/* Rounded corners with one pixel of coverage-based edge, enough to read as
 * smooth at this resolution. */
void gfx_round(int x, int y, int w, int h, int r, Color c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    gfx_rect(x + r, y, w - 2 * r, h, c);
    gfx_rect(x, y + r, r, h - 2 * r, c);
    gfx_rect(x + w - r, y + r, r, h - 2 * r, c);
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            u32 a = corner_alpha(dx, dy, r);
            if (!a) continue;
            int px[4] = { x + dx, x + w - 1 - dx, x + dx, x + w - 1 - dx };
            int py[4] = { y + dy, y + dy, y + h - 1 - dy, y + h - 1 - dy };
            for (int k = 0; k < 4; k++) {
                if (px[k] < 0 || py[k] < g_clip0 || px[k] >= GFX_W || py[k] >= g_clip1) continue;
                u32 *p = g_px + py[k] * g_stride + px[k];
                *p = blend(*p, c, a);
            }
        }
    }
}

void gfx_image_round(int x, int y, int w, int h, int r, const u32 *src)
{
    for (int iy = 0; iy < h; iy++) {
        int yy = y + iy;
        if (yy < g_clip0 || yy >= g_clip1) continue;
        u32 *row = g_px + yy * g_stride;
        int cy = iy < r ? iy : iy >= h - r ? h - 1 - iy : -1;
        for (int ix = 0; ix < w; ix++) {
            int xx = x + ix;
            if (xx < 0 || xx >= GFX_W) continue;
            int cx = ix < r ? ix : ix >= w - r ? w - 1 - ix : -1;
            u32 a = (cx >= 0 && cy >= 0) ? corner_alpha(cx, cy, r) : 255;
            if (a == 255) row[xx] = src[iy * w + ix] | 0xFF000000u;
            else if (a) row[xx] = blend(row[xx], src[iy * w + ix], a);
        }
    }
}

static u32 next_cp(const char **s)
{
    const u8 *p = (const u8 *)*s;
    u32 cp;
    int n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
    else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3;
    } else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); n = 4;
    } else { cp = '?'; n = 1; }
    *s += n;
    return cp;
}

int gfx_text(int x, int y, int px, Color c, const char *s)
{
    int pen = x, base = y + px * 4 / 5;
    while (*s) {
        Glyph *g = glyph(next_cp(&s), px);
        if (!g) continue;
        for (int gy = 0; gy < g->h; gy++) {
            int yy = base - g->top + gy;
            if (yy < g_clip0 || yy >= g_clip1) continue;
            u32 *row = g_px + yy * g_stride;
            for (int gx = 0; gx < g->w; gx++) {
                int xx = pen + g->left + gx;
                u32 a = g->bits[gy * g->w + gx];
                if (a && xx >= 0 && xx < GFX_W) row[xx] = blend(row[xx], c, a);
            }
        }
        pen += g->adv;
    }
    return pen - x;
}

int gfx_text_width(int px, const char *s)
{
    int w = 0;
    while (*s) {
        Glyph *g = glyph(next_cp(&s), px);
        if (g) w += g->adv;
    }
    return w;
}

void gfx_text_fit(int x, int y, int px, Color c, const char *s, int max_w)
{
    if (gfx_text_width(px, s) <= max_w) { gfx_text(x, y, px, c, s); return; }
    char buf[256];
    int ell = gfx_text_width(px, "…");
    size_t n = 0;
    const char *p = s;
    int w = 0;
    while (*p) {
        const char *start = p;
        Glyph *g = glyph(next_cp(&p), px);
        int adv = g ? g->adv : 0;
        if (w + adv + ell > max_w || n + (size_t)(p - start) > sizeof buf - 4) break;
        memcpy(buf + n, start, (size_t)(p - start));
        n += (size_t)(p - start);
        w += adv;
    }
    memcpy(buf + n, "…", 4);
    gfx_text(x, y, px, c, buf);
}
