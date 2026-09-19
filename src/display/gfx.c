#include "display/gfx.h"
#include "display/font8x8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

int pf_gfx_init(pf_gfx *g, int w, int h)
{
	g->w = w; g->h = h;
	g->px = calloc((size_t)w * h, sizeof(uint16_t));
	return g->px ? 0 : -1;
}

void pf_gfx_free(pf_gfx *g) { free(g->px); g->px = NULL; }

void pf_gfx_clear(pf_gfx *g, uint16_t c)
{
	uint16_t v = swap16(c);
	for (size_t i = 0, n = (size_t)g->w * g->h; i < n; i++) g->px[i] = v;
}

void pf_gfx_rect(pf_gfx *g, int x, int y, int w, int h, uint16_t c)
{
	uint16_t v = swap16(c);
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > g->w ? g->w : x + w, y1 = y + h > g->h ? g->h : y + h;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++) g->px[yy * g->w + xx] = v;
}

void pf_gfx_frame(pf_gfx *g, int x, int y, int w, int h, uint16_t c)
{
	pf_gfx_rect(g, x, y, w, 1, c); pf_gfx_rect(g, x, y + h - 1, w, 1, c);
	pf_gfx_rect(g, x, y, 1, h, c); pf_gfx_rect(g, x + w - 1, y, 1, h, c);
}

int pf_gfx_text_width(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

int pf_gfx_text(pf_gfx *g, int x, int y, const char *s, int scale, uint16_t fg)
{
	uint16_t v = swap16(fg);
	int cx = x;
	for (; *s; s++) {
		unsigned ch = (unsigned char)*s;
		if (ch == 0xC2 || ch == 0xC3) continue;          /* skip UTF-8 lead bytes (degree sign etc.) */
		if (ch == 0xB0) ch = 0x7F;                       /* degree sign -> special glyph below */
		const uint8_t *glyph = (ch >= 0x20 && ch < 0x7F) ? font8x8[ch - 0x20] : NULL;
		static const uint8_t degree[8] = { 0x06, 0x09, 0x09, 0x06, 0x00, 0x00, 0x00, 0x00 };
		if (ch == 0x7F) glyph = degree;
		if (!glyph) glyph = font8x8[0];
		for (int r = 0; r < 8; r++) {
			uint8_t bits = glyph[r];
			for (int c = 0; c < 8; c++) {
				if (!(bits & (1 << c))) continue;
				for (int sy = 0; sy < scale; sy++) {
					int py = y + r * scale + sy;
					if (py < 0 || py >= g->h) continue;
					for (int sx = 0; sx < scale; sx++) {
						int px = cx + c * scale + sx;
						if (px >= 0 && px < g->w) g->px[py * g->w + px] = v;
					}
				}
			}
		}
		cx += 8 * scale;
	}
	return cx - x;
}

void pf_gfx_text_center(pf_gfx *g, int cx, int y, const char *s, int scale, uint16_t fg)
{
	pf_gfx_text(g, cx - pf_gfx_text_width(s, scale) / 2, y, s, scale, fg);
}

void pf_gfx_text_right(pf_gfx *g, int rx, int y, const char *s, int scale, uint16_t fg)
{
	pf_gfx_text(g, rx - pf_gfx_text_width(s, scale), y, s, scale, fg);
}

void pf_gfx_bar(pf_gfx *g, int x, int y, int w, int h, double frac, uint16_t fg, uint16_t bg)
{
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	pf_gfx_rect(g, x, y, w, h, bg);
	pf_gfx_rect(g, x, y, (int)(w * frac + 0.5), h, fg);
}

int pf_gfx_write_ppm(const pf_gfx *g, const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	fprintf(f, "P6\n%d %d\n255\n", g->w, g->h);
	for (size_t i = 0, n = (size_t)g->w * g->h; i < n; i++) {
		uint16_t v = swap16(g->px[i]);
		unsigned char rgb[3] = { (unsigned char)(((v >> 11) & 0x1F) << 3), (unsigned char)(((v >> 5) & 0x3F) << 2), (unsigned char)((v & 0x1F) << 3) };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	return 0;
}
