/* QR encoder: byte mode, level L, versions 1-5 (single ECC block, so no interleaving).
 * Follows ISO/IEC 18004: build the bit stream, append Reed-Solomon parity over GF(256), lay the
 * function patterns and the zig-zag data path, then pick the mask with the lowest penalty. */
#include "display/qr.h"
#include <string.h>

/* per version at level L: total codewords, data codewords, ECC codewords (all single-block) */
static const struct { int total, data, ecc; } CAP[6] = {
	{ 0, 0, 0 }, { 26, 19, 7 }, { 44, 34, 10 }, { 70, 55, 15 }, { 100, 80, 20 }, { 134, 108, 26 },
};

/* ---------------- GF(256), primitive polynomial 0x11D ---------------- */

static uint8_t gf_exp[512], gf_log[256];

static void gf_init(void)
{
	if (gf_exp[0]) return;
	int x = 1;
	for (int i = 0; i < 255; i++) { gf_exp[i] = (uint8_t)x; gf_log[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
	for (int i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
}

static uint8_t gf_mul(uint8_t a, uint8_t b) { return (a && b) ? gf_exp[gf_log[a] + gf_log[b]] : 0; }

/* generator polynomial of degree n, highest coefficient first (g[0] == 1) */
static void rs_gen(int n, uint8_t *g)
{
	uint8_t tmp[64];
	memset(g, 0, 64);
	g[0] = 1;
	int len = 1;
	for (int i = 0; i < n; i++) {
		memset(tmp, 0, sizeof tmp);
		for (int j = 0; j < len; j++) {           /* multiply by (x + a^i) */
			tmp[j] ^= g[j];                       /* x term */
			tmp[j + 1] ^= gf_mul(g[j], gf_exp[i]);
		}
		len++;
		memcpy(g, tmp, (size_t)len);
	}
}

static void rs_ecc(const uint8_t *data, int dlen, int n, uint8_t *ecc)
{
	uint8_t g[64];
	rs_gen(n, g);
	memset(ecc, 0, (size_t)n);
	for (int i = 0; i < dlen; i++) {
		uint8_t factor = (uint8_t)(data[i] ^ ecc[0]);
		memmove(ecc, ecc + 1, (size_t)n - 1);
		ecc[n - 1] = 0;
		for (int j = 0; j < n; j++) ecc[j] ^= gf_mul(g[j + 1], factor);
	}
}

/* ---------------- module placement ---------------- */

typedef struct { pf_qr *q; uint8_t fn[PF_QR_MAX_SIZE][PF_QR_MAX_SIZE]; } grid;

static void put(grid *gr, int x, int y, int dark)
{
	if (x < 0 || y < 0 || x >= gr->q->size || y >= gr->q->size) return;
	gr->q->m[y][x] = (uint8_t)(dark ? 1 : 0);
	gr->fn[y][x] = 1;
}

static void finder(grid *gr, int ox, int oy)
{
	for (int dy = -1; dy <= 7; dy++)
		for (int dx = -1; dx <= 7; dx++) {
			int out = dx < 0 || dx > 6 || dy < 0 || dy > 6;
			int ring = dx == 0 || dx == 6 || dy == 0 || dy == 6;
			int core = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
			put(gr, ox + dx, oy + dy, !out && (ring || core));
		}
}

static void alignment(grid *gr, int cx, int cy)
{
	for (int dy = -2; dy <= 2; dy++)
		for (int dx = -2; dx <= 2; dx++) {
			int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
			put(gr, cx + dx, cy + dy, ax == 2 || ay == 2 || (dx == 0 && dy == 0));
		}
}

/* 15-bit format information for level L and the given mask */
static unsigned format_bits(int mask)
{
	unsigned data = (unsigned)(0x01 << 3 | mask);   /* level L = 01 */
	unsigned rem = data;
	for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	return ((data << 10) | (rem & 0x3FF)) ^ 0x5412;
}

static void draw_format(grid *gr, int mask)
{
	unsigned bits = format_bits(mask);
	int size = gr->q->size;
#define BIT(i) ((int)((bits >> (i)) & 1))
	for (int i = 0; i <= 5; i++) put(gr, 8, i, BIT(i));
	put(gr, 8, 7, BIT(6));
	put(gr, 8, 8, BIT(7));
	put(gr, 7, 8, BIT(8));
	for (int i = 9; i < 15; i++) put(gr, 14 - i, 8, BIT(i));
	for (int i = 0; i < 8; i++) put(gr, size - 1 - i, 8, BIT(i));
	for (int i = 8; i < 15; i++) put(gr, 8, size - 15 + i, BIT(i));
	put(gr, 8, size - 8, 1);   /* always-dark module */
#undef BIT
}

static void draw_function_patterns(grid *gr, int version)
{
	int size = gr->q->size;
	finder(gr, 0, 0);
	finder(gr, size - 7, 0);
	finder(gr, 0, size - 7);
	for (int i = 8; i < size - 8; i++) { put(gr, 6, i, !(i % 2)); put(gr, i, 6, !(i % 2)); }
	if (version >= 2) alignment(gr, size - 7, size - 7);
	draw_format(gr, 0);   /* reserves the area; rewritten once the mask is chosen */
}

static int mask_at(int mask, int x, int y)
{
	switch (mask) {
	case 0: return (x + y) % 2 == 0;
	case 1: return y % 2 == 0;
	case 2: return x % 3 == 0;
	case 3: return (x + y) % 3 == 0;
	case 4: return (y / 2 + x / 3) % 2 == 0;
	case 5: return (x * y) % 2 + (x * y) % 3 == 0;
	case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
	default: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
	}
}

/* ---------------- penalty (ISO 18004 section 8.8.2) ---------------- */

static int penalty(const pf_qr *q)
{
	int size = q->size, score = 0, dark = 0;
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++) dark += q->m[y][x];
	/* rule 1: runs of five or more */
	for (int d = 0; d < 2; d++)
		for (int a = 0; a < size; a++) {
			int run = 1;
			for (int b = 1; b < size; b++) {
				int cur = d ? q->m[b][a] : q->m[a][b];
				int prev = d ? q->m[b - 1][a] : q->m[a][b - 1];
				if (cur == prev) { run++; if (run == 5) score += 3; else if (run > 5) score += 1; }
				else run = 1;
			}
		}
	/* rule 2: 2x2 blocks of one colour */
	for (int y = 0; y + 1 < size; y++)
		for (int x = 0; x + 1 < size; x++) {
			int v = q->m[y][x];
			if (v == q->m[y][x + 1] && v == q->m[y + 1][x] && v == q->m[y + 1][x + 1]) score += 3;
		}
	/* rule 3: the 1:1:3:1:1 pattern with four light modules on one side, as an 11-module window */
	for (int d = 0; d < 2; d++)
		for (int a = 0; a < size; a++)
			for (int b = 0; b + 10 < size; b++) {
#define M(i) (d ? q->m[b + (i)][a] : q->m[a][b + (i)])
				if (!M(1) && M(4) && !M(5) && M(6) && !M(9) &&
				    ((M(0) && M(2) && M(3) && !M(7) && !M(8) && !M(10)) ||
				     (!M(0) && !M(2) && !M(3) && M(7) && M(8) && M(10))))
					score += 40;
				if (M(10)) b++;   /* the window cannot start at b+1 when b+10 is dark */
#undef M
			}
	/* rule 4: every full 5 % of deviation from an even balance of dark and light */
	int total = size * size;
	int dev = 20 * dark - 10 * total;
	if (dev < 0) dev = -dev;
	score += (dev / total) * 10;
	return score;
}

/* ---------------- encode ---------------- */

bool pf_qr_encode(const char *text, pf_qr *out)
{
	gf_init();
	size_t len = text ? strlen(text) : 0;
	int version = 0;
	for (int v = 1; v <= 5; v++)
		if ((int)len + 2 <= CAP[v].data) { version = v; break; }   /* 4 bits mode + 8 bits count = 12 bits */
	if (!version) return false;

	/* bit stream: mode, character count, payload, terminator, pad */
	uint8_t code[134];
	memset(code, 0, sizeof code);
	int nbits = 0;
#define PUSH(val, n) do { for (int _i = (n) - 1; _i >= 0; _i--) { if (((val) >> _i) & 1) code[nbits >> 3] |= (uint8_t)(0x80 >> (nbits & 7)); nbits++; } } while (0)
	PUSH(4, 4);
	PUSH((int)len, 8);
	for (size_t i = 0; i < len; i++) PUSH((unsigned char)text[i], 8);
	int datacw = CAP[version].data;
	int termin = datacw * 8 - nbits;
	PUSH(0, termin < 4 ? termin : 4);
	while (nbits % 8) PUSH(0, 1);
#undef PUSH
	for (int i = nbits / 8, alt = 0; i < datacw; i++, alt ^= 1) code[i] = alt ? 0x11 : 0xEC;
	rs_ecc(code, datacw, CAP[version].ecc, code + datacw);

	/* function patterns */
	grid gr;
	memset(&gr, 0, sizeof gr);
	memset(out, 0, sizeof *out);
	gr.q = out;
	out->size = 17 + 4 * version;
	draw_function_patterns(&gr, version);

	/* data path: two columns at a time, upwards then downwards, skipping the timing column */
	int total_bits = CAP[version].total * 8, bit = 0, size = out->size;
	for (int right = size - 1; right >= 1; right -= 2) {
		if (right == 6) right = 5;
		for (int vert = 0; vert < size; vert++)
			for (int j = 0; j < 2; j++) {
				int x = right - j;
				int upward = ((right + 1) & 2) == 0;
				int y = upward ? size - 1 - vert : vert;
				if (gr.fn[y][x] || bit >= total_bits) continue;
				out->m[y][x] = (uint8_t)((code[bit >> 3] >> (7 - (bit & 7))) & 1);
				bit++;
			}
	}

	/* choose the mask with the lowest penalty */
	int best = 0, best_score = 0;
	pf_qr trial;
	for (int mask = 0; mask < 8; mask++) {
		trial = *out;
		grid tg;
		memcpy(tg.fn, gr.fn, sizeof tg.fn);
		tg.q = &trial;
		for (int y = 0; y < size; y++)
			for (int x = 0; x < size; x++)
				if (!gr.fn[y][x] && mask_at(mask, x, y)) trial.m[y][x] ^= 1;
		draw_format(&tg, mask);
		int sc = penalty(&trial);
		if (mask == 0 || sc < best_score) { best_score = sc; best = mask; }
	}
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++)
			if (!gr.fn[y][x] && mask_at(best, x, y)) out->m[y][x] ^= 1;
	draw_format(&gr, best);
	return true;
}
