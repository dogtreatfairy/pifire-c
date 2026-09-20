/* TFT screens, industrial-HMI style: a filled mode banner, big filled status tiles for fan / auger /
 * igniter, the pit temperature large with the set point and the error next to it, food probes below.
 * Everything is bold and high-contrast; nothing is decorative. */
#include "display/screens.h"
#include "core/settings.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define R PF_FONT_REGULAR
#define B PF_FONT_SEMIBOLD
#define DEG "\xC2\xB0"

static void fmt_temp(char *out, size_t n, const cJSON *v)
{
	if (cJSON_IsNumber(v)) snprintf(out, n, "%.0f", v->valuedouble); else snprintf(out, n, "--");
}

static void fmt_clock(char *out, size_t n, double secs)
{
	int s = (int)fmax(0, secs + 0.5);
	if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
	else snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void upper(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }

static uint16_t mode_fill(const pf_gfx *g, const char *mode)
{
	if (!strcmp(mode, "Hold")) return g->th.ok;
	if (!strcmp(mode, "Smoke") || !strcmp(mode, "Startup") || !strcmp(mode, "Reignite") || !strcmp(mode, "Prime")) return g->th.accent;
	if (!strcmp(mode, "Error")) return g->th.danger;
	if (!strcmp(mode, "Shutdown")) return g->th.info;
	if (!strcmp(mode, "Manual")) return g->th.warn;
	return g->th.card2;   /* Stop, Monitor */
}
static uint16_t on_fill_text(const pf_gfx *g, uint16_t fill)
{
	return (fill == g->th.danger || fill == g->th.info || fill == g->th.card2) ? g->th.text : g->th.accent_text;
}

static bool is_timed(const char *mode)
{
	return !strcmp(mode, "Startup") || !strcmp(mode, "Reignite") || !strcmp(mode, "Shutdown") || !strcmp(mode, "Prime");
}

/* --------------------------------------------------------------- menu (same sets as the original) */

int pf_menu_build(const cJSON *status, pf_menu_item *out, int max)
{
	const char *mode = pf_json_str((cJSON *)status, "mode", "Stop");
	int n = 0;
#define ADD(i, l) do { if (n < max) { out[n].id = (i); snprintf(out[n].label, sizeof out[n].label, "%s", (l)); n++; } } while (0)
	if (!strcmp(mode, "Error")) {
		ADD(PF_MI_CLEAR, "Clear error & stop");
	} else if (!strcmp(mode, "Stop") || !strcmp(mode, "Monitor") || !strcmp(mode, "Prime")) {
		ADD(PF_MI_STARTUP, "Startup");
		ADD(PF_MI_HOLD, "Hold at...");
		ADD(PF_MI_PRIME, "Prime 10 g");
		if (strcmp(mode, "Monitor")) ADD(PF_MI_MONITOR, "Monitor"); else ADD(PF_MI_STOP, "Stop");
	} else if (!strcmp(mode, "Shutdown")) {
		ADD(PF_MI_STOP, "Stop");
	} else {  /* Startup, Reignite, Smoke, Hold, Manual */
		ADD(PF_MI_HOLD, !strcmp(mode, "Hold") ? "Change target" : "Hold at...");
		if (strcmp(mode, "Smoke")) ADD(PF_MI_SMOKE, "Smoke");
		if (!strcmp(mode, "Smoke")) ADD(PF_MI_SMOKE_PLUS, pf_json_bool((cJSON *)status, "s_plus", false) ? "Smoke+ off" : "Smoke+ on");
		ADD(PF_MI_SHUTDOWN, "Shutdown");
		ADD(PF_MI_STOP, "Stop");
	}
	ADD(PF_MI_BACK, "Back");
#undef ADD
	return n;
}

/* --------------------------------------------------------------- pieces */

static void draw_banner(pf_gfx *g, const cJSON *s, const char *mode)
{
	int W = g->vw;
	uint16_t fill = mode_fill(g, mode), tc = on_fill_text(g, fill);
	pf_gfx_rect(g, 0, 0, g->w, 34, fill);
	char up[16];
	snprintf(up, sizeof up, "%.12s", mode);
	upper(up);
	pf_gfx_text(g, B, 24, 10, 3, up, tc);
	/* right: countdown while a mode is timed, else how long the grill has been running */
	char clk[16] = "";
	double remaining = pf_json_num((cJSON *)s, "timers.mode_remaining", 0), cook = pf_json_num((cJSON *)s, "cook_elapsed", 0);
	if (is_timed(mode)) {
		bool waiting = (!strcmp(mode, "Startup") || !strcmp(mode, "Reignite")) && pf_json_bool((cJSON *)s, "coldstart.active", false) && !pf_json_bool((cJSON *)s, "coldstart.reached", false) && remaining <= 0;
		fmt_clock(clk, sizeof clk, waiting ? pf_json_num((cJSON *)s, "coldstart.remaining", 0) : remaining);
	} else if (cook > 0) fmt_clock(clk, sizeof clk, cook);
	if (clk[0]) pf_gfx_text_right(g, B, 22, W - 10, 4, clk, tc);
}

/* three filled tiles: on = bright fill with dark bold text, off = dark tile with grey text */
static void draw_tiles(pf_gfx *g, const cJSON *s, int y, int h, int px)
{
	static const char *const names[3] = { "FAN", "AUGER", "IGN" };
	static const char *const keys[3] = { "outputs.fan", "outputs.auger", "outputs.igniter" };
	int W = g->vw, gap = 5, w = (W - 12 - 2 * gap) / 3;
	for (int k = 0; k < 3; k++) {
		bool on = pf_json_bool((cJSON *)s, keys[k], false);
		int x = 6 + k * (w + gap);
		uint16_t fill = on ? (k == 0 ? g->th.fan : k == 1 ? g->th.auger : g->th.igniter) : g->th.card2;
		pf_gfx_rrect(g, x, y, w, h, 6, fill);
		char label[16];
		snprintf(label, sizeof label, "%s", names[k]);
		if (k == 0 && on && pf_set_bool("platform.dc_fan", false)) snprintf(label, sizeof label, "FAN %d%%", (int)pf_json_num((cJSON *)s, "outputs.fan_pct", 100) % 1000);
		int tw = pf_gfx_text_width(B, px, label);
		if (tw > w - 8) { px -= 4; tw = pf_gfx_text_width(B, px, label); }
		pf_gfx_text(g, B, px, x + (w - tw) / 2, y + (h - pf_gfx_line_height(B, px)) / 2, label, on ? g->th.accent_text : g->th.muted);
	}
}

/* right-hand data block: set point, error, and one status line */
static void draw_datablock(pf_gfx *g, const cJSON *s, const cJSON *primary, const char *mode, const char *units, int x, int y, int w, bool compact)
{
	int p1 = compact ? 22 : 26, p2 = compact ? 18 : 22, p3 = compact ? 14 : 16, l1 = p1 + 4, l2 = p2 + 4, l3 = p3 + 2;
	bool valid = primary && cJSON_IsNumber(cJSON_GetObjectItem((cJSON *)primary, "temp"));
	double pit = valid ? cJSON_GetObjectItem((cJSON *)primary, "temp")->valuedouble : 0;
	double sp = pf_json_num((cJSON *)s, "setpoint", 0);
	bool hold_like = !strcmp(mode, "Hold") || (!strcmp(mode, "Startup") && !strcmp(pf_json_str((cJSON *)s, "next_mode", ""), "Hold")) || !strcmp(mode, "Reignite");
	char line[32];
	int ly = y;
	if (hold_like && sp > 0) {
		pf_gfx_text(g, B, p3, x, ly, "SET", g->th.muted); ly += l3;
		snprintf(line, sizeof line, "%.0f" DEG, sp);
		pf_gfx_text(g, B, p1, x, ly, line, g->th.accent);
		ly += l1;
		if (valid && !strcmp(mode, "Hold")) {
			double e = pit - sp, tight = units[0] == 'C' ? 4 : 7, wide = units[0] == 'C' ? 8 : 15;
			uint16_t c = fabs(e) <= tight ? g->th.ok : fabs(e) <= wide ? g->th.accent : e < 0 ? g->th.info : g->th.danger;
			snprintf(line, sizeof line, "%+.0f" DEG, e);
			pf_gfx_text(g, B, p2, x, ly, line, c);
			ly += l2;
		}
	} else if (!strcmp(mode, "Smoke")) {
		snprintf(line, sizeof line, "P-MODE %d", pf_set_int("cycle_data.PMode", 2));
		pf_gfx_text(g, B, p2, x, ly, line, g->th.accent); ly += l2;
		if (pf_json_bool((cJSON *)s, "s_plus", false)) { pf_gfx_text(g, B, p2, x, ly, "SMOKE+", g->th.ok); ly += l2; }
	} else if (!strcmp(mode, "Startup") || !strcmp(mode, "Reignite")) {
		pf_gfx_text(g, B, p1, x, ly, "IGNITING", g->th.accent); ly += l1;
		double exit_t = pf_json_num((cJSON *)s, "timers.startup_exit_temp", 0);
		if (exit_t > 0) { snprintf(line, sizeof line, "EXIT %.0f" DEG, exit_t); pf_gfx_text(g, R, p3, x, ly, line, g->th.muted); ly += l3 + 2; }
	} else if (!strcmp(mode, "Shutdown")) {
		pf_gfx_text(g, B, p1, x, ly, "COOLING", g->th.info); ly += l1;
	} else if (!strcmp(mode, "Error")) {
		snprintf(line, sizeof line, "%.10s", pf_json_str((cJSON *)s, "safety.error_code", "ERROR"));
		pf_gfx_text(g, B, p2, x, ly, line, g->th.danger); ly += l2;
	} else if (!strcmp(mode, "Monitor")) {
		pf_gfx_text(g, B, p1, x, ly, "MONITOR", g->th.muted); ly += l1;
	} else if (!strcmp(mode, "Manual")) {
		pf_gfx_text(g, B, p1, x, ly, "MANUAL", g->th.warn); ly += l1;
	} else {
		pf_gfx_text(g, B, p1, x, ly, "READY", g->th.muted); ly += l1;
	}
	int hop = (int)pf_json_num((cJSON *)s, "hopper_pct", -1);
	if (hop >= 0) {
		snprintf(line, sizeof line, "HOP %d%%", hop % 1000);
		pf_gfx_text(g, B, p3, x, ly, line, hop <= 25 ? g->th.danger : g->th.muted);
		ly += l3;
		pf_gfx_bar(g, x, ly + 2, w, 6, hop / 100.0, hop <= 25 ? g->th.danger : g->th.ok, g->th.card2);
	}
	if (pf_json_bool((cJSON *)s, "lid_open", false)) pf_gfx_text(g, B, p3, x, ly + 12, "LID OPEN", g->th.warn);
}

static void draw_pit(pf_gfx *g, const cJSON *primary, const char *units, const char *mode, int x, int y, int big, int maxw)
{
	bool stopped = !strcmp(mode, "Stop");
	bool valid = primary && cJSON_IsNumber(cJSON_GetObjectItem((cJSON *)primary, "temp"));
	char v[8];
	fmt_temp(v, sizeof v, valid ? cJSON_GetObjectItem((cJSON *)primary, "temp") : NULL);
	if (stopped) { snprintf(v, sizeof v, "0"); valid = false; }
	/* big number with the unit at half size, baseline-aligned to its bottom right */
	uint16_t c = valid ? g->th.text : g->th.muted;
	char u[4];
	snprintf(u, sizeof u, DEG "%c", units[0]);
	while (big > 40 && pf_gfx_text_width(B, big, v) + 2 + pf_gfx_text_width(B, big / 2, u) > maxw) big -= 4;   /* shrink to fit */
	int small = big / 2;
	int adv = pf_gfx_text(g, B, big, x - 4, y, v, c);
	int uy = y + pf_gfx_ascent(B, big) - pf_gfx_ascent(B, small);   /* baselines aligned */
	pf_gfx_text(g, B, small, x - 4 + adv + 2, uy, u, c);
}

/* food probe card: name + Bluetooth signal, big temperature, target and time-to-target.
 * Done probes flash green; 5 °F (3 °C) over the target flash orange; 10 °F (6 °C) over flash red.
 * The flash alternates between a filled card and an outlined card on each display tick. */
static void fmt_eta(char *out, size_t n, double secs)
{
	int m = (int)(secs / 60 + 0.5);
	if (m < 1) m = 1;
	if (m > 99 * 60 + 59) m = 99 * 60 + 59;
	if (m >= 60) snprintf(out, n, "%dh%02d", (m / 60) % 100, m % 60); else snprintf(out, n, "%dm", m % 60);
}

static void draw_probe_col(pf_gfx *g, const cJSON *p, const char *units, bool blink, int x, int y, int w)
{
	const cJSON *tv = cJSON_GetObjectItem((cJSON *)p, "temp");
	double target = pf_json_num((cJSON *)p, "target", 0), eta = pf_json_num((cJSON *)p, "eta_s", -1);
	bool valid = cJSON_IsNumber(tv), hit = target > 0 && valid && tv->valuedouble >= target;
	bool wireless = pf_json_bool((cJSON *)p, "wireless", false);
	int bars = (int)pf_json_num((cJSON *)p, "signal", 0);
	double over = hit ? tv->valuedouble - target : 0, step = units[0] == 'C' ? 3 : 5;
	uint16_t alert = over >= 2 * step ? g->th.danger : over >= step ? g->th.accent : g->th.ok;
	bool filled = hit && !blink;
	if (filled) pf_gfx_rrect(g, x, y, w, 62, 6, alert);
	else {
		pf_gfx_rrect(g, x, y, w, 62, 6, g->th.card);
		if (hit) { pf_gfx_rrect(g, x, y, w, 62, 6, alert); pf_gfx_rrect(g, x + 3, y + 3, w - 6, 56, 4, g->th.card); }
	}
	uint16_t tc = filled ? g->th.accent_text : hit ? alert : valid ? g->th.text : g->th.muted;
	uint16_t mc = filled ? g->th.accent_text : hit ? alert : g->th.muted;
	uint16_t sig_on = filled ? g->th.accent_text : bars >= 3 ? g->th.info : bars == 2 ? g->th.warn : g->th.danger;
	uint16_t sig_off = filled ? alert : g->th.line;
	char name[12], t[8], tg[12] = "", et[8] = "";
	snprintf(name, sizeof name, "%.8s", pf_json_str((cJSON *)p, "name", "?"));
	upper(name);
	fmt_temp(t, sizeof t, tv);
	if (target > 0) snprintf(tg, sizeof tg, "%.0f" DEG, target);
	if (target > 0 && !hit && eta > 0) fmt_eta(et, sizeof et, eta);
	uint16_t tgc = filled ? g->th.accent_text : tc == alert ? alert : g->th.accent;
	if (w >= 90) {
		/* row 1: name left, Bluetooth rune + bars right; row 2: temperature left, target / ETA stacked right */
		int nw = w - 12;
		if (wireless) { pf_gfx_bt_rune(g, x + w - 6 - 15 - 11, y + 4, sig_on); pf_gfx_signal(g, x + w - 6 - 15, y + 3, bars, sig_on, sig_off); nw -= 30; }
		int px = 14;
		while (px > 10 && pf_gfx_text_width(B, px, name) > nw) px--;
		pf_gfx_text(g, B, px, x + 6, y + 4, name, mc);
		pf_gfx_text(g, B, 32, x + 6, y + 18, t, tc);
		if (tg[0]) pf_gfx_text_right(g, B, 15, x + w - 6, y + 20, tg, tgc);
		if (et[0]) pf_gfx_text_right(g, B, 13, x + w - 6, y + 40, et, filled ? g->th.accent_text : g->th.muted);
	} else {   /* narrow (portrait): name + bars / temperature / ETA + target */
		name[6] = 0;
		if (wireless) pf_gfx_signal(g, x + w - 5 - 15, y + 3, bars, sig_on, sig_off);
		pf_gfx_text(g, B, 12, x + 5, y + 3, name, mc);
		pf_gfx_text(g, B, 28, x + 5, y + 15, t, tc);
		if (et[0]) pf_gfx_text(g, B, 11, x + 5, y + 47, et, filled ? g->th.accent_text : g->th.muted);
		if (tg[0]) pf_gfx_text_right(g, B, 12, x + w - 5, y + 46, tg, tgc);
	}
}

static void render_main(pf_gfx *g, const cJSON *s, const pf_ui_state *ui)
{
	const char *mode = pf_json_str((cJSON *)s, "mode", "Stop");
	const char *units = pf_json_str((cJSON *)s, "units", "F");
	int W = g->vw, H = g->vh;
	bool landscape = g->w > g->h;
	draw_banner(g, s, mode);

	const cJSON *probes = cJSON_GetObjectItem((cJSON *)s, "probes");
	const cJSON *primary = NULL, *food[3] = { 0 }, *p;
	int nf = 0;
	cJSON_ArrayForEach(p, probes) {
		const char *role = pf_json_str((cJSON *)p, "role", "");
		if (!strcmp(role, "Primary")) { if (!primary) primary = p; continue; }
		if (!strcmp(role, "Food") && pf_json_bool((cJSON *)p, "enabled", true) && pf_json_bool((cJSON *)p, "home", true) && nf < 3) food[nf++] = p;
	}

	if (landscape) {
		draw_tiles(g, s, 39, 36, 20);
		int col = W - 100;                                      /* data column on the right */
		draw_pit(g, primary, units, mode, 8, 78, 100, col - 12);
		draw_datablock(g, s, primary, mode, units, col, 80, W - col - 6, true);
		int top = H - 66, w = (W - 12 - 10) / 3;
		if (nf == 0) pf_gfx_text(g, R, 16, 8, top + 22, "No food probes enabled", g->th.muted);
		for (int i = 0; i < nf; i++) draw_probe_col(g, food[i], units, ui->blink, 6 + i * (w + 5), top, w);
	} else {
		draw_tiles(g, s, 39, 36, 18);
		draw_pit(g, primary, units, mode, 10, 80, 118, W - 16);
		draw_datablock(g, s, primary, mode, units, 10, 190, W - 20, true);
		int top = H - 66, w = (W - 12 - 10) / 3;
		for (int i = 0; i < nf; i++) draw_probe_col(g, food[i], units, ui->blink, 6 + i * (w + 5), top, w);
	}
}

/* --------------------------------------------------------------- menu / set point / message */

static void render_menu(pf_gfx *g, const cJSON *s, const pf_ui_state *ui)
{
	int W = g->vw, H = g->vh;
	const char *mode = pf_json_str((cJSON *)s, "mode", "Stop");
	pf_menu_item items[PF_MENU_MAX];
	int n = pf_menu_build(s, items, PF_MENU_MAX);
	pf_gfx_rect(g, 0, 0, g->w, 34, g->th.card2);
	pf_gfx_text(g, B, 22, 10, 5, "MENU", g->th.text);
	char up[16]; snprintf(up, sizeof up, "%.12s", mode); upper(up);
	pf_gfx_text_right(g, B, 18, W - 10, 8, up, mode_fill(g, mode) == g->th.card2 ? g->th.muted : mode_fill(g, mode));
	int rowh = (H - 40) / (n > 0 ? n : 1);
	if (rowh > 44) rowh = 44;
	int y = 38 + ((H - 40) - rowh * n) / 2;
	int sel = ui->menu_index % (n > 0 ? n : 1);
	for (int i = 0; i < n; i++) {
		bool is = i == sel;
		if (is) pf_gfx_rrect(g, 6, y + 1, W - 12, rowh - 3, 7, g->th.accent);
		int ty = y + (rowh - pf_gfx_line_height(B, 24)) / 2;
		uint16_t c = is ? g->th.accent_text : (items[i].id == PF_MI_STOP || items[i].id == PF_MI_CLEAR) ? g->th.danger : g->th.text;
		pf_gfx_text(g, B, 24, 20, ty, items[i].label, c);
		y += rowh;
	}
}

static void render_setpoint(pf_gfx *g, const pf_ui_state *ui, const cJSON *s)
{
	int W = g->vw, H = g->vh;
	const char *units = pf_json_str((cJSON *)s, "units", "F");
	pf_gfx_rect(g, 0, 0, g->w, 34, g->th.ok);
	pf_gfx_text(g, B, 22, 10, 5, ui->edit_is_change ? "SET TARGET" : "HOLD AT", g->th.accent_text);
	char v[16];
	snprintf(v, sizeof v, "%.0f", ui->edit_setpoint);
	int big = W >= 320 ? 110 : 92;
	char unit[4] = { (char)0xC2, (char)0xB0, units[0], 0 };
	int vw = pf_gfx_text_width(B, big, v), uw = pf_gfx_text_width(B, big / 3, unit);
	int left = (W - vw - 6 - uw) / 2, ty = 44 + (H - 44 - 40 - pf_gfx_line_height(B, big)) / 2;
	pf_gfx_text(g, B, big, left, ty, v, g->th.text);
	pf_gfx_text(g, B, big / 3, left + vw + 6, ty + (int)(big * 0.18), unit, g->th.muted);
	pf_gfx_text_center(g, B, 16, W / 2, H - 34, "TURN TO ADJUST  \xC2\xB7  PRESS TO CONFIRM", g->th.muted);
}

void pf_screens_render(pf_gfx *g, const cJSON *status, const pf_ui_state *ui)
{
	pf_gfx_clear(g, g->th.bg);
	if (ui->screen == PF_SCR_MENU && status) { render_menu(g, status, ui); return; }
	if (ui->screen == PF_SCR_SETPOINT && status) { render_setpoint(g, ui, status); return; }
	if (ui->screen == PF_SCR_MESSAGE) {
		int w = g->vw - 24, h = 72;
		pf_gfx_rrect(g, 12, g->vh / 2 - h / 2, w, h, 8, g->th.card2);
		int px = pf_gfx_text_width(B, 20, ui->message) <= w - 24 ? 20 : 15;
		pf_gfx_text_center(g, B, px, g->vw / 2, g->vh / 2 - pf_gfx_line_height(B, px) / 2, ui->message, g->th.text);
		return;
	}
	if (status) render_main(g, status, ui);
	else pf_gfx_text_center(g, B, 32, g->vw / 2, g->vh / 2 - 20, "PiFire", g->th.accent);
}
