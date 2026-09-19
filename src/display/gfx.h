#pragma once
/* Tiny RGB565 framebuffer renderer for SPI TFT panels. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Colour tokens, mirrored from web/style.css so the panel and the web app share one look.
 * Both palettes are tuned for a transmissive TFT read outdoors: full-contrast text, no dim greys. */
typedef struct {
	uint16_t bg, card, card2, line, text, muted, accent, accent_text, ok, warn, danger, info;
	bool light;
} pf_gfx_theme;

typedef struct {
	int w, h;
	uint16_t *px;    /* w*h, big-endian RGB565 as the ILI9341 expects (byte-swapped on little-endian hosts) */
	pf_gfx_theme th;
} pf_gfx;

#define PF_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

int  pf_gfx_init(pf_gfx *g, int w, int h);
void pf_gfx_free(pf_gfx *g);
/* "dark" (default) or "light" (high-contrast, best in direct sun). */
void pf_gfx_set_theme(pf_gfx *g, const char *name);
void pf_gfx_clear(pf_gfx *g, uint16_t c);
void pf_gfx_rect(pf_gfx *g, int x, int y, int w, int h, uint16_t c);
/* filled rectangle with rounded corners of radius r */
void pf_gfx_rrect(pf_gfx *g, int x, int y, int w, int h, int r, uint16_t c);
void pf_gfx_frame(pf_gfx *g, int x, int y, int w, int h, uint16_t c);
void pf_gfx_disc(pf_gfx *g, int cx, int cy, int r, uint16_t c);
/* anti-aliased ring segment: radii [r_in, r_out], angles in degrees clockwise from 3 o'clock */
void pf_gfx_arc(pf_gfx *g, int cx, int cy, int r_in, int r_out, double a0, double a1, uint16_t c);
/* scale 1 = 8x8, 2 = 16x16, ... Returns the width drawn. */
int  pf_gfx_text(pf_gfx *g, int x, int y, const char *s, int scale, uint16_t fg);
int  pf_gfx_text_width(const char *s, int scale);
void pf_gfx_text_center(pf_gfx *g, int cx, int y, const char *s, int scale, uint16_t fg);
void pf_gfx_text_right(pf_gfx *g, int rx, int y, const char *s, int scale, uint16_t fg);
/* horizontal bar 0..1 */
void pf_gfx_bar(pf_gfx *g, int x, int y, int w, int h, double frac, uint16_t fg, uint16_t bg);
/* Write as binary PPM (for tests / previews). */
int  pf_gfx_write_ppm(const pf_gfx *g, const char *path);
