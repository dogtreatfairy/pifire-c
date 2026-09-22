/* TFT screens, industrial-HMI style: a filled mode banner, big filled status tiles for fan / auger /
 * igniter, the pit temperature large with the set point and the error next to it, food probes below.
 * Everything is bold and high-contrast; nothing is decorative. */
#include "display/screens.h"
#include "display/qr.h"
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
/* Black or white, chosen by how bright the fill actually is rather than by naming the colours that
 * happen to take white today. A hand-kept list goes stale the moment a theme or a tile colour
 * changes, and outdoors the wrong choice is not merely ugly: white on amber in direct sun washes
 * out to nothing. Pure black and pure white are picked deliberately -- this panel is read at arm's
 * length in sunlight, and that is where the contrast has to come from. */
static uint16_t on_fill_text(const pf_gfx *g, uint16_t fill)
{
	(void)g;
	int r = (fill >> 11) & 0x1F, gg = (fill >> 5) & 0x3F, b = fill & 0x1F;
	int lum = (54 * (r << 3) + 183 * (gg << 2) + 19 * (b << 3)) >> 8;   /* Rec. 601 on 0..255 */
	return lum > 140 ? PF_RGB(0, 0, 0) : PF_RGB(255, 255, 255);
}

static bool is_timed(const char *mode)
{
	return !strcmp(mode, "Startup") || !strcmp(mode, "Reignite") || !strcmp(mode, "Shutdown") || !strcmp(mode, "Prime");
}

/* --------------------------------------------------------------- navigation */

const pf_bt_kind PF_BT_KINDS[] = {
	{ "chefiq", "Chef iQ" }, { "meater", "MEATER" }, { "ibbq", "Inkbird / iBBQ" },
};
const int PF_BT_KIND_COUNT = (int)(sizeof PF_BT_KINDS / sizeof PF_BT_KINDS[0]);

void pf_nav_reset(pf_ui_state *ui) { ui->depth = 0; }

void pf_nav_push(pf_ui_state *ui, pf_screen screen, int list)
{
	if (ui->depth >= PF_NAV_MAX) ui->depth = PF_NAV_MAX - 1;
	ui->stack[ui->depth].screen = screen;
	ui->stack[ui->depth].list = list;
	ui->stack[ui->depth].index = 0;
	ui->depth++;
}

void pf_nav_pop(pf_ui_state *ui) { if (ui->depth > 0) ui->depth--; }

pf_screen pf_nav_screen(const pf_ui_state *ui)
{
	return ui->depth > 0 ? ui->stack[ui->depth - 1].screen : PF_SCR_MAIN;
}

pf_nav *pf_nav_top(pf_ui_state *ui) { return ui->depth > 0 ? &ui->stack[ui->depth - 1] : NULL; }

/* --------------------------------------------------------------- menus */

/* The Bluetooth probes that are paired, one row per physical probe (its ambient sibling rides
 * along). Built from the status so the panel needs no settings access to show the list. */
int pf_bt_devices(const cJSON *status, pf_bt_device *out, int max)
{
	int n = 0;
	const cJSON *p;
	cJSON_ArrayForEach(p, cJSON_GetObjectItem((cJSON *)status, "probes")) {
		if (n >= max) break;
		if (!pf_json_bool((cJSON *)p, "wireless", false)) continue;
		if (pf_json_bool((cJSON *)p, "companion", false)) continue;
		const char *dev = pf_json_str((cJSON *)p, "device", "");
		if (!dev[0]) continue;
		bool seen = false;
		for (int i = 0; i < n; i++) if (!strcmp(out[i].device, dev)) seen = true;
		if (seen) continue;
		snprintf(out[n].device, sizeof out[n].device, "%s", dev);
		snprintf(out[n].name, sizeof out[n].name, "%s", pf_json_str((cJSON *)p, "name", dev));
		out[n].enabled = pf_json_bool((cJSON *)p, "enabled", true);
		n++;
	}
	return n;
}

int pf_menu_build(const cJSON *status, const pf_ui_state *ui, pf_menu_item *out, int max)
{
	const char *mode = pf_json_str((cJSON *)status, "mode", "Stop");
	int list = ui->depth > 0 ? ui->stack[ui->depth - 1].list : PF_LIST_ROOT;
	int n = 0;
#define ADD(a, ar, l) do { if (n < max) { out[n].act = (a); out[n].arg = (ar); out[n].danger = false; out[n].right[0] = 0; snprintf(out[n].label, sizeof out[n].label, "%.*s", (int)sizeof out[n].label - 1, (l)); n++; } } while (0)
#define DANGER() do { if (n > 0) out[n - 1].danger = true; } while (0)

	switch (list) {
	case PF_LIST_STARTUP:
		ADD(PF_ACT_STARTUP_HOLD, 0, "Startup To Hold");
		ADD(PF_ACT_STARTUP_SMOKE, 0, "Startup To Smoke");
		ADD(PF_ACT_BACK, 0, "Back");
		break;

	case PF_LIST_SETTINGS: {
		/* only what is worth changing with the screen in front of you */
		ADD(PF_ACT_MARGINS, 0, "Screen Margins");
		char th[16];
		pf_set_str("display.theme", th, sizeof th, "dark");
		ADD(PF_ACT_THEME, 0, "Theme");
		snprintf(out[n - 1].right, sizeof out[n - 1].right, "%s", th[0] == 'l' ? "Light" : "Dark");
		/* The one setting you cannot judge from a phone: whether this panel is wired RGB or BGR is
		 * a question about the thing in front of you, and the answer is obvious the instant it is
		 * right. Orange stops being blue. */
		ADD(PF_ACT_COLOUR, 0, "Colour Order");
		snprintf(out[n - 1].right, sizeof out[n - 1].right, "%s", pf_set_bool("display.bgr", false) ? "BGR" : "RGB");
		ADD(PF_ACT_BACK, 0, "Back");
		break;
	}

	case PF_LIST_POWER:
		ADD(PF_ACT_RESTART, 0, "Restart"); DANGER();
		ADD(PF_ACT_POWEROFF, 0, "Shut Down"); DANGER();
		ADD(PF_ACT_BACK, 0, "Back");
		break;

	case PF_LIST_PROBE: {
		/* every probe that can carry a target, with its current reading alongside */
		const cJSON *probes = cJSON_GetObjectItem((cJSON *)status, "probes"), *p;
		const char *units = pf_json_str((cJSON *)status, "units", "F");
		int i = 0;
		cJSON_ArrayForEach(p, probes) {
			const char *role = pf_json_str((cJSON *)p, "role", "");
			/* companions are shown inside their sibling's card, so they carry no target of their own */
			if (strcmp(role, "Food") || !pf_json_bool((cJSON *)p, "enabled", true) || pf_json_bool((cJSON *)p, "companion", false)) { i++; continue; }
			ADD(PF_ACT_PROBE_TARGET, i, pf_json_str((cJSON *)p, "name", "Probe"));
			if (n > 0) {
				const cJSON *tv = cJSON_GetObjectItem((cJSON *)p, "temp");
				double target = pf_json_num((cJSON *)p, "target", 0);
				if (target > 0) snprintf(out[n - 1].right, sizeof out[n - 1].right, "%.0f" DEG "%c", target, units[0]);
				else if (cJSON_IsNumber(tv)) snprintf(out[n - 1].right, sizeof out[n - 1].right, "%.0f" DEG, tv->valuedouble);
			}
			i++;
		}
		if (n == 0) ADD(PF_ACT_NONE, 0, "No food probes");
		ADD(PF_ACT_BACK, 0, "Back");
		break;
	}

	case PF_LIST_BT: {
		pf_bt_device devs[PF_BT_DEV_MAX];
		int nd = pf_bt_devices(status, devs, PF_BT_DEV_MAX);
		ADD(PF_ACT_LIST, PF_LIST_BTKIND, "Connect");
		if (nd > 0) {
			ADD(PF_ACT_LIST, PF_LIST_BTEDIT, "Edit");
			ADD(PF_ACT_LIST, PF_LIST_BTDEL, "Delete"); DANGER();
		}
		ADD(PF_ACT_BACK, 0, "Back");
		break;
	}

	case PF_LIST_BTKIND:
		for (int i = 0; i < PF_BT_KIND_COUNT; i++) ADD(PF_ACT_BT_SCAN, i, PF_BT_KINDS[i].label);
		ADD(PF_ACT_BACK, 0, "Back");
		break;

	case PF_LIST_BTEDIT: case PF_LIST_BTDEL: {
		pf_bt_device devs[PF_BT_DEV_MAX];
		int nd = pf_bt_devices(status, devs, PF_BT_DEV_MAX);
		bool del = list == PF_LIST_BTDEL;
		for (int i = 0; i < nd; i++) {
			ADD(del ? PF_ACT_BT_DELETE : PF_ACT_BT_TOGGLE, i, devs[i].name);
			if (n > 0) {
				if (del) out[n - 1].danger = true;
				else snprintf(out[n - 1].right, sizeof out[n - 1].right, "%s", devs[i].enabled ? "On" : "Off");
			}
		}
		if (nd == 0) ADD(PF_ACT_NONE, 0, "None paired");
		ADD(PF_ACT_BACK, 0, "Back");
		break;
	}

	default:   /* PF_LIST_ROOT: the mode decides which menu this is */
		if (!strcmp(mode, "Error")) {
			ADD(PF_ACT_CLEAR_ERROR, 0, "Clear Error"); DANGER();
			ADD(PF_ACT_NETINFO, 0, "Network Info");
			ADD(PF_ACT_BACK, 0, "Back");
		} else if (!strcmp(mode, "Monitor")) {
			ADD(PF_ACT_MANUAL, 0, "Control");
			ADD(PF_ACT_LIST, PF_LIST_STARTUP, "Startup");
			ADD(PF_ACT_STOP, 0, "Stop"); DANGER();
			ADD(PF_ACT_LIST, PF_LIST_BT, "Bluetooth Probes");
			ADD(PF_ACT_NETINFO, 0, "Network Info");
			ADD(PF_ACT_BACK, 0, "Back");
		} else if (!strcmp(mode, "Stop") || !strcmp(mode, "Prime")) {
			ADD(PF_ACT_LIST, PF_LIST_STARTUP, "Startup");
			ADD(PF_ACT_MONITOR, 0, "Monitor");
			ADD(PF_ACT_NETINFO, 0, "Network Info");
			ADD(PF_ACT_LIST, PF_LIST_SETTINGS, "Settings");
			ADD(PF_ACT_LIST, PF_LIST_POWER, "Power");
			ADD(PF_ACT_BACK, 0, "Back");
		} else {   /* the active menu: Startup, Reignite, Smoke, Hold, Shutdown, Manual */
			if (!strcmp(mode, "Hold")) ADD(PF_ACT_SMOKE, 0, "Smoke Mode");
			else ADD(PF_ACT_HOLD, 0, "Hold Mode");
			ADD(PF_ACT_END_COOK, 0, "End Cook"); DANGER();   /* red: it stops the cook */
			ADD(PF_ACT_LIST, PF_LIST_PROBE, "Probe Target");
			ADD(PF_ACT_LIST, PF_LIST_BT, "Bluetooth Probes");
			ADD(PF_ACT_NETINFO, 0, "Network Info");
			ADD(PF_ACT_LIST, PF_LIST_SETTINGS, "Settings");
			ADD(PF_ACT_ESTOP, 0, "Emergency Stop"); DANGER();
			ADD(PF_ACT_BACK, 0, "Back");
		}
		break;
	}
#undef ADD
#undef DANGER
	return n;
}

/* --------------------------------------------------------------- pieces */

static void draw_banner(pf_gfx *g, const cJSON *s, const char *mode)
{
	int W = g->vw;
	bool tuning_fill = pf_json_bool((cJSON *)s, "tuning.running", false) || pf_json_bool((cJSON *)s, "autotune.active", false);
	uint16_t fill = tuning_fill ? g->th.info : mode_fill(g, mode), tc = on_fill_text(g, fill);
	pf_gfx_rect(g, 0, 0, g->w, 34, fill);
	/* A tuning run holds set points like any cook, so "HOLD" tells you nothing about why the pit is
	 * deliberately swinging either side of its target. Say what it is doing, and what it is aiming
	 * at, because during a run the set point is the thing that keeps changing. */
	bool tuning = tuning_fill;
	char up[24];
	if (tuning) {
		/* the run's own target, which it moves through the profile, not whatever the grill is
		 * holding at this instant: during startup those are not the same */
		double sp = pf_json_num((cJSON *)s, "tuning.setpoint", 0);
		if (sp <= 0) sp = pf_json_num((cJSON *)s, "setpoint", 0);
		int step = (int)pf_json_num((cJSON *)s, "tuning.step", 0) % 100;
		int steps = (int)pf_json_num((cJSON *)s, "tuning.steps", 0) % 100;
		int t = (int)(sp + 0.5) % 10000;
		if (sp > 0 && steps > 1) snprintf(up, sizeof up, "AUTO TUNE %d  %d/%d", t, step, steps);
		else if (sp > 0) snprintf(up, sizeof up, "AUTO TUNE %d", t);
		else snprintf(up, sizeof up, "AUTO TUNING");
	} else {
		snprintf(up, sizeof up, "%.12s", mode);
	}
	upper(up);
	int px = 24;
	while (px > 14 && pf_gfx_number_width(B, px, up) > W - 76) px -= 2;   /* leave the clock its corner */
	pf_gfx_text(g, B, px, 10, 3 + (24 - px) / 2, up, tc);
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
		uint16_t hc = hop <= 25 ? g->th.danger : g->th.ok;
		pf_gfx_text(g, B, p3, x, ly, line, hop <= 25 ? g->th.danger : g->th.text);
		ly += l3;
		/* Six pixels of bar is nothing at arm's length in daylight. Give it real height and an
		 * outline, so the level reads as a level rather than as a hairline. */
		pf_gfx_bar(g, x, ly + 2, w, 14, hop / 100.0, hc, g->th.card2);
		pf_gfx_frame(g, x, ly + 2, w, 14, g->th.line);
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
	while (big > 40 && pf_gfx_number_width(B, big, v) + 2 + pf_gfx_text_width(B, big / 2, u) > maxw) big -= 4;   /* shrink to fit */
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
	char name[12], t[8], tg[12] = "", et[8] = "", amb[16] = "", bat[8] = "";
	snprintf(name, sizeof name, "%.8s", pf_json_str((cJSON *)p, "name", "?"));
	upper(name);
	fmt_temp(t, sizeof t, tv);
	if (target > 0) snprintf(tg, sizeof tg, "%.0f" DEG, target);
	if (target > 0 && !hit && eta > 0) fmt_eta(et, sizeof et, eta);
	const cJSON *av = cJSON_GetObjectItem((cJSON *)p, "ambient");
	if (cJSON_GetObjectItem((cJSON *)p, "ambient_label")) { char a[8]; fmt_temp(a, sizeof a, av); snprintf(amb, sizeof amb, "AMB %s" DEG, a); }
	int battery = (int)pf_json_num((cJSON *)p, "battery", -1);
	if (wireless && battery >= 0) snprintf(bat, sizeof bat, "%d%%", battery % 1000);
	uint16_t tgc = filled ? g->th.accent_text : tc == alert ? alert : g->th.accent;
	uint16_t dim = filled ? g->th.accent_text : g->th.muted;
	uint16_t batc = filled ? g->th.accent_text : battery <= 20 ? g->th.danger : g->th.muted;
	if (w >= 90) {
		/* row 1: name left, Bluetooth rune + bars right
		 * row 2: temperature left; target and ETA stacked on the right
		 * row 3: ambient readout left, battery right */
		int nw = w - 12;
		if (wireless) { pf_gfx_bt_rune(g, x + w - 6 - 15 - 11, y + 4, sig_on); pf_gfx_signal(g, x + w - 6 - 15, y + 3, bars, sig_on, sig_off); nw -= 30; }
		int px = 13;
		while (px > 10 && pf_gfx_text_width(B, px, name) > nw) px--;
		pf_gfx_text(g, B, px, x + 6, y + 3, name, mc);
		pf_gfx_text(g, B, 28, x + 6, y + 15, t, tc);
		if (tg[0]) pf_gfx_text_right(g, B, 13, x + w - 6, y + 17, tg, tgc);
		if (et[0]) pf_gfx_text_right(g, B, 12, x + w - 6, y + 31, et, dim);
		if (amb[0]) pf_gfx_text(g, B, 11, x + 6, y + 47, amb, dim);
		if (bat[0]) pf_gfx_text_right(g, B, 11, x + w - 6, y + 47, bat, batc);
	} else {   /* narrow (portrait): name + bars / temperature / ambient + target / ETA + battery */
		name[6] = 0;
		if (wireless) pf_gfx_signal(g, x + w - 5 - 15, y + 3, bars, sig_on, sig_off);
		pf_gfx_text(g, B, 11, x + 5, y + 3, name, mc);
		pf_gfx_text(g, B, 24, x + 5, y + 13, t, tc);
		if (amb[0]) { memmove(amb + 1, amb + 4, strlen(amb + 4) + 1); pf_gfx_text(g, B, 10, x + 5, y + 38, amb, dim); }   /* "AMB 221°" -> "A221°": room for the target */
		if (tg[0]) pf_gfx_text_right(g, B, 11, x + w - 5, y + 37, tg, tgc);
		if (et[0]) pf_gfx_text(g, B, 10, x + 5, y + 49, et, dim);
		if (bat[0]) pf_gfx_text_right(g, B, 10, x + w - 5, y + 49, bat, batc);
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
		if (!strcmp(role, "Food") && pf_json_bool((cJSON *)p, "enabled", true) && pf_json_bool((cJSON *)p, "home", true) && !pf_json_bool((cJSON *)p, "companion", false) && nf < 3) food[nf++] = p;
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

/* a title bar shared by every screen that is not the grill itself */
static void chrome(pf_gfx *g, const char *title, const cJSON *s, uint16_t fill)
{
	pf_gfx_rect(g, 0, 0, g->w, 34, fill);
	uint16_t tc = on_fill_text(g, fill);
	pf_gfx_text(g, B, 22, 10, 5, title, tc);
	if (!s) return;
	const char *mode = pf_json_str((cJSON *)s, "mode", "Stop");
	char up[16];
	snprintf(up, sizeof up, "%.12s", mode);
	upper(up);
	pf_gfx_text_right(g, B, 18, g->vw - 10, 8, up, tc);
}

static void render_list(pf_gfx *g, const cJSON *s, const pf_ui_state *ui)
{
	int W = g->vw, H = g->vh;
	pf_menu_item items[PF_MENU_MAX];
	int n = pf_menu_build(s, ui, items, PF_MENU_MAX);
	chrome(g, "MENU", s, g->th.card2);
	int rowh = (H - 40) / (n > 0 ? n : 1);
	if (rowh > 44) rowh = 44;
	int px = rowh - 8 < 24 ? (rowh - 8 < 13 ? 13 : rowh - 8) : 24;
	int y = 38 + ((H - 40) - rowh * n) / 2;
	int sel = ui->depth > 0 ? ui->stack[ui->depth - 1].index : 0;
	if (n > 0) sel = ((sel % n) + n) % n;
	for (int i = 0; i < n; i++) {
		bool is = i == sel;
		/* Selection has to survive sunlight on a dim panel: a filled block, an outline around it in
		 * the opposite colour, and text chosen for the fill. A slightly lighter shade of grey --
		 * which is what this used to be in places -- disappears completely outdoors. */
		uint16_t rowfill = items[i].danger ? g->th.danger : g->th.accent;
		if (is) {
			pf_gfx_rrect(g, 6, y + 1, W - 12, rowh - 3, 7, rowfill);
			pf_gfx_frame(g, 6, y + 1, W - 12, rowh - 3, on_fill_text(g, rowfill));
		}
		int ty = y + (rowh - pf_gfx_line_height(B, px)) / 2;
		uint16_t c = is ? on_fill_text(g, rowfill) : items[i].danger ? g->th.danger : g->th.text;
		pf_gfx_text(g, B, px, 16, ty, items[i].label, c);
		if (items[i].right[0]) pf_gfx_text_right(g, B, px - 4 < 12 ? 12 : px - 4, W - 14, ty + 2, items[i].right, is ? c : g->th.muted);
		y += rowh;
	}
}

/* Temperature selector: the value, an action button bottom right, Back bottom left. The encoder
 * moves between those three stops and clamps at the ends; pressing the value starts editing it. */
static void render_temp(pf_gfx *g, const cJSON *s, const pf_ui_state *ui)
{
	int W = g->vw, H = g->vh;
	const char *units = pf_json_str((cJSON *)s, "units", "F");
	chrome(g, ui->temp_title, NULL, ui->temp_editing ? g->th.accent : g->th.card2);

	char v[16];
	snprintf(v, sizeof v, "%.0f", ui->temp_value);
	char unit[4] = { (char)0xC2, (char)0xB0, units[0], 0 };
	int bh = H - 34 - 46;
	int big = W >= 320 ? 96 : 76;
	int vw = pf_gfx_text_width(B, big, v), uw = pf_gfx_text_width(B, big / 3, unit);
	int left = (W - vw - 6 - uw) / 2, ty = 38 + (bh - pf_gfx_line_height(B, big)) / 2;
	bool on_value = ui->temp_focus == 0;
	if (on_value) {
		/* editing pulses the plate so it is obvious the knob now changes the number */
		uint16_t plate = ui->temp_editing ? (ui->blink ? g->th.accent : g->th.card2) : g->th.card2;
		pf_gfx_rrect(g, left - 14, 38, vw + uw + 34, bh - 4, 8, plate);
	}
	uint16_t vc = on_value && ui->temp_editing && ui->blink ? g->th.accent_text : g->th.text;
	pf_gfx_text(g, B, big, left, ty, v, vc);
	pf_gfx_text(g, B, big / 3, left + vw + 6, ty + (int)(big * 0.18), unit, on_value && ui->temp_editing && ui->blink ? g->th.accent_text : g->th.muted);

	/* the two buttons */
	int bw = (W - 18) / 2, by = H - 42;
	bool on_back = ui->temp_focus == 2, on_act = ui->temp_focus == 1;
	pf_gfx_rrect(g, 6, by, bw, 36, 7, on_back ? g->th.accent : g->th.card2);
	pf_gfx_text_center(g, B, 18, 6 + bw / 2, by + 8, "Back", on_back ? g->th.accent_text : g->th.text);
	pf_gfx_rrect(g, W - 6 - bw, by, bw, 36, 7, on_act ? g->th.ok : g->th.card2);
	pf_gfx_text_center(g, B, 18, W - 6 - bw / 2, by + 8, ui->temp_button, on_act ? g->th.accent_text : g->th.text);
}

static void render_confirm(pf_gfx *g, const pf_ui_state *ui)
{
	int W = g->vw, H = g->vh;
	uint16_t accent = ui->confirm_danger ? g->th.danger : g->th.accent;
	chrome(g, ui->confirm_danger ? "CONFIRM" : "CONFIRM", NULL, accent);
	int px = pf_gfx_text_width(B, 22, ui->confirm_text) <= W - 24 ? 22 : 17;
	pf_gfx_text_center(g, B, px, W / 2, 34 + (H - 34 - 46 - pf_gfx_line_height(B, px)) / 2, ui->confirm_text, g->th.text);
	int bw = (W - 18) / 2, by = H - 42;
	bool yes = ui->confirm_focus == 1;
	pf_gfx_rrect(g, 6, by, bw, 36, 7, yes ? g->th.card2 : g->th.accent);
	pf_gfx_text_center(g, B, 18, 6 + bw / 2, by + 8, "Cancel", yes ? g->th.text : g->th.accent_text);
	pf_gfx_rrect(g, W - 6 - bw, by, bw, 36, 7, yes ? accent : g->th.card2);
	pf_gfx_text_center(g, B, 18, W - 6 - bw / 2, by + 8, ui->confirm_yes, yes ? (ui->confirm_danger ? g->th.text : g->th.accent_text) : g->th.text);
}

/* Bluetooth scan results: name, signal bars and whether it is the make being paired. */
static void render_btscan(pf_gfx *g, const pf_ui_state *ui)
{
	int W = g->vw, H = g->vh;
	char title[28];
	snprintf(title, sizeof title, "%.20s", ui->bt_label);
	upper(title);
	chrome(g, title, NULL, g->th.info);
	if (ui->bt_scanning) {
		pf_gfx_text_center(g, B, 20, W / 2, H / 2 - 24, "Scanning...", g->th.text);
		pf_gfx_text_center(g, R, 14, W / 2, H / 2 + 4, "Take the probe out of its charger", g->th.muted);
		return;
	}
	if (ui->bt_n == 0) {
		pf_gfx_text_center(g, B, 20, W / 2, H / 2 - 24, "None found", g->th.muted);
		pf_gfx_text_center(g, R, 14, W / 2, H / 2 + 4, "Press to scan again", g->th.muted);
		return;
	}
	int rows = ui->bt_n + 1;   /* + Back */
	int rowh = (H - 40) / rows;
	if (rowh > 40) rowh = 40;
	int y = 38;
	int sel = ui->depth > 0 ? ui->stack[ui->depth - 1].index : 0;
	sel = ((sel % rows) + rows) % rows;
	for (int i = 0; i < rows; i++) {
		bool is = i == sel;
		if (is) pf_gfx_rrect(g, 6, y + 1, W - 12, rowh - 3, 7, g->th.accent);
		int ty = y + (rowh - pf_gfx_line_height(B, 18)) / 2;
		uint16_t c = is ? g->th.accent_text : g->th.text;
		if (i < ui->bt_n) {
			pf_gfx_text(g, B, 18, 14, ty, ui->bt[i].name, c);
			pf_gfx_signal(g, W - 14 - 15, y + (rowh - 13) / 2, ui->bt[i].bars, is ? c : g->th.info, g->th.line);
		} else {
			pf_gfx_text(g, B, 18, 14, ty, "Back", c);
		}
		y += rowh;
	}
}

/* Network info: a QR code for the grill's web address, plus the address and Wi-Fi in text. */
static void render_netinfo(pf_gfx *g, const cJSON *s)
{
	int W = g->vw, H = g->vh;
	pf_gfx_rect(g, 0, 0, g->w, 34, g->th.card2);
	pf_gfx_text(g, B, 22, 10, 5, "NETWORK", g->th.text);
	const char *ip = pf_json_str((cJSON *)s, "net.ip", "");
	const char *ssid = pf_json_str((cJSON *)s, "net.ssid", "");
	int port = (int)pf_json_num((cJSON *)s, "net.port", 80);
	int signal = (int)pf_json_num((cJSON *)s, "net.signal", 0);
	if (!ip[0]) {
		pf_gfx_text_center(g, B, 20, W / 2, H / 2 - 30, "No network", g->th.muted);
		pf_gfx_text_center(g, R, 15, W / 2, H / 2, "Join Wi-Fi from the setup hotspot", g->th.muted);
		return;
	}
	char url[80];
	if (port == 80) snprintf(url, sizeof url, "http://%.40s/", ip);
	else snprintf(url, sizeof url, "http://%.40s:%d/", ip, port % 100000);

	pf_qr q;
	int top = 40, bottom = H - 4;
	if (pf_qr_encode(url, &q)) {
		/* quiet zone of four modules, scaled to whatever room the panel has */
		int avail = (bottom - top) - 34;
		int scale = avail / (q.size + 8);
		if (scale < 2) scale = 2;
		int side = (q.size + 8) * scale;
		int ox = (W - side) / 2, oy = top;
		pf_gfx_rect(g, ox, oy, side, side, 0xFFFF);   /* white background: scanners need the quiet zone */
		for (int y = 0; y < q.size; y++)
			for (int x = 0; x < q.size; x++)
				if (q.m[y][x]) pf_gfx_rect(g, ox + (x + 4) * scale, oy + (y + 4) * scale, scale, scale, 0x0000);
		top = oy + side + 6;
	}
	pf_gfx_text_center(g, B, 16, W / 2, top, url, g->th.text);
	if (ssid[0]) {
		char line[64];
		snprintf(line, sizeof line, "%.24s  %d%%", ssid, signal % 1000);
		pf_gfx_text_center(g, R, 13, W / 2, top + 19, line, g->th.muted);
	}
}

/* Monitor control: the grill screen with the three outputs selectable, plus Exit. One press
 * toggles whatever is highlighted; leaving the screen turns them all off again. */
static void render_manual(pf_gfx *g, const cJSON *s, const pf_ui_state *ui)
{
	static const char *const names[3] = { "AUGER", "FAN", "IGNITER" };
	static const char *const keys[3] = { "outputs.auger", "outputs.fan", "outputs.igniter" };
	int W = g->vw, H = g->vh;
	chrome(g, "CONTROL", NULL, g->th.warn);
	const cJSON *probes = cJSON_GetObjectItem((cJSON *)s, "probes"), *p, *primary = NULL;
	cJSON_ArrayForEach(p, probes) if (!strcmp(pf_json_str((cJSON *)p, "role", ""), "Primary")) { primary = p; break; }

	/* four rows (three outputs and Exit) share whatever is left under the pit temperature */
	int gap = 4, rows = 4;
	int avail = H - 38 - 4;
	int rowh = (avail - 52) / rows - gap;
	if (rowh > 34) rowh = 34;
	if (rowh < 20) rowh = 20;
	int block = rows * (rowh + gap);
	int pit_h = avail - block;
	int big = pit_h > 58 ? 52 : pit_h - 6;
	if (big < 26) big = 26;
	draw_pit(g, primary, pf_json_str((cJSON *)s, "units", "F"), "Monitor", 8, 38, big, W - 16);

	int y = H - block - 2;
	for (int i = 0; i < 3; i++) {
		bool on = pf_json_bool((cJSON *)s, keys[i], false);
		bool is = ui->manual_focus == i;
		uint16_t fill = on ? (i == 0 ? g->th.auger : i == 1 ? g->th.fan : g->th.igniter) : g->th.card2;
		pf_gfx_rrect(g, 6, y, W - 12, rowh, 6, fill);
		if (is) pf_gfx_frame(g, 6, y, W - 12, rowh, g->th.text);
		uint16_t tc = on ? g->th.accent_text : is ? g->th.text : g->th.muted;
		int px = rowh >= 30 ? 18 : 15;
		pf_gfx_text(g, B, px, 14, y + (rowh - pf_gfx_line_height(B, px)) / 2, names[i], tc);
		pf_gfx_text_right(g, B, px - 2, W - 14, y + (rowh - pf_gfx_line_height(B, px - 2)) / 2 + 1, on ? "ON" : "OFF", tc);
		y += rowh + gap;
	}
	bool is_exit = ui->manual_focus == 3;
	int px = rowh >= 30 ? 18 : 15;
	pf_gfx_rrect(g, 6, y, W - 12, rowh, 6, is_exit ? g->th.accent : g->th.card2);
	pf_gfx_text_center(g, B, px, W / 2, y + (rowh - pf_gfx_line_height(B, px)) / 2, "Exit", is_exit ? g->th.accent_text : g->th.text);
}

/* The margin editor.
 *
 * Margins exist because a bezel hides the edge of the panel, and how much it hides is something you
 * can only see by looking at it. So the whole drawable area is outlined: turn the knob and the
 * outline moves under the bezel until it sits just inside it. The numbers are there, but they are
 * not the point; the frame is. */
static void render_margins(pf_gfx *g, const pf_ui_state *ui)
{
	static const char *const EDGE[4] = { "Top", "Right", "Bottom", "Left" };
	int W = g->vw, H = g->vh;

	/* the outline of what can be drawn: this is the thing being adjusted */
	pf_gfx_frame(g, 0, 0, W, H, g->th.accent);
	pf_gfx_frame(g, 1, 1, W - 2, H - 2, g->th.accent);

	pf_gfx_text_center(g, B, 17, W / 2, 8, "SCREEN MARGINS", g->th.muted);
	pf_gfx_text_center(g, R, 13, W / 2, 28, "Frame just inside the bezel", g->th.muted);

	/* each edge's number sits against the edge it controls, so there is nothing to decode */
	struct { int x, y; } at[4] = { { W / 2, 46 }, { W - 34, H / 2 - 8 }, { W / 2, H - 46 }, { 22, H / 2 - 8 } };
	for (int e = 0; e < 4; e++) {
		bool sel = ui->margin_focus == e;
		char v[8];
		snprintf(v, sizeof v, "%d", ui->margin[e]);
		int bw = 40, bh = 26, bx = at[e].x - bw / 2, by = at[e].y - 4;
		/* Editing: filled accent, so there is no doubt the knob is changing this one. Merely
		 * selected: a thick accent outline rather than a slightly lighter grey, which on a dim
		 * panel in daylight is indistinguishable from not selected at all. */
		uint16_t tc;
		if (sel && ui->margin_editing) {
			pf_gfx_rrect(g, bx, by, bw, bh, 6, g->th.accent);
			tc = on_fill_text(g, g->th.accent);
		} else if (sel) {
			pf_gfx_rrect(g, bx, by, bw, bh, 6, g->th.card2);
			pf_gfx_frame(g, bx, by, bw, bh, g->th.accent);
			pf_gfx_frame(g, bx + 1, by + 1, bw - 2, bh - 2, g->th.accent);
			tc = g->th.text;
		} else {
			pf_gfx_rrect(g, bx, by, bw, bh, 6, g->th.card2);
			tc = g->th.muted;
		}
		pf_gfx_text_center(g, B, 18, at[e].x, by + 4, v, tc);
		pf_gfx_text_center(g, R, 11, at[e].x, by + bh, EDGE[e], sel ? g->th.text : g->th.muted);
	}

	/* Save and Back, focused after the four edges */
	/* Back on the left, Save on the right: the dismissive action is always the one you reach first
	 * going backwards, and the committing one is always on the right. See docs/design-language.md. */
	static const char *const BTN[2] = { "Back", "Save" };
	for (int b2 = 0; b2 < 2; b2++) {
		bool sel = ui->margin_focus == 4 + b2;
		int bw = 62, bh = 26, bx = W / 2 - 66 + b2 * 70, by = H / 2 - 6;
		pf_gfx_rrect(g, bx, by, bw, bh, 6, sel ? g->th.accent : g->th.card2);
		if (sel) pf_gfx_frame(g, bx, by, bw, bh, on_fill_text(g, g->th.accent));
		pf_gfx_text_center(g, B, 14, bx + bw / 2, by + (bh - pf_gfx_line_height(B, 14)) / 2,
		                   BTN[b2], sel ? g->th.accent_text : g->th.text);
	}
	if (ui->margin_dirty) pf_gfx_text_center(g, R, 11, W / 2, H / 2 + 24, "unsaved", g->th.warn);
}

void pf_screens_render(pf_gfx *g, const cJSON *status, const pf_ui_state *ui)
{
	pf_gfx_clear(g, g->th.bg);
	pf_screen scr = pf_nav_screen(ui);
	if (scr == PF_SCR_MESSAGE) {
		int w = g->vw - 24, h = 72;
		pf_gfx_rrect(g, 12, g->vh / 2 - h / 2, w, h, 8, g->th.card2);
		int px = pf_gfx_text_width(B, 20, ui->message) <= w - 24 ? 20 : 15;
		pf_gfx_text_center(g, B, px, g->vw / 2, g->vh / 2 - pf_gfx_line_height(B, px) / 2, ui->message, g->th.text);
		return;
	}
	if (scr == PF_SCR_CONFIRM) { render_confirm(g, ui); return; }
	if (scr == PF_SCR_BTSCAN) { render_btscan(g, ui); return; }
	if (scr == PF_SCR_MARGINS) { render_margins(g, ui); return; }
	if (status) {
		if (scr == PF_SCR_LIST) { render_list(g, status, ui); return; }
		if (scr == PF_SCR_TEMP) { render_temp(g, status, ui); return; }
		if (scr == PF_SCR_NETINFO) { render_netinfo(g, status); return; }
		if (scr == PF_SCR_MANUAL) { render_manual(g, status, ui); return; }
		render_main(g, status, ui);
		return;
	}
	pf_gfx_text_center(g, B, 32, g->vw / 2, g->vh / 2 - 20, "PiFire", g->th.accent);
}
