/* TFT screens: a minimal, type-led layout in Inter. The primary probe is the hero, the set point and
 * the running mode/timers sit with it, and up to three food probes get their own cards. */
#include "display/screens.h"
#include "core/settings.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define R PF_FONT_REGULAR
#define B PF_FONT_SEMIBOLD
#define DEG "\xC2\xB0"
#define DOT " \xC2\xB7 "
#define ARROW "\xE2\x86\x92"

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

static uint16_t mode_colour(const pf_gfx *g, const char *mode)
{
	if (!strcmp(mode, "Hold") || !strcmp(mode, "Smoke")) return g->th.ok;
	if (!strcmp(mode, "Error")) return g->th.danger;
	if (!strcmp(mode, "Shutdown")) return g->th.info;
	if (!strcmp(mode, "Stop") || !strcmp(mode, "Monitor")) return g->th.muted;
	return g->th.accent;
}

static bool is_timed(const char *mode)
{
	return !strcmp(mode, "Startup") || !strcmp(mode, "Reignite") || !strcmp(mode, "Shutdown") || !strcmp(mode, "Prime");
}

/* --------------------------------------------------------------- menu */

int pf_menu_build(const cJSON *status, pf_menu_item *out, int max)
{
	const char *mode = pf_json_str((cJSON *)status, "mode", "Stop");
	int n = 0;
#define ADD(i, l) do { if (n < max) { out[n].id = (i); snprintf(out[n].label, sizeof out[n].label, "%s", (l)); n++; } } while (0)
	if (!strcmp(mode, "Stop") || !strcmp(mode, "Monitor")) {
		ADD(PF_MI_START_SMOKE, "Start " ARROW " Smoke");
		ADD(PF_MI_START_HOLD, "Start " ARROW " Hold");
		if (strcmp(mode, "Monitor")) ADD(PF_MI_MONITOR, "Monitor");
		else ADD(PF_MI_STOP, "Stop");
	} else if (!strcmp(mode, "Smoke")) {
		ADD(PF_MI_HOLD, "Hold");
		ADD(PF_MI_SMOKE_PLUS, pf_json_bool((cJSON *)status, "s_plus", false) ? "Smoke+ off" : "Smoke+ on");
		ADD(PF_MI_SHUTDOWN, "Shutdown");
		ADD(PF_MI_STOP, "Stop");
	} else if (!strcmp(mode, "Hold")) {
		ADD(PF_MI_SETPOINT, "Set point");
		ADD(PF_MI_SMOKE, "Smoke");
		ADD(PF_MI_SHUTDOWN, "Shutdown");
		ADD(PF_MI_STOP, "Stop");
	} else if (!strcmp(mode, "Error")) {
		ADD(PF_MI_CLEAR, "Clear & stop");
	} else if (!strcmp(mode, "Shutdown")) {
		ADD(PF_MI_STOP, "Stop");
	} else {  /* Startup, Reignite, Prime, Manual */
		ADD(PF_MI_SHUTDOWN, "Shutdown");
		ADD(PF_MI_STOP, "Stop");
	}
	ADD(PF_MI_BACK, "Back");
#undef ADD
	return n;
}

/* --------------------------------------------------------------- main screen pieces */

typedef struct {
	char bar_right[24];     /* top bar, right side: countdown / timer / cook time */
	char line1[40];         /* under the hero: set point or mode statement */
	char line2[56];         /* muted detail line */
	uint16_t line1_c;
} hero_text;

static hero_text compose(const pf_gfx *g, const cJSON *s, const char *mode, const char *units)
{
	hero_text t = { "", "", "", g->th.accent };
	double remaining = pf_json_num((cJSON *)s, "timers.mode_remaining", 0);
	double mode_el = pf_json_num((cJSON *)s, "mode_elapsed", 0), cook_el = pf_json_num((cJSON *)s, "cook_elapsed", 0);
	bool cs_active = pf_json_bool((cJSON *)s, "coldstart.active", false), cs_reached = pf_json_bool((cJSON *)s, "coldstart.reached", false);
	bool timer_on = pf_json_bool((cJSON *)s, "timer.running", false);
	char clk[16], clk2[16];

	/* top-right: the most urgent clock */
	if (is_timed(mode)) {
		bool waiting = (!strcmp(mode, "Startup") || !strcmp(mode, "Reignite")) && cs_active && !cs_reached && remaining <= 0;
		fmt_clock(clk, sizeof clk, waiting ? pf_json_num((cJSON *)s, "coldstart.remaining", 0) : remaining);
		snprintf(t.bar_right, sizeof t.bar_right, "%s left", clk);
	} else if (timer_on) {
		fmt_clock(clk, sizeof clk, pf_json_num((cJSON *)s, "timer.remaining", 0));
		snprintf(t.bar_right, sizeof t.bar_right, "timer %s", clk);
	} else if (cook_el > 0) {
		fmt_clock(clk, sizeof clk, cook_el);
		snprintf(t.bar_right, sizeof t.bar_right, "%s", clk);
	}

	double sp = pf_json_num((cJSON *)s, "setpoint", 0);
	const char *next = pf_json_str((cJSON *)s, "next_mode", "");
	fmt_clock(clk, sizeof clk, mode_el);
	fmt_clock(clk2, sizeof clk2, cook_el);
	if (!strcmp(mode, "Hold")) {
		snprintf(t.line1, sizeof t.line1, "Set %.0f" DEG "%c", sp, units[0]);
		if (pf_json_bool((cJSON *)s, "lid_open", false)) snprintf(t.line2, sizeof t.line2, "Lid open" DOT "feed paused");
		else snprintf(t.line2, sizeof t.line2, "Feed %.0f%%" DOT "%s in mode", pf_json_num((cJSON *)s, "cycle.u_applied", 0) * 100, clk);
	} else if (!strcmp(mode, "Smoke")) {
		snprintf(t.line1, sizeof t.line1, "%s", pf_json_bool((cJSON *)s, "s_plus", false) ? "Smoke+ on" : "Smoke");
		t.line1_c = g->th.ok;
		snprintf(t.line2, sizeof t.line2, "P%d" DOT "%s in mode", pf_set_int("cycle_data.PMode", 2), clk);
	} else if (!strcmp(mode, "Startup") || !strcmp(mode, "Reignite")) {
		double exit_t = pf_json_num((cJSON *)s, "timers.startup_exit_temp", 0);
		if (!strcmp(next, "Hold") && sp > 0) snprintf(t.line1, sizeof t.line1, "Then hold %.0f" DEG "%c", sp, units[0]);
		else snprintf(t.line1, sizeof t.line1, "Then smoke");
		if (cs_active && !cs_reached) snprintf(t.line2, sizeof t.line2, "Cold start" DOT "waiting for rise");
		else if (exit_t > 0) snprintf(t.line2, sizeof t.line2, "Igniting" DOT "exits at %.0f" DEG "%c", exit_t, units[0]);
		else snprintf(t.line2, sizeof t.line2, "Igniting" DOT "%s in mode", clk);
	} else if (!strcmp(mode, "Shutdown")) {
		snprintf(t.line1, sizeof t.line1, "Cooling down");
		t.line1_c = g->th.info;
		snprintf(t.line2, sizeof t.line2, "Fan on, feed off" DOT "cook %s", clk2);
	} else if (!strcmp(mode, "Prime")) {
		snprintf(t.line1, sizeof t.line1, "Priming %.0f g", pf_json_num((cJSON *)s, "timers.prime_amount", 0));
		snprintf(t.line2, sizeof t.line2, "Auger running");
	} else if (!strcmp(mode, "Error")) {
		snprintf(t.line1, sizeof t.line1, "%.24s", pf_json_str((cJSON *)s, "safety.error_code", "Error"));
		t.line1_c = g->th.danger;
		snprintf(t.line2, sizeof t.line2, "Outputs off" DOT "press to clear");
	} else if (!strcmp(mode, "Manual")) {
		snprintf(t.line1, sizeof t.line1, "Manual outputs");
		t.line1_c = g->th.warn;
		snprintf(t.line2, sizeof t.line2, "%s in mode", clk);
	} else if (!strcmp(mode, "Monitor")) {
		snprintf(t.line1, sizeof t.line1, "Monitoring");
		t.line1_c = g->th.muted;
		snprintf(t.line2, sizeof t.line2, "Outputs off");
	} else {
		snprintf(t.line1, sizeof t.line1, "Ready");
		t.line1_c = g->th.muted;
		int hop = (int)pf_json_num((cJSON *)s, "hopper_pct", -1);
		if (hop >= 0) snprintf(t.line2, sizeof t.line2, "Hopper %d%%" DOT "press for menu", hop % 1000);
		else snprintf(t.line2, sizeof t.line2, "Press for menu");
	}
	return t;
}

static void draw_topbar(pf_gfx *g, const char *mode, const hero_text *t)
{
	int W = g->w;
	bool err = !strcmp(mode, "Error");
	if (err) pf_gfx_rect(g, 0, 0, W, 30, g->th.danger);
	pf_gfx_disc(g, 14, 15, 4, err ? g->th.text : mode_colour(g, mode));
	pf_gfx_text(g, B, 15, 26, 6, mode, g->th.text);
	if (t->bar_right[0]) pf_gfx_text_right(g, R, 15, W - 12, 6, t->bar_right, err ? g->th.text : g->th.muted);
	if (!err) pf_gfx_rect(g, 0, 30, W, 1, g->th.line);
}

/* hero block inside [x, x+w): label, huge temperature, unit, then the two text lines */
static void draw_hero(pf_gfx *g, const cJSON *primary, const char *units, const hero_text *t, int x, int y, int w, int big)
{
	bool valid = primary && cJSON_IsNumber(cJSON_GetObjectItem((cJSON *)primary, "temp"));
	char v[8];
	fmt_temp(v, sizeof v, valid ? cJSON_GetObjectItem((cJSON *)primary, "temp") : NULL);
	char unit[4] = { (char)0xC2, (char)0xB0, units[0], 0 };
	int vw = pf_gfx_text_width(B, big, v), uw = pf_gfx_text_width(R, big / 3, unit);
	int cx = x + w / 2, left = cx - (vw + 4 + uw) / 2;
	pf_gfx_text_center(g, R, 13, cx, y, primary ? pf_json_str((cJSON *)primary, "name", "Pit") : "Pit", g->th.muted);
	int ty = y + 14;
	pf_gfx_text(g, B, big, left, ty, v, valid ? g->th.text : g->th.muted);
	pf_gfx_text(g, R, big / 3, left + vw + 4, ty + (int)(big * 0.16), unit, g->th.muted);
	int ly = ty + pf_gfx_line_height(B, big) - (int)(big * 0.14);
	if (t->line1[0]) pf_gfx_text_center(g, B, 19, cx, ly, t->line1, t->line1_c);
	if (t->line2[0]) pf_gfx_text_center(g, R, 13, cx, ly + 25, t->line2, g->th.muted);
}

static void draw_probe_card(pf_gfx *g, const cJSON *p, int x, int y, int w, int h)
{
	const cJSON *tv = cJSON_GetObjectItem((cJSON *)p, "temp");
	double target = pf_json_num((cJSON *)p, "target", 0);
	bool valid = cJSON_IsNumber(tv);
	bool hit = target > 0 && valid && tv->valuedouble >= target;
	pf_gfx_rrect(g, x, y, w, h, 8, g->th.card);
	if (hit) pf_gfx_rrect(g, x + 4, y + 8, 3, h - 16, 1, g->th.ok);
	char t[8], tg[16] = "";
	fmt_temp(t, sizeof t, tv);
	if (target > 0) snprintf(tg, sizeof tg, ARROW " %.0f" DEG, target);
	uint16_t tc = hit ? g->th.ok : valid ? g->th.text : g->th.muted;
	if (w >= 150) {
		/* one line: name ..... temp  →target */
		pf_gfx_text(g, R, 14, x + 12, y + (h - 17) / 2, pf_json_str((cJSON *)p, "name", "?"), g->th.muted);
		int rx = x + w - 12;
		if (tg[0]) { pf_gfx_text_right(g, R, 14, rx, y + (h - 17) / 2, tg, hit ? g->th.ok : g->th.accent); rx -= pf_gfx_text_width(R, 14, tg) + 10; }
		pf_gfx_text_right(g, B, 24, rx, y + (h - pf_gfx_line_height(B, 24)) / 2, t, tc);
	} else {
		int big = h >= 56 ? 28 : 22;
		pf_gfx_text(g, R, 13, x + 12, y + 6, pf_json_str((cJSON *)p, "name", "?"), g->th.muted);
		if (tg[0]) pf_gfx_text_right(g, R, 13, x + w - 10, y + 6, tg, hit ? g->th.ok : g->th.accent);
		pf_gfx_text(g, B, big, x + 12, y + h - pf_gfx_line_height(B, big) - 3, t, tc);
	}
}

static void draw_outputs(pf_gfx *g, const cJSON *s, int x, int y, int w)
{
	static const char *const names[4] = { "Power", "Fan", "Auger", "Igniter" };
	static const char *const keys[4] = { "outputs.power", "outputs.fan", "outputs.auger", "outputs.igniter" };
	int cx = x;
	for (int k = 0; k < 4; k++) {
		bool on = pf_json_bool((cJSON *)s, keys[k], false);
		char label[16];
		snprintf(label, sizeof label, "%s", names[k]);
		if (k == 1 && on && pf_set_bool("platform.dc_fan", false)) snprintf(label, sizeof label, "Fan %d%%", (int)pf_json_num((cJSON *)s, "outputs.fan_pct", 100) % 1000);
		pf_gfx_disc(g, cx + 4, y + 8, 3, on ? (k >= 2 ? g->th.accent : g->th.ok) : g->th.line);
		pf_gfx_text(g, R, 13, cx + 12, y, label, on ? g->th.text : g->th.muted);
		cx += 12 + pf_gfx_text_width(R, 13, label) + 14;
	}
	int hop = (int)pf_json_num((cJSON *)s, "hopper_pct", -1);
	if (hop >= 0 && cx < x + w - 70) {
		char hs[16];
		snprintf(hs, sizeof hs, "Hopper %d%%", hop % 1000);
		pf_gfx_text_right(g, R, 13, x + w, y, hs, hop <= 25 ? g->th.warn : g->th.muted);
	}
}

static void render_main(pf_gfx *g, const cJSON *s)
{
	const char *mode = pf_json_str((cJSON *)s, "mode", "Stop");
	const char *units = pf_json_str((cJSON *)s, "units", "F");
	int W = g->w, H = g->h;
	bool landscape = W > H;
	hero_text t = compose(g, s, mode, units);
	draw_topbar(g, mode, &t);

	const cJSON *probes = cJSON_GetObjectItem((cJSON *)s, "probes");
	const cJSON *primary = NULL, *food[3] = { 0 }, *p;
	int nf = 0;
	cJSON_ArrayForEach(p, probes) {
		const char *role = pf_json_str((cJSON *)p, "role", "");
		if (!strcmp(role, "Primary")) { if (!primary) primary = p; continue; }
		if (!strcmp(role, "Food") && pf_json_bool((cJSON *)p, "enabled", true) && nf < 3) food[nf++] = p;
	}

	int foot = H - 22;  /* outputs row */
	if (landscape) {
		if (nf == 0) {
			draw_hero(g, primary, units, &t, 0, 44, W, 88);
		} else {
			int col = 184;
			draw_hero(g, primary, units, &t, 0, 46, col, 74);
			int x = col + 2, w = W - x - 8, top = 40, gap = 6;
			int h = (foot - 8 - top - gap * (nf - 1)) / nf;
			if (h > 64) h = 64;
			for (int i = 0; i < nf; i++) draw_probe_card(g, food[i], x, top + i * (h + gap), w, h);
		}
	} else {
		draw_hero(g, primary, units, &t, 0, 42, W, 92);
		int top = 200, gap = 4, w = W - 16;
		if (nf) {
			int h = (foot - 8 - top - gap * (nf - 1)) / nf;
			if (h > 40) h = 40;
			for (int i = 0; i < nf; i++) draw_probe_card(g, food[i], 8, top + i * (h + gap), w, h);
		}
	}
	draw_outputs(g, s, 12, foot + 3, W - 24);
}

/* --------------------------------------------------------------- menu / set point / message */

static void render_menu(pf_gfx *g, const cJSON *s, const pf_ui_state *ui)
{
	int W = g->w, H = g->h;
	const char *mode = pf_json_str((cJSON *)s, "mode", "Stop");
	pf_menu_item items[PF_MENU_MAX];
	int n = pf_menu_build(s, items, PF_MENU_MAX);
	pf_gfx_text(g, B, 15, 14, 6, "Menu", g->th.text);
	pf_gfx_text_right(g, R, 15, W - 12, 6, mode, mode_colour(g, mode));
	pf_gfx_rect(g, 0, 30, W, 1, g->th.line);
	int rowh = (H - 38) / (n > 0 ? n : 1);
	if (rowh > 40) rowh = 40;
	int y = 34 + ((H - 38) - rowh * n) / 2;
	int sel = ui->menu_index % (n > 0 ? n : 1);
	for (int i = 0; i < n; i++) {
		bool is = i == sel;
		if (is) pf_gfx_rrect(g, 8, y + 1, W - 16, rowh - 2, 9, g->th.accent);
		int ty = y + (rowh - pf_gfx_line_height(is ? B : R, 19)) / 2;
		uint16_t c = is ? g->th.accent_text : (items[i].id == PF_MI_STOP || items[i].id == PF_MI_CLEAR) ? g->th.danger : g->th.text;
		pf_gfx_text(g, is ? B : R, 19, 22, ty, items[i].label, c);
		if (is) pf_gfx_text_right(g, R, 19, W - 22, ty, "\xE2\x80\xA2", g->th.accent_text);
		y += rowh;
	}
}

static void render_setpoint(pf_gfx *g, const pf_ui_state *ui, const cJSON *s)
{
	int W = g->w, H = g->h;
	const char *units = pf_json_str((cJSON *)s, "units", "F");
	pf_gfx_text(g, B, 15, 14, 6, ui->edit_is_change ? "Set point" : "Hold", g->th.text);
	pf_gfx_text_right(g, R, 15, W - 12, 6, "turn to adjust", g->th.muted);
	pf_gfx_rect(g, 0, 30, W, 1, g->th.line);
	char v[16];
	snprintf(v, sizeof v, "%.0f", ui->edit_setpoint);
	int big = W >= 320 ? 100 : 84;
	char unit[4] = { (char)0xC2, (char)0xB0, units[0], 0 };
	int vw = pf_gfx_text_width(B, big, v), uw = pf_gfx_text_width(R, big / 3, unit);
	int left = (W - vw - 6 - uw) / 2, ty = H / 2 - pf_gfx_line_height(B, big) / 2 - 6;
	pf_gfx_text(g, B, big, left, ty, v, g->th.text);
	pf_gfx_text(g, R, big / 3, left + vw + 6, ty + (int)(big * 0.16), unit, g->th.muted);
	double max = units[0] == 'C' ? 320 : 600, min = units[0] == 'C' ? 40 : 100;
	pf_gfx_bar(g, 24, H - 54, W - 48, 6, (ui->edit_setpoint - min) / (max - min), g->th.accent, g->th.card2);
	pf_gfx_text_center(g, R, 13, W / 2, H - 36, ui->edit_is_change ? "press to apply" DOT "hold to cancel" : "press to start" DOT "hold to cancel", g->th.muted);
}

void pf_screens_render(pf_gfx *g, const cJSON *status, const pf_ui_state *ui)
{
	pf_gfx_clear(g, g->th.bg);
	if (ui->screen == PF_SCR_MENU && status) { render_menu(g, status, ui); return; }
	if (ui->screen == PF_SCR_SETPOINT && status) { render_setpoint(g, ui, status); return; }
	if (ui->screen == PF_SCR_MESSAGE) {
		int w = g->w - 32, h = 64;
		pf_gfx_rrect(g, 16, g->h / 2 - h / 2, w, h, 10, g->th.card);
		int px = pf_gfx_text_width(R, 17, ui->message) <= w - 24 ? 17 : 13;
		pf_gfx_text_center(g, R, px, g->w / 2, g->h / 2 - pf_gfx_line_height(R, px) / 2, ui->message, g->th.text);
		return;
	}
	if (status) render_main(g, status);
	else pf_gfx_text_center(g, B, 28, g->w / 2, g->h / 2 - 18, "PiFire", g->th.accent);
}
