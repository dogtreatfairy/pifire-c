#pragma once
/* Tiny RGB565 framebuffer renderer for SPI TFT panels. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	int w, h;
	uint16_t *px;    /* w*h, big-endian RGB565 as the ILI9341 expects (byte-swapped on little-endian hosts) */
} pf_gfx;

#define PF_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define PF_C_BG      PF_RGB(0x11, 0x12, 0x14)
#define PF_C_CARD    PF_RGB(0x26, 0x27, 0x2D)
#define PF_C_TEXT    PF_RGB(0xF4, 0xF4, 0xF5)
#define PF_C_MUTED   PF_RGB(0x9B, 0x9C, 0xA3)
#define PF_C_ACCENT  PF_RGB(0xFF, 0x8A, 0x1F)
#define PF_C_OK      PF_RGB(0x4C, 0xD9, 0x64)
#define PF_C_WARN    PF_RGB(0xFF, 0xCC, 0x00)
#define PF_C_DANGER  PF_RGB(0xFF, 0x45, 0x3A)

int  pf_gfx_init(pf_gfx *g, int w, int h);
void pf_gfx_free(pf_gfx *g);
void pf_gfx_clear(pf_gfx *g, uint16_t c);
void pf_gfx_rect(pf_gfx *g, int x, int y, int w, int h, uint16_t c);
void pf_gfx_frame(pf_gfx *g, int x, int y, int w, int h, uint16_t c);
/* scale 1 = 8x8, 2 = 16x16, ... Returns the width drawn. */
int  pf_gfx_text(pf_gfx *g, int x, int y, const char *s, int scale, uint16_t fg);
int  pf_gfx_text_width(const char *s, int scale);
void pf_gfx_text_center(pf_gfx *g, int cx, int y, const char *s, int scale, uint16_t fg);
void pf_gfx_text_right(pf_gfx *g, int rx, int y, const char *s, int scale, uint16_t fg);
/* horizontal bar 0..1 */
void pf_gfx_bar(pf_gfx *g, int x, int y, int w, int h, double frac, uint16_t fg, uint16_t bg);
/* Write as binary PPM (for tests / previews). */
int  pf_gfx_write_ppm(const pf_gfx *g, const char *path);
