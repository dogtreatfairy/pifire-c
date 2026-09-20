#include "display/gfx.h"
#include "core/embedded.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

int pf_gfx_init(pf_gfx *g, int w, int h)
{
	g->w = w; g->h = h; g->vw = w; g->vh = h;
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
		                    .text = PF_RGB(0x00, 0x00, 0x00), .muted = PF_RGB(0x4A, 0x4C, 0x55), .accent = PF_RGB(0xE8, 0x6E, 0x00), .accent_text = PF_RGB(0xFF, 0xFF, 0xFF),
		                    .ok = PF_RGB(0x0E, 0x8A, 0x2E), .warn = PF_RGB(0xB0, 0x7A, 0x00), .danger = PF_RGB(0xD3, 0x1F, 0x14), .info = PF_RGB(0x00, 0x6C, 0xB8),
		                    .fan = PF_RGB(0x0E, 0x9A, 0x30), .auger = PF_RGB(0x00, 0x6C, 0xE8), .igniter = PF_RGB(0xF0, 0x70, 0x00), .light = true };
	} else {
		/* web dark tokens; background pulled to true black and "muted" lifted so it survives glare */
		/* saturated, bright fills read in sunlight; "muted" stays light grey rather than dim */
		t = (pf_gfx_theme){ .bg = PF_RGB(0x00, 0x00, 0x00), .card = PF_RGB(0x1A, 0x1B, 0x20), .card2 = PF_RGB(0x2A, 0x2B, 0x32), .line = PF_RGB(0x3C, 0x3D, 0x46),
		                    .text = PF_RGB(0xFF, 0xFF, 0xFF), .muted = PF_RGB(0xC4, 0xC5, 0xCC), .accent = PF_RGB(0xFF, 0x95, 0x00), .accent_text = PF_RGB(0x14, 0x0C, 0x00),
		                    .ok = PF_RGB(0x30, 0xE0, 0x58), .warn = PF_RGB(0xFF, 0xD6, 0x0A), .danger = PF_RGB(0xFF, 0x3B, 0x30), .info = PF_RGB(0x40, 0xB0, 0xFF),
		                    .fan = PF_RGB(0x30, 0xE0, 0x58), .auger = PF_RGB(0x3D, 0xA5, 0xFF), .igniter = PF_RGB(0xFF, 0x8A, 0x00), .light = false };
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

void pf_gfx_bar(pf_gfx *g, int x, int y, int w, int h, double frac, uint16_t fg, uint16_t bg)
{
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	pf_gfx_rrect(g, x, y, w, h, h / 2, bg);
	int fw = (int)(w * frac + 0.5);
	if (fw > 0) pf_gfx_rrect(g, x, y, fw < h ? h : fw, h, h / 2, fg);
}

/* ---------------- TrueType text ---------------- */

typedef struct {
	stbtt_fontinfo info;
	bool ok;
	int ascent, descent, linegap;
} font_t;

static font_t g_fonts[2];
static pthread_once_t g_fonts_once = PTHREAD_ONCE_INIT;

/* small glyph cache: rasterised coverage bitmaps keyed by (font, px, codepoint) */
typedef struct {
	int font, px; unsigned cp;
	unsigned char *bmp; int w, h, xoff, yoff; int advance;
	unsigned age;
} glyph_t;
#define GLYPH_CACHE 384
static glyph_t g_cache[GLYPH_CACHE];
static unsigned g_tick;
static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static void fonts_load(void)
{
	static const char *const names[2] = { "Inter-Regular.ttf", "Inter-SemiBold.ttf" };
	for (int i = 0; i < 2; i++) {
		const pf_embedded_file *f = pf_embedded_share(names[i]);
		if (!f) continue;
		if (!stbtt_InitFont(&g_fonts[i].info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) continue;
		stbtt_GetFontVMetrics(&g_fonts[i].info, &g_fonts[i].ascent, &g_fonts[i].descent, &g_fonts[i].linegap);
		g_fonts[i].ok = true;
	}
}

static font_t *font_get(pf_font f)
{
	pthread_once(&g_fonts_once, fonts_load);
	font_t *ft = &g_fonts[f == PF_FONT_SEMIBOLD ? 1 : 0];
	if (!ft->ok) ft = g_fonts[0].ok ? &g_fonts[0] : g_fonts[1].ok ? &g_fonts[1] : NULL;
	return ft;
}

static unsigned utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	unsigned cp = p[0];
	int n = 1;
	if (cp >= 0xF0 && p[1] && p[2] && p[3]) { cp = ((cp & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63); n = 4; }
	else if (cp >= 0xE0 && p[1] && p[2]) { cp = ((cp & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); n = 3; }
	else if (cp >= 0xC0 && p[1]) { cp = ((cp & 31) << 6) | (p[1] & 63); n = 2; }
	*s += n;
	return cp;
}

/* must be called with g_cache_mu held */
static glyph_t *glyph_get(font_t *ft, int fidx, int px, unsigned cp)
{
	unsigned h = (unsigned)(fidx * 7919 + px * 131 + (int)cp) % GLYPH_CACHE;
	for (unsigned i = 0; i < GLYPH_CACHE; i++) {
		glyph_t *e = &g_cache[(h + i) % GLYPH_CACHE];
		if (e->bmp && e->font == fidx && e->px == px && e->cp == cp) { e->age = ++g_tick; return e; }
		if (!e->bmp) break;
	}
	/* evict the oldest of a small probe window */
	glyph_t *victim = &g_cache[h];
	for (unsigned i = 0; i < 8; i++) { glyph_t *e = &g_cache[(h + i) % GLYPH_CACHE]; if (!e->bmp) { victim = e; break; } if (e->age < victim->age) victim = e; }
	free(victim->bmp);
	memset(victim, 0, sizeof *victim);
	float scale = stbtt_ScaleForPixelHeight(&ft->info, (float)px);
	int gi = stbtt_FindGlyphIndex(&ft->info, (int)cp);
	if (!gi) gi = stbtt_FindGlyphIndex(&ft->info, '?');
	int adv, lsb;
	stbtt_GetGlyphHMetrics(&ft->info, gi, &adv, &lsb);
	victim->bmp = stbtt_GetGlyphBitmap(&ft->info, scale, scale, gi, &victim->w, &victim->h, &victim->xoff, &victim->yoff);
	if (!victim->bmp) { victim->bmp = malloc(1); victim->w = victim->h = 0; }  /* space: keep a valid (empty) entry */
	victim->advance = (int)lroundf(adv * scale);
	victim->font = fidx; victim->px = px; victim->cp = cp; victim->age = ++g_tick;
	return victim;
}

int pf_gfx_ascent(pf_font f, int px)
{
	font_t *ft = font_get(f);
	if (!ft) return px;
	return (int)lroundf(ft->ascent * stbtt_ScaleForPixelHeight(&ft->info, (float)px));
}

int pf_gfx_line_height(pf_font f, int px)
{
	font_t *ft = font_get(f);
	if (!ft) return px;
	float s = stbtt_ScaleForPixelHeight(&ft->info, (float)px);
	return (int)lroundf((ft->ascent - ft->descent) * s);
}

int pf_gfx_text_width(pf_font f, int px, const char *s)
{
	font_t *ft = font_get(f);
	if (!ft) return 0;
	int fidx = ft == &g_fonts[1] ? 1 : 0, w = 0;
	pthread_mutex_lock(&g_cache_mu);
	while (*s) w += glyph_get(ft, fidx, px, utf8_next(&s))->advance;
	pthread_mutex_unlock(&g_cache_mu);
	return w;
}

int pf_gfx_text(pf_gfx *g, pf_font f, int px, int x, int y, const char *s, uint16_t c)
{
	font_t *ft = font_get(f);
	if (!ft) return 0;
	int fidx = ft == &g_fonts[1] ? 1 : 0;
	int base = y + pf_gfx_ascent(f, px), cx = x;
	pthread_mutex_lock(&g_cache_mu);
	while (*s) {
		glyph_t *gl = glyph_get(ft, fidx, px, utf8_next(&s));
		for (int r = 0; r < gl->h; r++) {
			int py = base + gl->yoff + r;
			if (py < 0 || py >= g->h) continue;
			const unsigned char *row = gl->bmp + r * gl->w;
			for (int k = 0; k < gl->w; k++) if (row[k]) blend_px(g, cx + gl->xoff + k, py, c, row[k] / 255.0);
		}
		cx += gl->advance;
	}
	pthread_mutex_unlock(&g_cache_mu);
	return cx - x;
}

void pf_gfx_text_center(pf_gfx *g, pf_font f, int px, int cx, int y, const char *s, uint16_t c)
{
	pf_gfx_text(g, f, px, cx - pf_gfx_text_width(f, px, s) / 2, y, s, c);
}

void pf_gfx_text_right(pf_gfx *g, pf_font f, int px, int rx, int y, const char *s, uint16_t c)
{
	pf_gfx_text(g, f, px, rx - pf_gfx_text_width(f, px, s), y, s, c);
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
