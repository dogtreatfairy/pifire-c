#include "display/gfx.h"
#include "display/font8x8.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

int pf_gfx_init(pf_gfx *g, int w, int h)
{
	g->w = w; g->h = h;
	g->px = calloc((size_t)w * h, sizeof(uint16_t));
	pf_gfx_set_theme(g, "dark");
	return g->px ? 0 : -1;
}

void pf_gfx_set_theme(pf_gfx *g, const char *name)
{
	bool light = name && !strcmp(name, "light");
	pf_gfx_theme t;
	if (light) {
		/* web light tokens, with text pushed to full black for sunlight */
		t = (pf_gfx_theme){ .bg = PF_RGB(0xF3, 0xF3, 0xF5), .card = PF_RGB(0xFF, 0xFF, 0xFF), .card2 = PF_RGB(0xE4, 0xE6, 0xEC), .line = PF_RGB(0xC8, 0xCA, 0xD2),
		                    .text = PF_RGB(0x00, 0x00, 0x00), .muted = PF_RGB(0x3C, 0x3E, 0x46), .accent = PF_RGB(0xE8, 0x6E, 0x00), .accent_text = PF_RGB(0xFF, 0xFF, 0xFF),
		                    .ok = PF_RGB(0x0E, 0x8A, 0x2E), .warn = PF_RGB(0xB0, 0x7A, 0x00), .danger = PF_RGB(0xD3, 0x1F, 0x14), .info = PF_RGB(0x00, 0x6C, 0xB8), .light = true };
	} else {
		/* web dark tokens; background pulled to true black and "muted" lifted so it survives glare */
		t = (pf_gfx_theme){ .bg = PF_RGB(0x00, 0x00, 0x00), .card = PF_RGB(0x1B, 0x1C, 0x20), .card2 = PF_RGB(0x2A, 0x2B, 0x32), .line = PF_RGB(0x3A, 0x3B, 0x44),
		                    .text = PF_RGB(0xFF, 0xFF, 0xFF), .muted = PF_RGB(0xC4, 0xC5, 0xCC), .accent = PF_RGB(0xFF, 0x8A, 0x1F), .accent_text = PF_RGB(0x1A, 0x10, 0x02),
		                    .ok = PF_RGB(0x4C, 0xD9, 0x64), .warn = PF_RGB(0xFF, 0xCC, 0x00), .danger = PF_RGB(0xFF, 0x45, 0x3A), .info = PF_RGB(0x5A, 0xC8, 0xFA), .light = false };
	}
	g->th = t;
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

static inline void blend_px(pf_gfx *g, int x, int y, uint16_t c, double cov)
{
	if (x < 0 || y < 0 || x >= g->w || y >= g->h || cov <= 0) return;
	uint16_t *p = &g->px[y * g->w + x];
	if (cov >= 1) { *p = swap16(c); return; }
	uint16_t o = swap16(*p);
	int r0 = (o >> 11) & 0x1F, g0 = (o >> 5) & 0x3F, b0 = o & 0x1F;
	int r1 = (c >> 11) & 0x1F, g1 = (c >> 5) & 0x3F, b1 = c & 0x1F;
	int r = (int)(r0 + (r1 - r0) * cov + 0.5), gg = (int)(g0 + (g1 - g0) * cov + 0.5), b = (int)(b0 + (b1 - b0) * cov + 0.5);
	*p = swap16((uint16_t)((r << 11) | (gg << 5) | b));
}

static inline double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

void pf_gfx_disc(pf_gfx *g, int cx, int cy, int r, uint16_t c)
{
	for (int y = cy - r - 1; y <= cy + r + 1; y++)
		for (int x = cx - r - 1; x <= cx + r + 1; x++) {
			double d = sqrt((double)(x - cx) * (x - cx) + (double)(y - cy) * (y - cy));
			blend_px(g, x, y, c, clamp01(r + 0.5 - d));
		}
}

void pf_gfx_rrect(pf_gfx *g, int x, int y, int w, int h, int r, uint16_t c)
{
	if (r <= 0) { pf_gfx_rect(g, x, y, w, h, c); return; }
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	pf_gfx_rect(g, x + r, y, w - 2 * r, h, c);
	pf_gfx_rect(g, x, y + r, r, h - 2 * r, c);
	pf_gfx_rect(g, x + w - r, y + r, r, h - 2 * r, c);
	int cs[4][2] = { { x + r, y + r }, { x + w - r - 1, y + r }, { x + r, y + h - r - 1 }, { x + w - r - 1, y + h - r - 1 } };
	for (int k = 0; k < 4; k++) {
		int cx = cs[k][0], cy = cs[k][1];
		int sx = k & 1 ? 1 : -1, sy = k & 2 ? 1 : -1;
		for (int dy = 0; dy <= r; dy++)
			for (int dx = 0; dx <= r; dx++) {
				double d = sqrt((double)dx * dx + (double)dy * dy);
				blend_px(g, cx + sx * dx, cy + sy * dy, c, clamp01(r + 0.5 - d));
			}
	}
}

void pf_gfx_arc(pf_gfx *g, int cx, int cy, int r_in, int r_out, double a0, double a1, uint16_t c)
{
	if (a1 <= a0) return;
	double span = a1 - a0;
	if (span > 360) span = 360;
	for (int y = cy - r_out - 1; y <= cy + r_out + 1; y++)
		for (int x = cx - r_out - 1; x <= cx + r_out + 1; x++) {
			double dx = x - cx, dy = y - cy, d = sqrt(dx * dx + dy * dy);
			double cov = clamp01(fmin(d - (r_in - 0.5), (r_out + 0.5) - d));
			if (cov <= 0) continue;
			double ang = atan2(dy, dx) * 180.0 / M_PI - a0;
			ang = fmod(fmod(ang, 360.0) + 360.0, 360.0);  /* 0..360 from the arc start */
			if (span < 360) {
				/* signed distance (pixels along the circle) to the nearest arc end: positive inside the span */
				double deg = ang <= span ? fmin(ang, span - ang) : -fmin(ang - span, 360.0 - ang);
				cov *= clamp01(deg * d * M_PI / 180.0 + 0.5);
			}
			blend_px(g, x, y, c, cov);
		}
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
