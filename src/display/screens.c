#include "display/screens.h"
#include "core/settings.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

const char *const pf_menu_items[PF_MENU_COUNT] = { "Start (Smoke)", "Hold at...", "Smoke", "Smoke+ toggle", "Shutdown", "Stop", "Back" };

static void fmt_temp(char *out, size_t n, const cJSON *v)
{
	if (cJSON_IsNumber(v)) snprintf(out, n, "%.0f", v->valuedouble); else snprintf(out, n, "--");
}

static void render_main(pf_gfx *g, const cJSON *s)
{
	const char *mode = pf_json_str((cJSON *)s, "mode", "Stop");
	const char *units = pf_json_str((cJSON *)s, "units", "F");
	int W = g->w, H = g->h;
	int scale = W >= 320 ? 1 : 1;
	/* top bar: mode + set point */
	uint16_t mode_c = !strcmp(mode, "Hold") || !strcmp(mode, "Smoke") ? PF_C_OK : !strcmp(mode, "Error") ? PF_C_DANGER :
	                  !strcmp(mode, "Stop") || !strcmp(mode, "Monitor") ? PF_C_MUTED : PF_C_ACCENT;
	pf_gfx_rect(g, 0, 0, W, 22, PF_C_CARD);
	char up[24];
	snprintf(up, sizeof up, "%s", mode);
	for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
	pf_gfx_text(g, 8, 3, up, 2 * scale, mode_c);
	if (!strcmp(mode, "Hold") || !strcmp(mode, "Startup") || !strcmp(mode, "Reignite")) {
		char sp[24];
		snprintf(sp, sizeof sp, "SET %.0f%c", pf_json_num((cJSON *)s, "setpoint", 0), units[0]);
		pf_gfx_text_right(g, W - 8, 3, sp, 2 * scale, PF_C_TEXT);
	} else if (!strcmp(mode, "Startup") || !strcmp(mode, "Shutdown")) {
		double el = pf_json_num((cJSON *)s, "mode_elapsed", 0);
		double dur = pf_json_num((cJSON *)s, !strcmp(mode, "Startup") ? "timers.startup_duration" : "timers.shutdown_duration", 0);
		char t[16];
		int rem = (int)fmax(0, dur - el);
		snprintf(t, sizeof t, "%d:%02d", rem / 60, rem % 60);
		pf_gfx_text_right(g, W - 8, 3, t, 2 * scale, PF_C_TEXT);
	}

	/* pit temperature, big */
	const cJSON *probes = cJSON_GetObjectItem((cJSON *)s, "probes");
	const cJSON *primary = NULL, *p;
	cJSON_ArrayForEach(p, probes) if (!strcmp(pf_json_str((cJSON *)p, "role", ""), "Primary")) { primary = p; break; }
	char big[16];
	fmt_temp(big, sizeof big, primary ? cJSON_GetObjectItem((cJSON *)primary, "temp") : NULL);
	int big_scale = W >= 320 ? 7 : 5;
	int bw = pf_gfx_text_width(big, big_scale);
	int bx = (W - bw - 16) / 2, by = 36;
	pf_gfx_text(g, bx, by, big, big_scale, PF_C_TEXT);
	char deg[4] = { (char)0xB0, units[0], 0, 0 };
	pf_gfx_text(g, bx + bw + 2, by, deg, 2, PF_C_MUTED);
	pf_gfx_text_center(g, W / 2, by + big_scale * 8 + 4, "PIT", 1, PF_C_MUTED);

	/* food probes: up to 3 in a row */
	int y = by + big_scale * 8 + 22;
	int n = 0;
	cJSON_ArrayForEach(p, probes) if (strcmp(pf_json_str((cJSON *)p, "role", ""), "Primary") && pf_json_bool((cJSON *)p, "enabled", true) && strcmp(pf_json_str((cJSON *)p, "role", ""), "Aux")) n++;
	if (n > 3) n = 3;
	int cw = n ? (W - 16) / n : 0, i = 0;
	cJSON_ArrayForEach(p, probes) {
		if (!strcmp(pf_json_str((cJSON *)p, "role", ""), "Primary") || !strcmp(pf_json_str((cJSON *)p, "role", ""), "Aux") || !pf_json_bool((cJSON *)p, "enabled", true) || i >= 3) continue;
		int x = 8 + i * cw;
		pf_gfx_rect(g, x, y, cw - 4, 44, PF_C_CARD);
		char name[12];
		snprintf(name, sizeof name, "%.7s", pf_json_str((cJSON *)p, "name", "?"));
		pf_gfx_text(g, x + 4, y + 4, name, 1, PF_C_MUTED);
		char t[16];
		fmt_temp(t, sizeof t, cJSON_GetObjectItem((cJSON *)p, "temp"));
		double target = pf_json_num((cJSON *)p, "target", 0);
		uint16_t col = target > 0 && cJSON_IsNumber(cJSON_GetObjectItem((cJSON *)p, "temp")) && cJSON_GetObjectItem((cJSON *)p, "temp")->valuedouble >= target ? PF_C_OK : PF_C_TEXT;
		pf_gfx_text(g, x + 4, y + 16, t, 3, col);
		if (target > 0) { char tg[12]; snprintf(tg, sizeof tg, ">%.0f", target); pf_gfx_text_right(g, x + cw - 8, y + 4, tg, 1, PF_C_ACCENT); }
		i++;
	}
	/* pit vs set point bar while holding */
	if (!strcmp(mode, "Hold") && primary && cJSON_IsNumber(cJSON_GetObjectItem((cJSON *)primary, "temp"))) {
		double sp = pf_json_num((cJSON *)s, "setpoint", 0), pit = cJSON_GetObjectItem((cJSON *)primary, "temp")->valuedouble;
		int barw = W - 16, bary = y + 56;
		double frac = sp > 0 ? pit / sp : 0;
		pf_gfx_bar(g, 8, bary, barw, 6, frac > 1 ? 1 : frac, fabs(pit - sp) <= 5 ? PF_C_OK : PF_C_ACCENT, PF_C_CARD);
		pf_gfx_rect(g, 8 + barw - 1, bary - 2, 2, 10, PF_C_TEXT);
	}

	/* bottom: outputs + feed */
	int oy = H - 20;
	pf_gfx_rect(g, 0, oy - 4, W, 24, PF_C_CARD);
	const char *outs[] = { "PWR", "FAN", "AUG", "IGN" };
	const char *keys[] = { "outputs.power", "outputs.fan", "outputs.auger", "outputs.igniter" };
	int ox = 8;
	for (int k = 0; k < 4; k++) {
		bool on = pf_json_bool((cJSON *)s, keys[k], false);
		pf_gfx_rect(g, ox, oy + 2, 8, 8, on ? (k >= 2 ? PF_C_ACCENT : PF_C_OK) : PF_C_BG);
		pf_gfx_text(g, ox + 12, oy + 1, outs[k], 1, on ? PF_C_TEXT : PF_C_MUTED);
		ox += 52;
	}
	if (!strcmp(mode, "Hold")) {
		char fd[16];
		snprintf(fd, sizeof fd, "FEED %.0f%%", pf_json_num((cJSON *)s, "cycle.u_applied", 0) * 100);
		pf_gfx_text_right(g, W - 8, oy + 1, fd, 1, PF_C_TEXT);
	}
	int hop = (int)pf_json_num((cJSON *)s, "hopper_pct", -1);
	if (hop >= 0 && strcmp(mode, "Hold")) { char hs[24]; snprintf(hs, sizeof hs, "HOPPER %d%%", hop % 1000); pf_gfx_text_right(g, W - 8, oy + 1, hs, 1, hop <= 25 ? PF_C_WARN : PF_C_TEXT); }

	const char *err = pf_json_str((cJSON *)s, "safety.error_code", "");
	if (err[0]) { pf_gfx_rect(g, 0, 24, W, 12, PF_C_DANGER); pf_gfx_text_center(g, W / 2, 26, err, 1, PF_C_TEXT); }
	if (pf_json_bool((cJSON *)s, "lid_open", false)) pf_gfx_text_center(g, W / 2, 24, "LID OPEN - FEED PAUSED", 1, PF_C_WARN);
}

static void render_menu(pf_gfx *g, const pf_ui_state *ui)
{
	int W = g->w;
	pf_gfx_rect(g, 0, 0, W, 22, PF_C_CARD);
	pf_gfx_text(g, 8, 3, "MENU", 2, PF_C_ACCENT);
	pf_gfx_text_right(g, W - 8, 6, "turn: move  press: select", 1, PF_C_MUTED);
	int y = 30, rowh = 26;
	for (int i = 0; i < PF_MENU_COUNT; i++) {
		bool sel = i == ui->menu_index;
		if (sel) pf_gfx_rect(g, 4, y - 3, W - 8, rowh - 2, PF_C_ACCENT);
		pf_gfx_text(g, 14, y + 1, pf_menu_items[i], 2, sel ? PF_RGB(0x1A, 0x10, 0x02) : PF_C_TEXT);
		y += rowh;
	}
}

static void render_setpoint(pf_gfx *g, const pf_ui_state *ui, const cJSON *s)
{
	int W = g->w, H = g->h;
	const char *units = pf_json_str((cJSON *)s, "units", "F");
	pf_gfx_rect(g, 0, 0, W, 22, PF_C_CARD);
	pf_gfx_text(g, 8, 3, "HOLD AT", 2, PF_C_ACCENT);
	pf_gfx_text_right(g, W - 8, 6, "turn: adjust  press: go", 1, PF_C_MUTED);
	char v[16];
	snprintf(v, sizeof v, "%.0f", ui->edit_setpoint);
	int sc = 8;
	int w = pf_gfx_text_width(v, sc);
	pf_gfx_text(g, (W - w) / 2 - 8, H / 2 - sc * 4, v, sc, PF_C_TEXT);
	char deg[4] = { (char)0xB0, units[0], 0, 0 };
	pf_gfx_text(g, (W + w) / 2 - 4, H / 2 - sc * 4, deg, 2, PF_C_MUTED);
	pf_gfx_text_center(g, W / 2, H - 30, "long press: cancel", 1, PF_C_MUTED);
}

void pf_screens_render(pf_gfx *g, const cJSON *status, const pf_ui_state *ui)
{
	pf_gfx_clear(g, PF_C_BG);
	if (ui->screen == PF_SCR_MENU) { render_menu(g, ui); return; }
	if (ui->screen == PF_SCR_SETPOINT) { render_setpoint(g, ui, status); return; }
	if (ui->screen == PF_SCR_MESSAGE) { pf_gfx_text_center(g, g->w / 2, g->h / 2 - 8, ui->message, 2, PF_C_TEXT); return; }
	if (status) render_main(g, status);
	else pf_gfx_text_center(g, g->w / 2, g->h / 2 - 8, "PiFire", 3, PF_C_ACCENT);
}
