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

void pf_nav_reset(pf_ui_state *ui) { ui->depth = 0; ui->main_focus = PF_FOCUS_NONE; }

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

/* the same table the phone's picker offers: the doneness, how early it comes off, the step alerts */
static const pf_doneness D_BEEF[] = { { "Rare", 125, 5, { "Flip" }, { 100 } }, { "Medium rare", 135, 5, { "Flip" }, { 110 } }, { "Medium", 140, 5, { "Flip" }, { 115 } },
                                      { "Medium well", 150, 5, { "Flip" }, { 125 } }, { "Well done", 160, 5, { "Flip" }, { 135 } } };
static const pf_doneness D_BRISKET[] = { { "Probe tender", 203, 0, { "Spritz", "Wrap" }, { 150, 165 } } };
static const pf_doneness D_PORK[] = { { "Chops and loin", 145, 4, { "Flip" }, { 120 } }, { "Pulled pork", 203, 0, { "Spritz", "Wrap" }, { 150, 165 } } };
static const pf_doneness D_RIBS[] = { { "Bend test", 195, 0, { "Spritz", "Wrap", "Unwrap" }, { 150, 165, 185 } } };
static const pf_doneness D_CHICKEN[] = { { "Breast 155", 155, 0, { "Flip" }, { 135 } }, { "Breast 165", 165, 0, { "Flip" }, { 145 } }, { "Thighs", 175, 0, { "Flip" }, { 155 } } };
static const pf_doneness D_TURKEY[] = { { "Whole bird", 165, 0, { "Baste" }, { 145 } } };
static const pf_doneness D_FISH[] = { { "Flaky", 145, 0, { 0 }, { 0 } } };
static const pf_doneness D_LAMB[] = { { "Medium rare", 135, 4, { "Flip" }, { 110 } }, { "Medium", 140, 4, { "Flip" }, { 115 } } };
static const pf_doneness D_SAUSAGE[] = { { "Cooked through", 160, 0, { "Flip" }, { 140 } } };
#define MEAT(n, d) { n, d, (int)(sizeof d / sizeof d[0]) }
const pf_meat PF_MEATS[] = { MEAT("Beef", D_BEEF), MEAT("Brisket", D_BRISKET), MEAT("Pork", D_PORK), MEAT("Ribs", D_RIBS), MEAT("Chicken", D_CHICKEN),
                             MEAT("Turkey", D_TURKEY), MEAT("Fish", D_FISH), MEAT("Lamb", D_LAMB), MEAT("Sausage", D_SAUSAGE) };
const int PF_MEAT_COUNT = (int)(sizeof PF_MEATS / sizeof PF_MEATS[0]);

/* the food probes the main screen shows, in card order: the same rule render_main uses */
static int main_food(const cJSON *status, const cJSON **food, int max)
{
	const cJSON *probes = cJSON_GetObjectItem((cJSON *)status, "probes"), *p;
	int nf = 0;
	cJSON_ArrayForEach(p, probes) {
		if (nf >= max) break;
		if (!strcmp(pf_json_str((cJSON *)p, "role", ""), "Food") && pf_json_bool((cJSON *)p, "enabled", true) && pf_json_bool((cJSON *)p, "home", true) && !pf_json_bool((cJSON *)p, "companion", false)) food[nf++] = p;
	}
	return nf;
}

int pf_main_probe_index(const cJSON *status, int card)
{
	const cJSON *food[3];
	int nf = main_food(status, food, 3);
	if (card < 0 || card >= nf) return -1;
	const cJSON *probes = cJSON_GetObjectItem((cJSON *)status, "probes"), *p;
	int i = 0;
	cJSON_ArrayForEach(p, probes) { if (p == food[card]) return i; i++; }
	return -1;
}

int pf_main_focus_step(const cJSON *status, int cur, int dir)
{
	const cJSON *food[3];
	int nf = main_food(status, food, 3);
	bool hopper = pf_json_num((cJSON *)status, "hopper_pct", -1) >= 0;
	bool timer = pf_json_bool((cJSON *)status, "timer.running", false);
	int n = PF_FOCUS_PROBE0 + nf;
	if (cur < 0) return PF_FOCUS_SETPOINT;
	for (int k = 0; k < n; k++) {
		cur = ((cur + (dir > 0 ? 1 : -1)) % n + n) % n;
		if (cur == PF_FOCUS_HOPPER && !hopper) continue;
		if (cur == PF_FOCUS_TIMER && !timer) continue;
		return cur;
	}
	return PF_FOCUS_SETPOINT;
}

/* ---- the marks on menu rows: lines, discs and arcs, nothing typed ---- */
static void draw_icon(pf_gfx *g, pf_icon ic, int x, int y, uint16_t c)
{
	/* a 16-pixel box with its top-left at (x, y) */
	double cx = x + 8, cy = y + 8, t = 1.8;
	switch (ic) {
	case PF_ICON_PLAY:
		pf_gfx_line(g, x + 4, y + 2, x + 4, y + 14, t, c); pf_gfx_line(g, x + 4, y + 2, x + 14, y + 8, t, c); pf_gfx_line(g, x + 4, y + 14, x + 14, y + 8, t, c); break;
	case PF_ICON_HOLD:
		pf_gfx_arc(g, (int)cx, (int)cy, 5, 7, 0, 360, c);
		pf_gfx_line(g, cx, y + 0.5, cx, y + 4, t, c); pf_gfx_line(g, cx, y + 12, cx, y + 15.5, t, c);
		pf_gfx_line(g, x + 0.5, cy, x + 4, cy, t, c); pf_gfx_line(g, x + 12, cy, x + 15.5, cy, t, c); break;
	case PF_ICON_SMOKE:
		pf_gfx_disc(g, x + 5, y + 10, 4, c); pf_gfx_disc(g, x + 9, y + 7, 5, c); pf_gfx_disc(g, x + 12, y + 10, 3, c);
		pf_gfx_rect(g, x + 4, y + 10, 10, 4, c); break;
	case PF_ICON_STOP:
		pf_gfx_rrect(g, x + 2, y + 2, 12, 12, 2, c); break;
	case PF_ICON_POWER:
		pf_gfx_arc(g, (int)cx, (int)cy + 1, 5, 7, 300, 600, c); pf_gfx_line(g, cx, y + 1, cx, y + 8, t, c); break;   /* degrees, 0 = right, 90 = down */
	case PF_ICON_TIMER:
		pf_gfx_arc(g, (int)cx, (int)cy, 5, 7, 0, 360, c); pf_gfx_line(g, cx, cy, cx, cy - 4, t, c); pf_gfx_line(g, cx, cy, cx + 3, cy, t, c); break;
	case PF_ICON_PROBE:
		pf_gfx_line(g, x + 8, y + 2, x + 8, y + 9, 3.2, c); pf_gfx_disc(g, x + 8, y + 12, 3, c); break;
	case PF_ICON_GEAR:
		pf_gfx_arc(g, (int)cx, (int)cy, 3, 6, 0, 360, c);
		for (int i = 0; i < 8; i++) { double a = i * 0.7854; pf_gfx_line(g, cx + 5 * cos(a), cy + 5 * sin(a), cx + 7.5 * cos(a), cy + 7.5 * sin(a), 2.2, c); } break;
	case PF_ICON_WIFI:
		pf_gfx_arc(g, (int)cx, y + 15, 10, 12, 225, 315, c); pf_gfx_arc(g, (int)cx, y + 15, 5, 7, 220, 320, c); pf_gfx_disc(g, (int)cx, y + 14, 2, c); break;
	case PF_ICON_BACK:
		pf_gfx_line(g, x + 3, cy, x + 14, cy, t, c); pf_gfx_line(g, x + 3, cy, x + 8, cy - 5, t, c); pf_gfx_line(g, x + 3, cy, x + 8, cy + 5, t, c); break;
	case PF_ICON_EYE:
		pf_gfx_arc(g, (int)cx, y + 13, 9, 11, 215, 325, c); pf_gfx_arc(g, (int)cx, y + 3, 9, 11, 35, 145, c); pf_gfx_disc(g, (int)cx, (int)cy, 2.5, c); break;
	case PF_ICON_SLIDERS:
		pf_gfx_line(g, x + 2, y + 4, x + 14, y + 4, t, c); pf_gfx_line(g, x + 2, y + 12, x + 14, y + 12, t, c);
		pf_gfx_disc(g, x + 6, y + 4, 2.4, c); pf_gfx_disc(g, x + 11, y + 12, 2.4, c); break;
	case PF_ICON_BT:
		pf_gfx_bt_rune(g, x + 4, y + 2, c); break;
	case PF_ICON_NEXT:
		pf_gfx_line(g, x + 3, y + 3, x + 8, cy, t, c); pf_gfx_line(g, x + 8, cy, x + 3, y + 13, t, c);
		pf_gfx_line(g, x + 9, y + 3, x + 14, cy, t, c); pf_gfx_line(g, x + 14, cy, x + 9, y + 13, t, c); break;
	case PF_ICON_PREV:
		pf_gfx_line(g, x + 8, y + 3, x + 3, cy, t, c); pf_gfx_line(g, x + 3, cy, x + 8, y + 13, t, c);
		pf_gfx_line(g, x + 14, y + 3, x + 9, cy, t, c); pf_gfx_line(g, x + 9, cy, x + 14, y + 13, t, c); break;
	case PF_ICON_EXIT:
		pf_gfx_line(g, x + 3, y + 3, x + 13, y + 13, t, c); pf_gfx_line(g, x + 13, y + 3, x + 3, y + 13, t, c); break;
	case PF_ICON_HOPPER:
		pf_gfx_line(g, x + 2, y + 3, x + 4, y + 14, t, c); pf_gfx_line(g, x + 14, y + 3, x + 12, y + 14, t, c);
		pf_gfx_line(g, x + 2, y + 3, x + 14, y + 3, t, c); pf_gfx_line(g, x + 4, y + 14, x + 12, y + 14, t, c);
		pf_gfx_rect(g, x + 5, y + 9, 6, 4, c); break;
	case PF_ICON_CHECK:
		pf_gfx_line(g, x + 3, cy, x + 7, y + 13, t, c); pf_gfx_line(g, x + 7, y + 13, x + 14, y + 3, t, c); break;
	case PF_ICON_FLAG:
		/* a pole and a flag of six checks */
		pf_gfx_rect(g, x + 2, y + 1, 2, 15, c);
		for (int r = 0; r < 2; r++) {
			for (int k = 0; k < 3; k++) { if ((r + k) % 2 == 0) pf_gfx_rect(g, x + 4 + k * 4, y + 2 + r * 4, 4, 4, c); }
		}
		pf_gfx_rect(g, x + 4, y + 2, 12, 1, c); pf_gfx_rect(g, x + 4, y + 9, 12, 1, c); pf_gfx_rect(g, x + 15, y + 2, 1, 8, c); break;
	case PF_ICON_STEAK:
		/* a slab with the bone's eye */
		pf_gfx_rrect(g, x + 1, y + 4, 14, 9, 4, c); pf_gfx_disc(g, x + 5, y + 8, 2, g->th.bg); break;
	case PF_ICON_BRISKET:
		pf_gfx_rrect(g, x + 1, y + 5, 14, 8, 3, c); pf_gfx_line(g, x + 3, y + 9, x + 13, y + 7, 1.2, g->th.bg); break;
	case PF_ICON_PORK:
		/* a pig's face: ears, snout */
		pf_gfx_disc(g, (int)cx, (int)cy + 1, 6, c); pf_gfx_disc(g, x + 4, y + 4, 2, c); pf_gfx_disc(g, x + 12, y + 4, 2, c);
		pf_gfx_rrect(g, x + 5, y + 9, 6, 4, 2, g->th.bg); pf_gfx_disc(g, x + 7, y + 11, 1, c); pf_gfx_disc(g, x + 9, y + 11, 1, c); break;
	case PF_ICON_RIBS:
		/* a rack: the bones under the arc */
		pf_gfx_arc(g, (int)cx, y + 14, 9, 11, 200, 340, c);
		for (int k = 0; k < 4; k++) { pf_gfx_rect(g, x + 2 + k * 4, y + 8, 2, 7, c); }
		break;
	case PF_ICON_CHICKEN:
		/* a drumstick: the meat, the bone, the knuckle */
		pf_gfx_disc(g, x + 6, y + 6, 5, c); pf_gfx_line(g, x + 8, y + 8, x + 14, y + 14, 2.4, c); pf_gfx_disc(g, x + 14, y + 14, 1.8, c); break;
	case PF_ICON_TURKEY:
		/* the whole bird: body and the two legs */
		pf_gfx_disc(g, (int)cx, y + 7, 6, c); pf_gfx_line(g, x + 5, y + 11, x + 3, y + 15, 2.2, c); pf_gfx_line(g, x + 11, y + 11, x + 13, y + 15, 2.2, c); break;
	case PF_ICON_FISH:
		/* body, tail, eye */
		pf_gfx_disc(g, x + 6, (int)cy, 5, c); pf_gfx_rect(g, x + 6, y + 5, 5, 6, c);
		pf_gfx_line(g, x + 11, cy, x + 15, y + 3, 2.2, c); pf_gfx_line(g, x + 11, cy, x + 15, y + 13, 2.2, c); pf_gfx_disc(g, x + 4, y + 7, 1, g->th.bg); break;
	case PF_ICON_LAMB:
		/* a chop: the meat with its bone */
		pf_gfx_disc(g, x + 6, y + 7, 5, c); pf_gfx_line(g, x + 9, y + 10, x + 14, y + 15, 2.4, c); break;
	case PF_ICON_SAUSAGE:
		/* a link, curved */
		pf_gfx_arc(g, (int)cx, y + 16, 7, 11, 210, 330, c); break;
	default: break;
	}
}

static pf_icon icon_for(pf_action act, int arg)
{
	switch (act) {
	case PF_ACT_STARTUP: case PF_ACT_STARTUP_HOLD: case PF_ACT_STARTUP_SMOKE: return PF_ICON_PLAY;
	case PF_ACT_HOLD: return PF_ICON_HOLD;
	case PF_ACT_SMOKE: return PF_ICON_SMOKE;
	case PF_ACT_STOP: case PF_ACT_STOP_GRILL: case PF_ACT_ESTOP: return PF_ICON_STOP;
	case PF_ACT_END_COOK: return PF_ICON_FLAG;
	case PF_ACT_RESTART: case PF_ACT_POWEROFF: return PF_ICON_POWER;
	case PF_ACT_TIMER: case PF_ACT_TIMER_CANCEL: case PF_ACT_TIMER_CHANGE: return PF_ICON_TIMER;
	case PF_ACT_PROBE_TARGET: case PF_ACT_PROBE_PICK: case PF_ACT_PROBE_CUSTOM: case PF_ACT_PROBE_CLEAR: case PF_ACT_DONE: return PF_ICON_PROBE;
	case PF_ACT_MEAT: return arg >= 0 && arg < 9 ? (pf_icon)(PF_ICON_STEAK + arg) : PF_ICON_PROBE;
	case PF_ACT_MARGINS: case PF_ACT_THEME: case PF_ACT_COLOUR: return PF_ICON_GEAR;
	case PF_ACT_NETINFO: return PF_ICON_WIFI;
	case PF_ACT_BACK: return PF_ICON_BACK;
	case PF_ACT_MONITOR: return PF_ICON_EYE;
	case PF_ACT_MANUAL: return PF_ICON_SLIDERS;
	case PF_ACT_BT_SCAN: case PF_ACT_BT_ADD: case PF_ACT_BT_TOGGLE: case PF_ACT_BT_DELETE: return PF_ICON_BT;
	case PF_ACT_RECIPE_NEXT: case PF_ACT_RECIPE_SKIP: return PF_ICON_NEXT;
	case PF_ACT_RECIPE_BACK: return PF_ICON_PREV;
	case PF_ACT_RECIPE_EXIT: return PF_ICON_EXIT;
	case PF_ACT_HOPPER_FULL: case PF_ACT_HOPPER_EMPTY: return PF_ICON_HOPPER;
	case PF_ACT_CLEAR_ERROR: return PF_ICON_CHECK;
	case PF_ACT_LIST:
		return arg == PF_LIST_STARTUP ? PF_ICON_PLAY : arg == PF_LIST_PROBE ? PF_ICON_PROBE : arg == PF_LIST_SETTINGS ? PF_ICON_GEAR
		     : arg == PF_LIST_POWER ? PF_ICON_POWER : arg == PF_LIST_BT || arg == PF_LIST_BTKIND || arg == PF_LIST_BTEDIT || arg == PF_LIST_BTDEL ? PF_ICON_BT
		     : arg == PF_LIST_HOPPER ? PF_ICON_HOPPER : arg == PF_LIST_TIMER ? PF_ICON_TIMER : arg == PF_LIST_STOP ? PF_ICON_FLAG : PF_ICON_NONE;
	default: return PF_ICON_NONE;
	}
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
		ADD(PF_ACT_LIST, PF_LIST_BT, "Bluetooth Probes");
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

	case PF_LIST_MODE:
		/* from the banner: where the grill can go from here, and Stop */
		if (!strcmp(mode, "Error")) { ADD(PF_ACT_CLEAR_ERROR, 0, "Clear Error"); }
		else if (!strcmp(mode, "Stop") || !strcmp(mode, "Monitor") || !strcmp(mode, "Prime")) {
			ADD(PF_ACT_STARTUP_HOLD, 0, "Startup To Hold");
			ADD(PF_ACT_STARTUP_SMOKE, 0, "Startup To Smoke");
			if (strcmp(mode, "Monitor")) ADD(PF_ACT_MONITOR, 0, "Monitor");
			if (strcmp(mode, "Stop")) { ADD(PF_ACT_STOP, 0, "Stop"); DANGER(); }
		} else {
			if (!strcmp(mode, "Hold")) ADD(PF_ACT_SMOKE, 0, "Smoke Mode");
			else ADD(PF_ACT_HOLD, 0, "Hold Mode");
			if (strcmp(mode, "Shutdown")) ADD(PF_ACT_END_COOK, 0, "Shutdown");
			ADD(PF_ACT_STOP, 0, "Stop"); DANGER();
		}
		ADD(PF_ACT_BACK, 0, "Back");
		break;

	case PF_LIST_STOP:
		/* Finish asks which kind: the normal shutdown under the flag, the emergency stop under the
		 * square. There is no plain Stop row in the menu; Finish is the way a cook ends. */
		ADD(PF_ACT_END_COOK, 0, "Shutdown");
		ADD(PF_ACT_STOP_GRILL, 0, "Emergency Stop"); DANGER();
		ADD(PF_ACT_BACK, 0, "Back");
		break;

	case PF_LIST_TIMER:
		ADD(PF_ACT_TIMER_CHANGE, 0, "Change Time");
		ADD(PF_ACT_TIMER_CANCEL, 0, "Cancel Timer"); DANGER();
		ADD(PF_ACT_BACK, 0, "Back");
		break;

	case PF_LIST_HOPPER:
		/* the level in the hopper right now becomes one end of the scale */
		ADD(PF_ACT_HOPPER_FULL, 0, "Set Full Here");
		ADD(PF_ACT_HOPPER_EMPTY, 0, "Set Empty Here");
		ADD(PF_ACT_BACK, 0, "Back");
		break;

	case PF_LIST_PROBE_ACT: {
		/* a probe that has a target: what its sheet on the phone offers */
		const cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItem((cJSON *)status, "probes"), ui->probe_idx);
		ADD(PF_ACT_PROBE_PICK, 0, "Change Target");
		if (p) {
			const char *meat = pf_json_str((cJSON *)p, "meat", ""), *done = pf_json_str((cJSON *)p, "done", "");
			if (meat[0]) snprintf(out[n - 1].right, sizeof out[n - 1].right, "%.10s", done[0] ? done : meat);
		}
		ADD(PF_ACT_PROBE_CUSTOM, 0, "Custom Temperature");
		ADD(PF_ACT_PROBE_CLEAR, 0, "Clear Target"); DANGER();
		ADD(PF_ACT_BACK, 0, "Back");
		break;
	}

	case PF_LIST_MEAT:
		ADD(PF_ACT_PROBE_CUSTOM, 0, "Custom");   /* a temperature of your own, first, as on the phone */
		for (int i = 0; i < PF_MEAT_COUNT && i < max - 2; i++) ADD(PF_ACT_MEAT, i, PF_MEATS[i].name);
		ADD(PF_ACT_BACK, 0, "Back");
		break;

	case PF_LIST_DONE: {
		const char *units = pf_json_str((cJSON *)status, "units", "F");
		int m = ui->meat_idx >= 0 && ui->meat_idx < PF_MEAT_COUNT ? ui->meat_idx : 0;
		for (int i = 0; i < PF_MEATS[m].n; i++) {
			ADD(PF_ACT_DONE, i, PF_MEATS[m].d[i].name);
			double done_f = PF_MEATS[m].d[i].to_f;   /* the doneness, as on the phone; it comes off earlier by carry_f */
			double v = units[0] == 'C' ? round((done_f - 32) * 5 / 9) : done_f;
			snprintf(out[n - 1].right, sizeof out[n - 1].right, "%.0f" DEG, v);
		}
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

	default: {  /* PF_LIST_ROOT: what you can do now, Stop, then Settings and Network Info -- always in that order */
		bool recipe = pf_json_bool((cJSON *)status, "recipe.active", false);
		bool timer_on = pf_json_bool((cJSON *)status, "timer.running", false);
		if (recipe) {
			if (pf_json_bool((cJSON *)status, "recipe.waiting", false)) ADD(PF_ACT_RECIPE_NEXT, 0, "Continue");
			else ADD(PF_ACT_RECIPE_SKIP, 0, "Skip Forward");
			ADD(PF_ACT_RECIPE_BACK, 0, "Skip Back");
			ADD(PF_ACT_RECIPE_EXIT, 0, "Exit Recipe"); DANGER();
			ADD(PF_ACT_LIST, PF_LIST_STOP, "Finish");
		} else if (!strcmp(mode, "Error")) {
			ADD(PF_ACT_CLEAR_ERROR, 0, "Clear Error"); DANGER();
		} else if (!strcmp(mode, "Monitor")) {
			ADD(PF_ACT_MANUAL, 0, "Control");
			ADD(PF_ACT_LIST, PF_LIST_STARTUP, "Startup");
			ADD(timer_on ? PF_ACT_TIMER_CANCEL : PF_ACT_TIMER, 0, timer_on ? "Cancel Timer" : "Timer");
			ADD(PF_ACT_LIST, PF_LIST_STOP, "Finish");
		} else if (!strcmp(mode, "Stop") || !strcmp(mode, "Prime")) {
			ADD(PF_ACT_LIST, PF_LIST_STARTUP, "Startup");
			ADD(PF_ACT_MONITOR, 0, "Monitor");
			ADD(timer_on ? PF_ACT_TIMER_CANCEL : PF_ACT_TIMER, 0, timer_on ? "Cancel Timer" : "Timer");
		} else {   /* the active menu: Startup, Reignite, Smoke, Hold, Shutdown, Manual */
			if (!strcmp(mode, "Hold")) ADD(PF_ACT_SMOKE, 0, "Smoke Mode");
			else ADD(PF_ACT_HOLD, 0, "Hold Mode");
			ADD(PF_ACT_LIST, PF_LIST_PROBE, "Probe Target");
			ADD(timer_on ? PF_ACT_TIMER_CANCEL : PF_ACT_TIMER, 0, timer_on ? "Cancel Timer" : "Timer");
			ADD(PF_ACT_LIST, PF_LIST_STOP, "Finish");
		}
		ADD(PF_ACT_LIST, PF_LIST_SETTINGS, "Settings");
		ADD(PF_ACT_NETINFO, 0, "Network Info");
		if (!strcmp(mode, "Stop")) ADD(PF_ACT_LIST, PF_LIST_POWER, "Power");
		ADD(PF_ACT_BACK, 0, "Back");
		break;
	}
	}
#undef ADD
#undef DANGER
	for (int i = 0; i < n; i++) out[i].icon = icon_for(out[i].act, out[i].arg);
	return n;
}

/* --------------------------------------------------------------- pieces */

static void draw_banner(pf_gfx *g, const cJSON *s, const char *mode, int ring)   /* 0 none, 1 the mode, 2 the timer corner */
{
	int W = g->vw;
	bool tuning_fill = pf_json_bool((cJSON *)s, "tuning.running", false) || pf_json_bool((cJSON *)s, "autotune.active", false);
	uint16_t fill = tuning_fill ? g->th.info : mode_fill(g, mode), tc = on_fill_text(g, fill);
	pf_gfx_rect(g, 0, 0, g->w, 34, fill);
	/* picked out by the knob: a two-pixel ring in the banner's own text colour, under the words */
	if (ring == 1) { pf_gfx_rrect(g, 2, 2, W - 96, 30, 5, tc); pf_gfx_rrect(g, 4, 4, W - 100, 26, 3, fill); }
	if (ring == 2) { pf_gfx_rrect(g, W - 92, 2, 90, 30, 5, tc); pf_gfx_rrect(g, W - 90, 4, 86, 26, 3, fill); }
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
	} else if (pf_json_bool((cJSON *)s, "recipe.active", false)) {
		/* a recipe has the grill: which step of how many, and what it is doing -- or that it is
		 * waiting for a hand, which is the one thing worth reading from across the yard */
		int step = (int)pf_json_num((cJSON *)s, "recipe.step", 0) + 1, n = (int)pf_json_num((cJSON *)s, "recipe.nsteps", 0);
		if (pf_json_bool((cJSON *)s, "recipe.waiting", false)) snprintf(up, sizeof up, "%d/%d CONTINUE?", step % 100, n % 100);
		else snprintf(up, sizeof up, "%d/%d %.9s", step % 100, n % 100, mode);
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
	} else if (pf_json_bool((cJSON *)s, "timer.running", false)) {
		/* a running timer takes the corner from the cook time: it is the one the cook set and is
		 * waiting on. A mode's own countdown still comes first. */
		fmt_clock(clk, sizeof clk, pf_json_num((cJSON *)s, "timer.remaining", 0));
		pf_gfx_text_right(g, B, 9, W - 10, 1, "TIMER", tc);
		pf_gfx_text_right(g, B, 18, W - 10, 11, clk, tc);
		clk[0] = 0;
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
/* where the hopper was drawn on the last main screen, so the focus ring can find it */
static int g_hop_x, g_hop_y, g_hop_w, g_hop_h;
static int g_sp_x, g_sp_y, g_sp_w, g_sp_h;   /* and the set point, likewise */

static void draw_datablock(pf_gfx *g, const cJSON *s, const cJSON *primary, const char *mode, const char *units, int x, int y, int w, bool compact)
{
	g_hop_h = 0;
	int p1 = compact ? 22 : 26, p2 = compact ? 18 : 22, p3 = compact ? 14 : 16, l1 = p1 + 4, l2 = p2 + 4, l3 = p3 + 2;
	bool valid = primary && cJSON_IsNumber(cJSON_GetObjectItem((cJSON *)primary, "temp"));
	double pit = valid ? cJSON_GetObjectItem((cJSON *)primary, "temp")->valuedouble : 0;
	double sp = pf_json_num((cJSON *)s, "setpoint", 0);
	bool hold_like = !strcmp(mode, "Hold") || (!strcmp(mode, "Startup") && !strcmp(pf_json_str((cJSON *)s, "next_mode", ""), "Hold")) || !strcmp(mode, "Reignite");
	char line[32];
	int ly = y;
	g_sp_h = 0;
	if (hold_like && sp > 0) {
		/* the set point is the thing you select to change the temperature, so it stands on its
		 * own, large, in the accent, with no label over it: the number is the label */
		int sz = compact ? 34 : 38;
		snprintf(line, sizeof line, "%.0f" DEG, sp);
		while (sz > 24 && pf_gfx_text_width(B, sz, line) > w - 4) sz -= 2;
		pf_gfx_text(g, B, sz, x, ly, line, g->th.accent);
		g_sp_x = x - 4; g_sp_y = ly - 2; g_sp_w = w + 8; g_sp_h = pf_gfx_line_height(B, sz) + 4;
		ly += pf_gfx_line_height(B, sz) + 2;
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
		pf_gfx_text(g, B, p1, x, ly, "MANUAL", g->th.text); ly += l1;
	} else {
		pf_gfx_text(g, B, p1, x, ly, "READY", g->th.muted); ly += l1;
	}
	int hop = (int)pf_json_num((cJSON *)s, "hopper_pct", -1);
	if (hop >= 0) {
		snprintf(line, sizeof line, "HOP %d%%", hop % 1000);
		/* Getting low is amber; about to run out is red. Red at a quarter full meant the panel
		 * spent most of a long cook claiming a fault it did not have, which is the surest way to
		 * teach someone to ignore the colour. */
		uint16_t hc = hop <= 10 ? g->th.danger : hop <= 25 ? g->th.warn : g->th.ok;
		pf_gfx_text(g, B, p3, x, ly, line, hop <= 25 ? g->th.danger : g->th.text);
		ly += l3;
		/* Six pixels of bar is nothing at arm's length in daylight. Give it real height and an
		 * outline, so the level reads as a level rather than as a hairline. */
		pf_gfx_bar(g, x, ly + 2, w, 14, hop / 100.0, hc, g->th.card2);
		g_hop_x = x - 4; g_hop_y = ly - l3 - 2; g_hop_w = w + 8; g_hop_h = l3 + 22;
	}
	if (pf_json_bool((cJSON *)s, "lid_open", false)) pf_gfx_text(g, B, p3, x, ly + 12, "LID OPEN", g->th.danger);
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
	int small = big / 3;
	while (big > 40 && pf_gfx_number_width(B, big, v) + 6 + pf_gfx_text_width(B, small, u) > maxw) big -= 4;   /* shrink to fit */
	pf_gfx_text(g, B, big, x - 4, y, v, c);
	/* the unit sits at the top right of the reading's box, out of the number's way */
	pf_gfx_text_right(g, B, small, x + maxw - 4, y + 2, u, c);
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

/* A one-pixel outline that follows the same corner as the block it surrounds.
 *
 * Every selection on this panel is a rounded block with an outline around it, and the outline was
 * a square frame: at each corner it stepped outside the shape it was meant to trace. On an orange
 * row against a dark card nobody saw it; on the red Stop row, where the outline is white, it read
 * as a stray box drawn around the word. One shape for all of them, so a selected row looks like a
 * selected row wherever it is. */
static void pf_sel_ring(pf_gfx *g, int x, int y, int w, int h, int r, uint16_t edge, uint16_t fill)
{
	pf_gfx_rrect(g, x, y, w, h, r, edge);
	pf_gfx_rrect(g, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : r, fill);
}

static void draw_probe_col(pf_gfx *g, const cJSON *p, const char *units, bool blink, int x, int y, int w)
{
	const cJSON *tv = cJSON_GetObjectItem((cJSON *)p, "temp");
	double target = pf_json_num((cJSON *)p, "target", 0), eta = pf_json_num((cJSON *)p, "eta_s", -1);
	bool valid = cJSON_IsNumber(tv), hit = target > 0 && valid && tv->valuedouble >= target;
	bool wireless = pf_json_bool((cJSON *)p, "wireless", false);
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
	/* The Bluetooth rune is an identity mark -- "this probe is wireless" -- not a status light. It
	 * is Bluetooth blue, always, except on a filled card where it takes the card's own text colour.
	 * The signal bars are gone from the card, as they are from the phone's: a card read across a
	 * garden carries the mark, the name and the battery, the reading, and the target. */
	uint16_t rune = filled ? g->th.accent_text : g->th.info;
	char name[12], t[8], tg[12] = "", et[8] = "";
	snprintf(name, sizeof name, "%.8s", pf_json_str((cJSON *)p, "name", "?"));
	upper(name);
	fmt_temp(t, sizeof t, tv);
	if (target > 0) snprintf(tg, sizeof tg, "%.0f" DEG, target);
	if (target > 0 && !hit && eta > 0) fmt_eta(et, sizeof et, eta);
	int battery = wireless ? (int)pf_json_num((cJSON *)p, "battery", -1) : -2;
	uint16_t tgc = filled ? g->th.accent_text : tc == alert ? alert : g->th.accent;
	uint16_t dim = filled ? g->th.accent_text : g->th.muted;
	uint16_t batc = filled ? g->th.accent_text : battery <= 20 && battery >= 0 ? g->th.danger : g->th.text;
	/* The battery is plain text -- "81%" -- at the right of the name line. The phone's cell with the
	 * number inside it was drawn here too, and at this size the number sat on its own fill with no
	 * contrast left; three characters of type say the same thing and can be read. */
	char bat[6] = "";
	if (battery >= 0) snprintf(bat, sizeof bat, "%d%%", battery % 1000);
	else if (battery == -1) snprintf(bat, sizeof bat, "--");
	if (w >= 90) {
		/* row 1: rune, name, the battery at the right
		 * row 2: the reading, large
		 * row 3: time to target left, target right */
		int nx = x + 6, nw = w - 12;
		if (wireless) { pf_gfx_bt_rune(g, x + 5, y + 4, rune); nx = x + 16; nw -= 10; }
		if (bat[0]) { pf_gfx_text_right(g, B, 11, x + w - 6, y + 5, bat, batc); nw -= pf_gfx_text_width(B, 11, bat) + 4; }
		int px = 13;
		while (px > 10 && pf_gfx_text_width(B, px, name) > nw) px--;
		pf_gfx_text(g, B, px, nx, y + 4, name, mc);
		pf_gfx_text(g, B, 28, x + 6, y + 19, t, tc);
		if (et[0]) pf_gfx_text(g, B, 11, x + 6, y + 48, et, dim);
		if (tg[0]) pf_gfx_text_right(g, B, 13, x + w - 6, y + 46, tg, tgc);
	} else {   /* narrow (portrait): rune + name / reading / target */
		name[6] = 0;
		int nx = x + 5;
		if (wireless) { pf_gfx_bt_rune(g, x + 4, y + 3, rune); nx = x + 14; }
		pf_gfx_text(g, B, 11, nx, y + 3, name, mc);
		if (bat[0]) pf_gfx_text_right(g, B, 10, x + w - 5, y + 4, bat, batc);
		pf_gfx_text(g, B, 24, x + 5, y + 15, t, tc);
		if (et[0]) pf_gfx_text(g, B, 10, x + 5, y + 49, et, dim);
		if (tg[0]) pf_gfx_text_right(g, B, 11, x + w - 5, y + 47, tg, tgc);
	}
}

static void render_main(pf_gfx *g, const cJSON *s, const pf_ui_state *ui)
{
	const char *mode = pf_json_str((cJSON *)s, "mode", "Stop");
	const char *units = pf_json_str((cJSON *)s, "units", "F");
	int W = g->vw, H = g->vh;
	bool landscape = g->w > g->h;
	draw_banner(g, s, mode, ui->main_focus == PF_FOCUS_MODE ? 1 : ui->main_focus == PF_FOCUS_TIMER ? 2 : 0);

	const cJSON *probes = cJSON_GetObjectItem((cJSON *)s, "probes");
	const cJSON *primary = NULL, *food[3] = { 0 }, *p;
	int nf = 0;
	cJSON_ArrayForEach(p, probes) {
		const char *role = pf_json_str((cJSON *)p, "role", "");
		if (!strcmp(role, "Primary")) { if (!primary) primary = p; continue; }
		if (!strcmp(role, "Food") && pf_json_bool((cJSON *)p, "enabled", true) && pf_json_bool((cJSON *)p, "home", true) && !pf_json_bool((cJSON *)p, "companion", false) && nf < 3) food[nf++] = p;
	}

	/* The focus ring: a turn of the knob picks out the banner, the pit, the hopper or a probe card,
	 * and a press acts on it. Two pixels of the accent following the block's own corner, drawn
	 * under the block so the block's own fill leaves exactly the ring showing. */
	int f = ui->main_focus;
	int top = H - 66, w = (W - 12 - 10) / 3;
	int pit_x, pit_y, pit_w, pit_h, col = W - 100;
	if (landscape) { pit_x = 4; pit_y = 76; pit_w = col - 10; pit_h = 84; }
	else { pit_x = 6; pit_y = 78; pit_w = W - 12; pit_h = 100; }
	bool sp_box = false;   /* set once the block has drawn; the ring around the pit is the fallback */
	if (landscape) {
		draw_tiles(g, s, 39, 36, 20);
		draw_pit(g, primary, units, mode, 8, 78, 100, col - 12);
		draw_datablock(g, s, primary, mode, units, col, 80, W - col - 6, true);
		if (nf == 0) pf_gfx_text(g, R, 16, 8, top + 22, "No food probes enabled", g->th.muted);
	} else {
		draw_tiles(g, s, 39, 36, 18);
		draw_pit(g, primary, units, mode, 10, 80, 118, W - 16);
		draw_datablock(g, s, primary, mode, units, 10, 190, W - 20, true);
	}
	sp_box = g_sp_h > 0;
	if (f == PF_FOCUS_SETPOINT) {
		if (sp_box) {
			pf_gfx_rrect(g, g_sp_x, g_sp_y, g_sp_w, g_sp_h, 5, g->th.accent);
			pf_gfx_rrect(g, g_sp_x + 2, g_sp_y + 2, g_sp_w - 4, g_sp_h - 4, 3, g->th.bg);
			if (landscape) draw_datablock(g, s, primary, mode, units, col, 80, W - col - 6, true);
			else draw_datablock(g, s, primary, mode, units, 10, 190, W - 20, true);
		} else {
			pf_gfx_rrect(g, pit_x, pit_y, pit_w, pit_h, 6, g->th.accent); pf_gfx_rrect(g, pit_x + 2, pit_y + 2, pit_w - 4, pit_h - 4, 4, g->th.bg);
			if (landscape) draw_pit(g, primary, units, mode, 8, 78, 100, col - 12); else draw_pit(g, primary, units, mode, 10, 80, 118, W - 16);
		}
	}
	if (f == PF_FOCUS_HOPPER && g_hop_h > 0) {
		/* the hopper's place is known once the block has drawn; ring it and draw the block again */
		pf_gfx_rrect(g, g_hop_x, g_hop_y, g_hop_w, g_hop_h, 4, g->th.accent);
		pf_gfx_rrect(g, g_hop_x + 2, g_hop_y + 2, g_hop_w - 4, g_hop_h - 4, 2, g->th.bg);
		if (landscape) draw_datablock(g, s, primary, mode, units, col, 80, W - col - 6, true);
		else draw_datablock(g, s, primary, mode, units, 10, 190, W - 20, true);
	}
	for (int i = 0; i < nf; i++) {
		int cx = 6 + i * (w + 5);
		if (f == PF_FOCUS_PROBE0 + i) pf_gfx_rrect(g, cx - 2, top - 2, w + 4, 66, 8, g->th.accent);
		draw_probe_col(g, food[i], units, ui->blink, cx, top, w);
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
	/* A long list scrolls rather than shrinks: the meats are eleven rows, and eleven rows of
	 * 13-pixel type is not a menu anyone reads at arm's length. Six rows at most, the selected one
	 * kept in view, a mark at the edge when there is more above or below. */
	int sel = ui->depth > 0 ? ui->stack[ui->depth - 1].index : 0;
	if (n > 0) sel = ((sel % n) + n) % n;
	int vis = n > 6 ? 6 : (n > 0 ? n : 1);
	int first = sel - vis / 2;
	if (first > n - vis) first = n - vis;
	if (first < 0) first = 0;
	int rowh = (H - 40) / vis;
	if (rowh > 44) rowh = 44;
	int px = rowh - 8 < 24 ? (rowh - 8 < 13 ? 13 : rowh - 8) : 24;
	int y = 38 + ((H - 40) - rowh * vis) / 2;
	/* drawn, not typed: the panel font has no arrow glyphs */
	/* a scroll bar when there is more than fits: the track down the right edge, the thumb sized
	 * and placed by where the window is in the list, so the eye knows how much menu there is */
	int bar_w = n > vis ? 8 : 0;
	if (bar_w) {
		int tx = W - 10, ty0 = 40, th = H - 46;
		pf_gfx_rrect(g, tx, ty0, 4, th, 2, g->th.card2);
		int thumb = th * vis / n; if (thumb < 12) thumb = 12;
		int pos = ty0 + (th - thumb) * first / (n - vis);
		pf_gfx_rrect(g, tx, pos, 4, thumb, 2, g->th.text);
	}
	for (int i = first; i < first + vis && i < n; i++) {
		bool is = i == sel;
		/* Selection has to survive sunlight on a dim panel: a filled block and text chosen for the
		 * fill. A slightly lighter shade of grey disappears completely outdoors. */
		uint16_t rowfill = items[i].danger ? g->th.danger : g->th.accent;
		if (is) pf_gfx_rrect(g, 6, y + 1, W - 12 - bar_w - 2, rowh - 3, 7, rowfill);   /* the fill is the selection; a ring around it read as a stray box */
		int ty = y + (rowh - pf_gfx_line_height(B, px)) / 2;
		uint16_t c = is ? on_fill_text(g, rowfill) : items[i].danger ? g->th.danger : g->th.text;
		int tx = 16;
		if (items[i].icon != PF_ICON_NONE) { draw_icon(g, items[i].icon, 14, y + (rowh - 16) / 2 - 1, c); tx = 40; }
		pf_gfx_text(g, B, px, tx, ty, items[i].label, c);
		if (items[i].right[0]) pf_gfx_text_right(g, B, px - 4 < 12 ? 12 : px - 4, W - 14 - bar_w, ty + 2, items[i].right, is ? c : g->th.muted);
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
	if (ui->temp_kind == 1) snprintf(unit, sizeof unit, "MIN");
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
		if (is) pf_sel_ring(g, 6, y, W - 12, rowh, 6, g->th.text, fill);
		else pf_gfx_rrect(g, 6, y, W - 12, rowh, 6, fill);
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
		if (sel) pf_sel_ring(g, bx, by, bw, bh, 6, on_fill_text(g, g->th.accent), g->th.accent);
		else pf_gfx_rrect(g, bx, by, bw, bh, 6, g->th.card2);
		pf_gfx_text_center(g, B, 14, bx + bw / 2, by + (bh - pf_gfx_line_height(B, 14)) / 2,
		                   BTN[b2], sel ? g->th.accent_text : g->th.text);
	}
	if (ui->margin_dirty) pf_gfx_text_center(g, R, 11, W / 2, H / 2 + 24, "unsaved", g->th.danger);
}

void pf_screens_render(pf_gfx *g, const cJSON *status, const pf_ui_state *ui)
{
	pf_gfx_clear(g, g->th.bg);
	pf_screen scr = pf_nav_screen(ui);
	/* Something is waiting to be seen -- a timer has run out, a probe has arrived -- and the panel
	 * says so with the whole screen: orange, with the word, alternating with the interface once a
	 * second until a press here or a clear on the phone acknowledges it. */
	if (ui->attention[0] && ui->blink) {
		pf_gfx_clear(g, g->th.accent);
		char word[24];
		snprintf(word, sizeof word, "%.20s", ui->attention);
		upper(word);
		int px = 56;
		while (px > 24 && pf_gfx_text_width(B, px, word) > g->vw - 24) px -= 4;
		pf_gfx_text_center(g, B, px, g->vw / 2, g->vh / 2 - pf_gfx_line_height(B, px) / 2, word, g->th.accent_text);
		return;
	}
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
