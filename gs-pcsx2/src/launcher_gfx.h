// SPDX-License-Identifier: GPL-3.0+
// From switch-devtools (MIT), source/gfx.h.
#ifndef UN5_LAUNCHER_GFX_H
#define UN5_LAUNCHER_GFX_H

#include <stdbool.h>
#include <stdint.h>

/* A 1280x720 RGBA framebuffer with rectangles and text in the console's own
 * shared font. Everything is drawn in software; a frame is only redrawn when
 * something on it changed. */

#define GFX_W 1280
#define GFX_H 720

typedef uint32_t Color;  /* 0xAABBGGRR, the framebuffer's byte order */
#define RGB(r, g, b) ((Color)(0xFF000000u | ((b) << 16) | ((g) << 8) | (r)))

bool gfx_init(void);
void gfx_exit(void);

void gfx_begin(void);
void gfx_end(void);

/* Drawing below is limited to rows y0..y1-1 until gfx_unclip(). */
void gfx_clip(int y0, int y1);
void gfx_unclip(void);

void gfx_clear(Color c);
/* c blended at alpha a (0..255) over what is there. */
void gfx_shade(int x, int y, int w, int h, Color c, int a);
/* A border t pixels thick whose color runs from top to bottom. */
void gfx_border(int x, int y, int w, int h, int t, Color top, Color bottom);
void gfx_rect(int x, int y, int w, int h, Color c);
void gfx_round(int x, int y, int w, int h, int r, Color c);

/* An image of w*h RGBA pixels (same byte order as Color) with rounded
 * corners, blended over whatever is already drawn. */
void gfx_image_round(int x, int y, int w, int h, int r, const uint32_t *px);

/* Glyphs for the controller's buttons, from the console's NintendoExt font
 * (empty strings when that font is missing, e.g. in the desktop preview). */
extern const char *GLYPH_A, *GLYPH_B, *GLYPH_X, *GLYPH_Y, *GLYPH_L, *GLYPH_R,
                  *GLYPH_PLUS, *GLYPH_MINUS;
bool gfx_has_glyphs(void);

/* Text with its top-left at (x, y); returns the advance width. */
int  gfx_text(int x, int y, int px, Color c, const char *utf8);
int  gfx_text_width(int px, const char *utf8);
/* Text cut with an ellipsis to fit max_w. */
void gfx_text_fit(int x, int y, int px, Color c, const char *utf8, int max_w);

#endif
