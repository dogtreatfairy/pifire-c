/* TFT screens. The layout mirrors the web Home page (ring gauge, probe cards, output chips) using the
 * same colour tokens, and every piece of text is at least 16 px tall so it stays legible in sunlight. */
#include "display/screens.h"
#include "core/settings.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

const char *const pf_menu_items[PF_MENU_COUNT] = { "Start (Smoke)", "Hold at...", "Smoke", "Smoke+ toggle", "Shutdown", "Stop", "Back" };

#define GAUGE_START 135.0   /* degrees, clockwise from 3 o'clock: same 270 degree sweep as the web gauge */
#define GAUGE_SWEEP 270.0

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

static uint16_t mode_colour(const pf_gfx *g, const char *mode)
{
	if (!strcmp(mode, "Hold") || !strcmp(mode, "Smoke")) return g->th.ok;
	if (!strcmp(mode, "Error")) return g->th.danger;
	if (!strcmp(mode, "Shutdown")) return g->th.info;
	if (!strcmp(mode, "Stop") || !strcmp(mode, "Monitor")) return g->th.muted;
	return g->th.accent;
}

/* what the top-right of the bar shows, and the short hint under the gauge */
typedef struct { char right[20]; char hint[24]; uint16_t hint_c; } pf_mode_text;

static pf_mode_text mode_text(const pf_gfx *g, const cJSON *s, const char *mode, const char *units)
{
	pf_mode_text t = { "", "", g->th.muted };
	double remaining = pf_json_num((cJSON *)s, "timers.mode_remaining", 0);
	bool cs_active = pf_json_bool((cJSON *)s, "coldstart.active", false), cs_reached = pf_json_bool((cJSON *)s, "coldstart.reached", false);
	if (!strcmp(mode, "Startup") || !strcmp(mode, "Reignite")) {
		double exit_t = pf_json_num((cJSON *)s, "timers.startup_exit_temp", 0);
		if (cs_active && !cs_reached && remaining <= 0) {
			/* timer expired but cold-start is still waiting for the rise: count down its own deadline */
			fmt_clock(t.right, sizeof t.right, pf_json_num((cJSON *)s, "coldstart.remaining", 0));
			snprintf(t.hint, sizeof t.hint, "WAIT RISE");
			t.hint_c = g->th.warn;
		} else {
			fmt_clock(t.right, sizeof t.right, remaining);
			if (cs_active && !cs_reached) { snprintf(t.hint, sizeof t.hint, "COLD START"); t.hint_c = g->th.accent; }
			else if (exit_t > 0) { snprintf(t.hint, sizeof t.hint, "OR %.0f%c%c", exit_t, 0xB0, units[0]); t.hint_c = g->th.accent; }
			else { snprintf(t.hint, sizeof t.hint, "IGNITING"); t.hint_c = g->th.accent; }
		}
	} else if (!strcmp(mode, "Shutdown")) {
		fmt_clock(t.right, sizeof t.right, remaining);
		snprintf(t.hint, sizeof t.hint, "COOLING");
		t.hint_c = g->th.info;
	} else if (!strcmp(mode, "Prime")) {
		fmt_clock(t.right, sizeof t.right, remaining);
		snprintf(t.hint, sizeof t.hint, "PRIME %.0fg", pf_json_num((cJSON *)s, "timers.prime_amount", 0));
		t.hint_c = g->th.accent;
	} else if (!strcmp(mode, "Hold")) {
		snprintf(t.right, sizeof t.right, "SET %.0f%c", pf_json_num((cJSON *)s, "setpoint", 0), 0xB0);
		if (pf_json_bool((cJSON *)s, "lid_open", false)) { snprintf(t.hint, sizeof t.hint, "LID OPEN"); t.hint_c = g->th.warn; }
		else { snprintf(t.hint, sizeof t.hint, "FEED %.0f%%", pf_json_num((cJSON *)s, "cycle.u_applied", 0) * 100); t.hint_c = g->th.text; }
	} else if (!strcmp(mode, "Smoke")) {
		snprintf(t.right, sizeof t.right, "P%d", pf_set_int("cycle_data.PMode", 2));
		snprintf(t.hint, sizeof t.hint, pf_json_bool((cJSON *)s, "s_plus", false) ? "SMOKE+ ON" : "SMOKING");
		t.hint_c = g->th.ok;
	} else if (!strcmp(mode, "Error")) {
		snprintf(t.right, sizeof t.right, "%.12s", pf_json_str((cJSON *)s, "safety.error_code", ""));
		snprintf(t.hint, sizeof t.hint, "PRESS STOP");
		t.hint_c = g->th.danger;
	} else if (!strcmp(mode, "Manual")) {
		snprintf(t.hint, sizeof t.hint, "MANUAL");
		t.hint_c = g->th.warn;
	} else {
		int hop = (int)pf_json_num((cJSON *)s, "hopper_pct", -1);
		if (hop >= 0) { snprintf(t.hint, sizeof t.hint, "HOPPER %d%%", hop % 1000); t.hint_c = hop <= 25 ? g->th.warn : g->th.muted; }
	}
	return t;
}

static void draw_topbar(pf_gfx *g, const cJSON *s, const char *mode, const pf_mode_text *mt)
{
	int W = g->w;
	bool err = !strcmp(mode, "Error");
	pf_gfx_rect(g, 0, 0, W, 26, err ? g->th.danger : g->th.card);
	uint16_t mc = mode_colour(g, mode);
	pf_gfx_disc(g, 12, 13, 5, err ? g->th.text : mc);
	char up[16];
	snprintf(up, sizeof up, "%.12s", mode);
	upper(up);
	pf_gfx_text(g, 24, 5, up, 2, err ? g->th.text : g->th.text);
	if (mt->right[0]) pf_gfx_text_right(g, W - 8, 5, mt->right, 2, err ? g->th.text : g->th.text);
	(void)s;
}

/* ring gauge like the web one: track, filled arc, set-point tick, big number inside */
static void draw_gauge(pf_gfx *g, const cJSON *s, const cJSON *primary, const char *mode, const char *units, int cx, int cy, int r_out, int r_in, int big_scale)
{
	double max = units[0] == 'C' ? 320 : 600;
	bool valid = primary && cJSON_IsNumber(cJSON_GetObjectItem((cJSON *)primary, "temp"));
	double pit = valid ? cJSON_GetObjectItem((cJSON *)primary, "temp")->valuedouble : 0;
	double frac = valid ? fmin(1, fmax(0, pit / max)) : 0;
	pf_gfx_arc(g, cx, cy, r_in, r_out, GAUGE_START, GAUGE_START + GAUGE_SWEEP, g->th.card2);
	if (frac > 0) pf_gfx_arc(g, cx, cy, r_in, r_out, GAUGE_START, GAUGE_START + fmax(1.5, GAUGE_SWEEP * frac), g->th.accent);
	bool hold_like = !strcmp(mode, "Hold") || !strcmp(mode, "Reignite") || (!strcmp(mode, "Startup") && !strcmp(pf_json_str((cJSON *)s, "next_mode", ""), "Hold"));
	double sp = pf_json_num((cJSON *)s, "setpoint", 0);
	if (hold_like && sp > 0) {
		double a = GAUGE_START + GAUGE_SWEEP * fmin(1, sp / max);
		pf_gfx_arc(g, cx, cy, r_in - 5, r_out + 5, a - 1.6, a + 1.6, g->th.text);
	}
	pf_gfx_text_center(g, cx, cy - r_in + 14, "PIT", 2, g->th.muted);
	char big[8];
	fmt_temp(big, sizeof big, valid ? cJSON_GetObjectItem((cJSON *)primary, "temp") : NULL);
	int bh = 8 * big_scale;
	pf_gfx_text_center(g, cx, cy - bh / 2 + 2, big, big_scale, valid ? g->th.text : g->th.muted);
	char deg[4] = { (char)0xB0, units[0], 0, 0 };
	pf_gfx_text_center(g, cx, cy + bh / 2 + 6, deg, 2, g->th.muted);
}

static void draw_probe_card(pf_gfx *g, const cJSON *p, int x, int y, int w, int h, int temp_scale)
{
	const cJSON *tv = cJSON_GetObjectItem((cJSON *)p, "temp");
	double target = pf_json_num((cJSON *)p, "target", 0);
	bool valid = cJSON_IsNumber(tv);
	bool hit = target > 0 && valid && tv->valuedouble >= target;
	pf_gfx_rrect(g, x, y, w, h, 6, g->th.card2);
	if (hit) { pf_gfx_rrect(g, x, y, w, h, 6, g->th.ok); pf_gfx_rrect(g, x + 2, y + 2, w - 4, h - 4, 5, g->th.card2); }
	char name[12];
	snprintf(name, sizeof name, "%.*s", (w - 8) / 16, pf_json_str((cJSON *)p, "name", "?"));
	pf_gfx_text(g, x + 4, y + 4, name, 2, g->th.muted);
	char t[8];
	fmt_temp(t, sizeof t, tv);
	pf_gfx_text(g, x + 4, y + h - 8 * temp_scale - 4, t, temp_scale, hit ? g->th.ok : valid ? g->th.text : g->th.muted);
	if (target > 0) {
		char tg[8];
		snprintf(tg, sizeof tg, ">%.0f", target);
		pf_gfx_text_right(g, x + w - 4, y + h - 16 - 4 - (temp_scale > 2 ? (8 * temp_scale - 16) / 2 : 0), tg, 2, hit ? g->th.ok : g->th.accent);
	}
}

/* one-line card: name left, temp + target right */
static void draw_probe_row(pf_gfx *g, const cJSON *p, int x, int y, int w, int h)
{
	const cJSON *tv = cJSON_GetObjectItem((cJSON *)p, "temp");
	double target = pf_json_num((cJSON *)p, "target", 0);
	bool valid = cJSON_IsNumber(tv);
	bool hit = target > 0 && valid && tv->valuedouble >= target;
	pf_gfx_rrect(g, x, y, w, h, 6, g->th.card2);
	if (hit) { pf_gfx_rrect(g, x, y, w, h, 6, g->th.ok); pf_gfx_rrect(g, x + 2, y + 2, w - 4, h - 4, 5, g->th.card2); }
	int ty = y + (h - 16) / 2;
	char t[8];
	fmt_temp(t, sizeof t, tv);
	int rx = x + w - 6;
	if (target > 0) {
		char tg[8];
		snprintf(tg, sizeof tg, ">%.0f", target);
		pf_gfx_text_right(g, rx, ty, tg, 2, hit ? g->th.ok : g->th.accent);
		rx -= pf_gfx_text_width(tg, 2) + 8;
	}
	pf_gfx_text_right(g, rx, ty, t, 2, hit ? g->th.ok : valid ? g->th.text : g->th.muted);
	rx -= pf_gfx_text_width(t, 2) + 8;
	int chars = (rx - (x + 6)) / 16;
	if (chars > 8) chars = 8;
	if (chars > 0) {
		char name[12];
		snprintf(name, sizeof name, "%.*s", chars, pf_json_str((cJSON *)p, "name", "?"));
		pf_gfx_text(g, x + 6, ty, name, 2, g->th.muted);
	}
}

/* PWR / FAN / AUG / IGN chips, `cols` per row */
static void draw_chips(pf_gfx *g, const cJSON *s, int x, int y, int w, int cols, int scale)
{
	static const char *const names[4] = { "PWR", "FAN", "AUG", "IGN" };
	static const char *const keys[4] = { "outputs.power", "outputs.fan", "outputs.auger", "outputs.igniter" };
	int cw = w / cols, rh = 8 * scale + 6;
	bool roomy = cw >= 70;
	for (int k = 0; k < 4; k++) {
		bool on = pf_json_bool((cJSON *)s, keys[k], false);
		int cx = x + (k % cols) * cw, cy = y + (k / cols) * rh;
		uint16_t dot = on ? (k >= 2 ? g->th.accent : g->th.ok) : g->th.line;
		pf_gfx_disc(g, cx + (roomy ? 6 : 4), cy + 4 * scale + 1, roomy ? 5 : 4, dot);
		char label[12];
		snprintf(label, sizeof label, "%s", names[k]);
		if (roomy && k == 1 && on && pf_set_bool("platform.dc_fan", false)) snprintf(label, sizeof label, "%d%%", (int)pf_json_num((cJSON *)s, "outputs.fan_pct", 100) % 1000);
		pf_gfx_text(g, cx + (roomy ? 16 : 11), cy + 1, label, scale, on ? g->th.text : g->th.muted);
	}
}

static void render_main(pf_gfx *g, const cJSON *s)
{
	const char *mode = pf_json_str((cJSON *)s, "mode", "Stop");
	const char *units = pf_json_str((cJSON *)s, "units", "F");
	int W = g->w, H = g->h;
	bool landscape = W > H;
	pf_mode_text mt = mode_text(g, s, mode, units);
	draw_topbar(g, s, mode, &mt);

	const cJSON *probes = cJSON_GetObjectItem((cJSON *)s, "probes");
	const cJSON *primary = NULL, *p;
	cJSON_ArrayForEach(p, probes) if (!strcmp(pf_json_str((cJSON *)p, "role", ""), "Primary")) { primary = p; break; }

	if (landscape) {
		/* left: gauge + hint; right: up to three food probes and the output chips */
		int cx = 80, cy = 128, r_out = 70, r_in = 58;
		draw_gauge(g, s, primary, mode, units, cx, cy, r_out, r_in, 5);
		if (mt.hint[0]) pf_gfx_text_center(g, cx, cy + r_out + 8, mt.hint, 2, mt.hint_c);
		int x = 166, w = W - x - 6, y = 30, h = 50, i = 0;
		cJSON_ArrayForEach(p, probes) {
			const char *role = pf_json_str((cJSON *)p, "role", "");
			if (!strcmp(role, "Primary") || !strcmp(role, "Aux") || !pf_json_bool((cJSON *)p, "enabled", true) || i >= 3) continue;
			draw_probe_card(g, p, x, y + i * (h + 4), w, h, 3);
			i++;
		}
		draw_chips(g, s, x, 194, w, 2, 2);
	} else {
		/* gauge, hint, one row of output chips, then one-line probe cards */
		int cx = W / 2, cy = 106, r_out = 70, r_in = 58;
		draw_gauge(g, s, primary, mode, units, cx, cy, r_out, r_in, 5);
		if (mt.hint[0]) pf_gfx_text_center(g, cx, cy + r_out + 6, mt.hint, 2, mt.hint_c);
		draw_chips(g, s, 2, 204, W - 4, 4, 2);
		int y = 228, h = 28, i = 0;
		cJSON_ArrayForEach(p, probes) {
			const char *role = pf_json_str((cJSON *)p, "role", "");
			if (!strcmp(role, "Primary") || !strcmp(role, "Aux") || !pf_json_bool((cJSON *)p, "enabled", true) || i >= 3) continue;
			draw_probe_row(g, p, 6, y + i * (h + 2), W - 12, h);
			i++;
		}
	}
}

static void render_menu(pf_gfx *g, const pf_ui_state *ui)
{
	int W = g->w, H = g->h;
	pf_gfx_rect(g, 0, 0, W, 26, g->th.card);
	pf_gfx_text(g, 8, 5, "MENU", 2, g->th.accent);
	pf_gfx_text_right(g, W - 8, 5, "turn / press", 2, g->th.muted);
	int rowh = (H - 30) / PF_MENU_COUNT, y = 30;
	for (int i = 0; i < PF_MENU_COUNT; i++) {
		bool sel = i == ui->menu_index;
		if (sel) pf_gfx_rrect(g, 4, y, W - 8, rowh - 2, 6, g->th.accent);
		pf_gfx_text(g, 14, y + (rowh - 2 - 16) / 2, pf_menu_items[i], 2, sel ? g->th.accent_text : g->th.text);
		y += rowh;
	}
}

static void render_setpoint(pf_gfx *g, const pf_ui_state *ui, const cJSON *s)
{
	int W = g->w, H = g->h;
	const char *units = pf_json_str((cJSON *)s, "units", "F");
	pf_gfx_rect(g, 0, 0, W, 26, g->th.card);
	pf_gfx_text(g, 8, 5, "HOLD AT", 2, g->th.accent);
	pf_gfx_text_right(g, W - 8, 5, "press: go", 2, g->th.muted);
	char v[16];
	snprintf(v, sizeof v, "%.0f", ui->edit_setpoint);
	int sc = W >= 320 ? 8 : 6;
	int w = pf_gfx_text_width(v, sc);
	pf_gfx_text(g, (W - w) / 2 - 10, H / 2 - sc * 4 - 6, v, sc, g->th.text);
	char deg[4] = { (char)0xB0, units[0], 0, 0 };
	pf_gfx_text(g, (W + w) / 2 - 6, H / 2 - sc * 4 - 6, deg, 2, g->th.muted);
	double max = units[0] == 'C' ? 320 : 600;
	pf_gfx_bar(g, 16, H / 2 + sc * 4 + 2, W - 32, 8, ui->edit_setpoint / max, g->th.accent, g->th.card2);
	pf_gfx_text_center(g, W / 2, H - 24, "hold: cancel", 2, g->th.muted);
}

void pf_screens_render(pf_gfx *g, const cJSON *status, const pf_ui_state *ui)
{
	pf_gfx_clear(g, g->th.bg);
	if (ui->screen == PF_SCR_MENU) { render_menu(g, ui); return; }
	if (ui->screen == PF_SCR_SETPOINT) { render_setpoint(g, ui, status); return; }
	if (ui->screen == PF_SCR_MESSAGE) {
		pf_gfx_rrect(g, 10, g->h / 2 - 30, g->w - 20, 60, 8, g->th.card);
		int sc = pf_gfx_text_width(ui->message, 2) <= g->w - 32 ? 2 : 1;
		pf_gfx_text_center(g, g->w / 2, g->h / 2 - 4 * sc, ui->message, sc, g->th.text);
		return;
	}
	if (status) render_main(g, status);
	else pf_gfx_text_center(g, g->w / 2, g->h / 2 - 12, "PiFire", 3, g->th.accent);
}
