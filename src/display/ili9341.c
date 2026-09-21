/* ILI9341 320x240 SPI TFT with a KY-040 rotary encoder for an on-device menu.
 * Pins: platform.devices.display {dc, led, rst}, SPI0 CE per settings.display.spi_device,
 * encoder: platform.devices.input {up_clk, down_dt, enter_sw}.
 *
 * Input follows the original PiFire display (pyky040): CLK/DT pulled down, the switch pulled UP and
 * active-low (the KY-040 shorts it to ground), quadrature decoded from edge events on both lines,
 * 250 ms switch debounce, and every input handled immediately - not on the display tick.
 * Behaviour: click = menu; turning while holding = target picker (click to confirm); the menu closes
 * after 5 s without input; while stopped the screen goes dark after settings.display.backlight_timeout_s
 * and a click brings it back straight into the menu; a long press (>= 1.5 s) is an emergency stop. */
#define _GNU_SOURCE
#include "core/cmdq.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "display/gfx.h"
#include "display/registry.h"
#include "display/screens.h"
#include "probes/ble/bluez.h"
#include "probes/probes.h"
#include "hal/gpio.h"
#include "hal/spi.h"
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "ili9341"
#define SPI_CHUNK 4096
#define MENU_TIMEOUT_S 5.0
#define LONG_PRESS_S 1.5
#define SW_DEBOUNCE_S 0.25

typedef struct {
	const pf_env *env;
	int chipfd, spi;
	pf_gpio_line *dc, *rst, *led;
	pf_gpio_line *clk, *dt, *sw;
	int rotation, margin_right, margin_bottom;
	bool bgr, encoder;
	pf_gfx fb;
	pf_ui_state ui;
	cJSON *status;
	pthread_mutex_t mu;
	pthread_t tid;
	atomic_bool run;
	atomic_bool estop;              /* long press seen: reported to the daemon through poll_input */
	double last_activity, backlight_timeout;
	bool backlight_on;
	unsigned last_hash;
	char theme[8];
	/* set-point picker acceleration (as the original: 3 quick turns -> x4, 7 -> x6) */
	int spin_count; double spin_last_t;
} tft_t;

/* ---------------- low level ---------------- */

static void cmd(tft_t *t, uint8_t c) { pf_gpio_set(t->dc, false); pf_spi_xfer(t->spi, &c, NULL, 1); }
static void data(tft_t *t, const uint8_t *d, size_t n)
{
	pf_gpio_set(t->dc, true);
	for (size_t off = 0; off < n; off += SPI_CHUNK) pf_spi_xfer(t->spi, d + off, NULL, n - off > SPI_CHUNK ? SPI_CHUNK : n - off);
}
static void cmd1(tft_t *t, uint8_t c, uint8_t v) { cmd(t, c); data(t, &v, 1); }
static void cmdn(tft_t *t, uint8_t c, const uint8_t *v, size_t n) { cmd(t, c); data(t, v, n); }

static void init_panel(tft_t *t)
{
	if (t->rst) { pf_gpio_set(t->rst, true); pf_sleep_ms(5); pf_gpio_set(t->rst, false); pf_sleep_ms(20); pf_gpio_set(t->rst, true); pf_sleep_ms(150); }
	cmd(t, 0x01); pf_sleep_ms(150);                                  /* SWRESET */
	cmdn(t, 0xEF, (const uint8_t[]){ 0x03, 0x80, 0x02 }, 3);
	cmdn(t, 0xCF, (const uint8_t[]){ 0x00, 0xC1, 0x30 }, 3);         /* power control B */
	cmdn(t, 0xED, (const uint8_t[]){ 0x64, 0x03, 0x12, 0x81 }, 4);   /* power on sequence */
	cmdn(t, 0xE8, (const uint8_t[]){ 0x85, 0x00, 0x78 }, 3);         /* driver timing A */
	cmdn(t, 0xCB, (const uint8_t[]){ 0x39, 0x2C, 0x00, 0x34, 0x02 }, 5); /* power control A */
	cmd1(t, 0xF7, 0x20);                                              /* pump ratio */
	cmdn(t, 0xEA, (const uint8_t[]){ 0x00, 0x00 }, 2);               /* driver timing B */
	cmd1(t, 0xC0, 0x23);                                              /* power control 1 */
	cmd1(t, 0xC1, 0x10);                                              /* power control 2 */
	cmdn(t, 0xC5, (const uint8_t[]){ 0x3E, 0x28 }, 2);               /* VCOM 1 */
	cmd1(t, 0xC7, 0x86);                                              /* VCOM 2 */
	static const uint8_t madctl[4] = { 0x40, 0x20, 0x80, 0xE0 };      /* 0, 90, 180, 270 degrees (MX/MV/MY) */
	cmd1(t, 0x36, (uint8_t)(madctl[(t->rotation / 90) & 3] | (t->bgr ? 0x08 : 0x00)));
	cmd1(t, 0x3A, 0x55);                                              /* 16 bpp */
	cmdn(t, 0xB1, (const uint8_t[]){ 0x00, 0x18 }, 2);               /* frame rate */
	cmdn(t, 0xB6, (const uint8_t[]){ 0x08, 0x82, 0x27 }, 3);         /* display function */
	cmd1(t, 0xF2, 0x00);                                              /* 3 gamma off */
	cmd1(t, 0x26, 0x01);                                              /* gamma curve 1 */
	cmdn(t, 0xE0, (const uint8_t[]){ 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00 }, 15);
	cmdn(t, 0xE1, (const uint8_t[]){ 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F }, 15);
	cmd(t, 0x11); pf_sleep_ms(120);                                  /* sleep out */
	cmd(t, 0x29);                                                     /* display on */
}

static void push_frame(tft_t *t)
{
	uint16_t w = (uint16_t)t->fb.w, h = (uint16_t)t->fb.h;
	uint8_t ca[4] = { 0, 0, (uint8_t)((w - 1) >> 8), (uint8_t)((w - 1) & 0xFF) };
	uint8_t pa[4] = { 0, 0, (uint8_t)((h - 1) >> 8), (uint8_t)((h - 1) & 0xFF) };
	cmdn(t, 0x2A, ca, 4);
	cmdn(t, 0x2B, pa, 4);
	cmd(t, 0x2C);
	data(t, (const uint8_t *)t->fb.px, (size_t)w * h * 2);
}

static void backlight(tft_t *t, bool on)
{
	if (t->led) pf_gpio_set(t->led, on);
	cmd(t, on ? 0x29 : 0x28);   /* display on / off: no ghost image behind a dark backlight */
	t->backlight_on = on;
	if (on) t->last_hash = 0;   /* force a full redraw on wake */
}

static void apply_theme(tft_t *t) { pf_gfx_set_theme(&t->fb, !strcmp(t->theme, "light") ? "light" : "dark"); }

static unsigned hash_fb(const pf_gfx *g)
{
	unsigned h = 2166136261u;
	const unsigned char *p = (const unsigned char *)g->px;
	for (size_t i = 0, n = (size_t)g->w * g->h * 2; i < n; i += 7) { h ^= p[i]; h *= 16777619u; }
	return h;
}

/* caller holds t->mu */
static void redraw(tft_t *t)
{
	apply_theme(t);
	if (pf_nav_screen(&t->ui) == PF_SCR_MESSAGE && pf_now() > t->ui.message_until) pf_nav_reset(&t->ui);
	if (!t->backlight_on) return;   /* asleep: nothing to draw */
	pf_screens_render(&t->fb, t->status, &t->ui);
	unsigned h = hash_fb(&t->fb);
	if (h != t->last_hash) { t->last_hash = h; push_frame(t); }
}

/* ---------------- keys (caller holds t->mu) ---------------- */

/* ---------------- navigation ---------------- */

static void open_menu(tft_t *t)
{
	pf_nav_reset(&t->ui);
	pf_nav_push(&t->ui, PF_SCR_LIST, PF_LIST_ROOT);
}

static void show_message(tft_t *t, const char *msg, double secs)
{
	pf_strlcpy(t->ui.message, msg, sizeof t->ui.message);
	t->ui.message_until = pf_now() + secs;
	pf_nav_reset(&t->ui);
	pf_nav_push(&t->ui, PF_SCR_MESSAGE, 0);
}

static double temp_step(const tft_t *t, bool *celsius)
{
	const char *units = pf_json_str(t->status, "units", "F");
	bool c = units[0] == 'C';
	if (celsius) *celsius = c;
	return c ? 2 : 5;
}

static void open_temp(tft_t *t, pf_action act, const char *title, const char *button, double value)
{
	bool c = false;
	temp_step(t, &c);
	double lo = c ? 50 : 120, hi = c ? 300 : 570;
	if (value < lo) value = lo;
	if (value > hi) value = hi;
	t->ui.temp_value = round(value);
	t->ui.temp_action = act;
	t->ui.temp_focus = 0;
	t->ui.temp_editing = false;
	pf_strlcpy(t->ui.temp_title, title, sizeof t->ui.temp_title);
	pf_strlcpy(t->ui.temp_button, button, sizeof t->ui.temp_button);
	pf_nav_push(&t->ui, PF_SCR_TEMP, 0);
}

static void open_confirm(tft_t *t, pf_action act, const char *text, const char *yes, bool danger)
{
	t->ui.confirm_action = act;
	t->ui.confirm_focus = 0;   /* Cancel: a stray press never triggers the action */
	t->ui.confirm_danger = danger;
	pf_strlcpy(t->ui.confirm_text, text, sizeof t->ui.confirm_text);
	pf_strlcpy(t->ui.confirm_yes, yes, sizeof t->ui.confirm_yes);
	pf_nav_push(&t->ui, PF_SCR_CONFIRM, 0);
}

static void spin_temp(tft_t *t, int dir, double now)
{
	bool c = false;
	double step = temp_step(t, &c), lo = c ? 50 : 120, hi = c ? 300 : 570;
	if (now - t->spin_last_t < 0.5) t->spin_count++; else t->spin_count = 0;
	t->spin_last_t = now;
	if (t->spin_count >= 7) step *= 6; else if (t->spin_count >= 3) step *= 4;
	t->ui.temp_value += dir * step;
	if (t->ui.temp_value > hi) t->ui.temp_value = lo;
	if (t->ui.temp_value < lo) t->ui.temp_value = hi;
	t->ui.temp_value = round(t->ui.temp_value);
}

/* ---------------- Bluetooth pairing from the panel ---------------- */

/* the ports each make exposes, matching share/manifest.json */
static const char *const BT_PORTS[][3] = {
	{ "BT_Probe", "BT_Ambient", NULL },   /* chefiq */
	{ "BT_Tip", "BT_Ambient", NULL },     /* meater */
	{ "BT0", "BT1", NULL },               /* ibbq: the first two channels */
};

static void bt_begin_scan(tft_t *t, int kind)
{
	if (kind < 0 || kind >= PF_BT_KIND_COUNT) return;
	pf_strlcpy(t->ui.bt_kind, PF_BT_KINDS[kind].module, sizeof t->ui.bt_kind);
	pf_strlcpy(t->ui.bt_label, PF_BT_KINDS[kind].label, sizeof t->ui.bt_label);
	t->ui.bt_n = 0;
	t->ui.bt_scanning = true;
	pf_ble_scan_start(8);
	pf_nav_push(&t->ui, PF_SCR_BTSCAN, 0);
}

/* caller holds t->mu */
static void bt_collect(tft_t *t)
{
	cJSON *found = NULL;
	if (!t->ui.bt_scanning || !pf_ble_scan_take(&found)) return;
	t->ui.bt_scanning = false;
	t->ui.bt_n = 0;
	cJSON *f;
	cJSON_ArrayForEach(f, found) {
		if (t->ui.bt_n >= PF_BT_MAX) break;
		if (strcmp(pf_json_str(f, "kind", ""), t->ui.bt_kind)) continue;   /* only this make */
		const char *addr = pf_json_str(f, "address", "");
		if (!addr[0]) continue;
		pf_bt_found *b = &t->ui.bt[t->ui.bt_n++];
		pf_strlcpy(b->name, pf_json_str(f, "name", addr), sizeof b->name);
		pf_strlcpy(b->addr, addr, sizeof b->addr);
		b->bars = pf_signal_bars((int)pf_json_num(f, "rssi", 0));
		b->mine = true;
	}
	cJSON_Delete(found);
	if (pf_nav_top(&t->ui)) pf_nav_top(&t->ui)->index = 0;
}

/* Add the probe to settings the way the web pairing flow does: one device with its ports, and one
 * probe per port, the ambient sensor hidden from the home screens. */
static void bt_add(tft_t *t, int idx)
{
	if (idx < 0 || idx >= t->ui.bt_n) return;
	int kind = 0;
	for (int i = 0; i < PF_BT_KIND_COUNT; i++) if (!strcmp(PF_BT_KINDS[i].module, t->ui.bt_kind)) kind = i;

	cJSON *root = pf_settings_lock();
	cJSON *map = pf_json_path(root, "probe_settings.probe_map");
	cJSON *devs = cJSON_GetObjectItem(map, "probe_devices");
	cJSON *infos = cJSON_GetObjectItem(map, "probe_info");
	if (!devs || !infos) { pf_settings_unlock(); show_message(t, "Cannot add probe", 4); return; }

	/* a device name nothing else is using, and the next free BTn probe name */
	char dev[32];
	for (int n = 1; n < 100; n++) {
		snprintf(dev, sizeof dev, "%.16s%d", t->ui.bt_kind, n % 1000);
		bool taken = false;
		cJSON *d;
		cJSON_ArrayForEach(d, devs) if (!strcasecmp(pf_json_str(d, "device", ""), dev)) taken = true;
		if (!taken) break;
	}
	int bt = 1;
	for (; bt < 100; bt++) {
		char want[12];
		snprintf(want, sizeof want, "BT%d", bt % 1000);
		bool taken = false;
		cJSON *pi;
		cJSON_ArrayForEach(pi, infos) if (!strcasecmp(pf_json_str(pi, "name", ""), want)) taken = true;
		if (!taken) break;
	}

	cJSON *d = cJSON_CreateObject();
	cJSON_AddStringToObject(d, "device", dev);
	cJSON_AddStringToObject(d, "module", t->ui.bt_kind);
	cJSON *ports = cJSON_AddArrayToObject(d, "ports");
	for (int i = 0; BT_PORTS[kind][i]; i++) cJSON_AddItemToArray(ports, cJSON_CreateString(BT_PORTS[kind][i]));
	cJSON *cfg = cJSON_AddObjectToObject(d, "config");
	cJSON_AddStringToObject(cfg, "hardware_id", t->ui.bt[idx].addr);
	cJSON_AddBoolToObject(cfg, "transient", true);
	cJSON_AddItemToArray(devs, d);

	for (int i = 0; BT_PORTS[kind][i]; i++) {
		bool ambient = strcasestr(BT_PORTS[kind][i], "Ambient") != NULL;
		char name[40], label[40];
		snprintf(name, sizeof name, ambient ? "BT%d Ambient" : "BT%d", bt % 1000);
		snprintf(label, sizeof label, "%.30s%d", dev, i + 1);
		cJSON *pi = cJSON_CreateObject();
		cJSON_AddStringToObject(pi, "type", "Food");
		cJSON_AddStringToObject(pi, "label", label);
		cJSON_AddStringToObject(pi, "name", name);
		cJSON_AddStringToObject(pi, "profile", "TWPS00");
		cJSON_AddStringToObject(pi, "device", dev);
		cJSON_AddStringToObject(pi, "port", BT_PORTS[kind][i]);
		cJSON_AddBoolToObject(pi, "enabled", true);
		cJSON_AddBoolToObject(pi, "show_on_home", !ambient);
		cJSON_AddItemToArray(infos, pi);
	}
	pf_settings_unlock();
	pf_settings_save();
	pf_cmd c = { .type = PF_CMD_PROBES_CHANGED };
	pf_cmdq_push(&c);
	char msg[64];
	snprintf(msg, sizeof msg, "BT%d added", bt % 1000);
	show_message(t, msg, 3);
}

/* Switch every probe of one Bluetooth device on or off. The ambient sibling follows its probe,
 * because the two are one piece of hardware. */
static void bt_set_enabled(tft_t *t, const char *device, bool on)
{
	cJSON *root = pf_settings_lock();
	cJSON *infos = pf_json_path(root, "probe_settings.probe_map.probe_info"), *pi;
	int changed = 0;
	cJSON_ArrayForEach(pi, infos) {
		if (strcmp(pf_json_str(pi, "device", ""), device)) continue;
		cJSON *e = cJSON_GetObjectItem(pi, "enabled");
		if (e) cJSON_ReplaceItemInObject(pi, "enabled", on ? cJSON_CreateTrue() : cJSON_CreateFalse());
		else cJSON_AddBoolToObject(pi, "enabled", on);
		changed++;
	}
	pf_settings_unlock();
	if (!changed) return;
	pf_settings_save();
	pf_cmd c = { .type = PF_CMD_PROBES_CHANGED };
	pf_cmdq_push(&c);
	LOGI(TAG, "%s %s from the panel", device, on ? "enabled" : "disabled");
}

static void bt_remove(tft_t *t, const char *device)
{
	cJSON *root = pf_settings_lock();
	cJSON *map = pf_json_path(root, "probe_settings.probe_map");
	cJSON *devs = cJSON_GetObjectItem(map, "probe_devices");
	cJSON *infos = cJSON_GetObjectItem(map, "probe_info");
	for (int i = cJSON_GetArraySize(infos) - 1; i >= 0; i--)
		if (!strcmp(pf_json_str(cJSON_GetArrayItem(infos, i), "device", ""), device)) cJSON_DeleteItemFromArray(infos, i);
	for (int i = cJSON_GetArraySize(devs) - 1; i >= 0; i--)
		if (!strcmp(pf_json_str(cJSON_GetArrayItem(devs, i), "device", ""), device)) cJSON_DeleteItemFromArray(devs, i);
	pf_settings_unlock();
	pf_settings_save();
	pf_cmd c = { .type = PF_CMD_PROBES_CHANGED };
	pf_cmdq_push(&c);
	char msg[64];
	snprintf(msg, sizeof msg, "%.24s removed", device);
	show_message(t, msg, 3);
}

/* ---------------- acting on a menu row ---------------- */

static void do_action(tft_t *t, pf_action act, int arg)
{
	pf_cmd c = { 0 };
	const char *mode = pf_json_str(t->status, "mode", "Stop");
	switch (act) {
	case PF_ACT_BACK:
		pf_nav_pop(&t->ui);
		return;
	case PF_ACT_LIST:
		pf_nav_push(&t->ui, PF_SCR_LIST, arg);
		return;
	case PF_ACT_NETINFO:
		pf_nav_push(&t->ui, PF_SCR_NETINFO, 0);
		return;
	case PF_ACT_MANUAL:
		t->ui.manual_focus = 0;
		pf_nav_push(&t->ui, PF_SCR_MANUAL, 0);
		return;

	case PF_ACT_STARTUP:
		c.type = PF_CMD_MODE; c.mode = PF_MODE_STARTUP; pf_cmdq_push(&c); break;
	case PF_ACT_STARTUP_HOLD:
		/* the target is remembered and applied when startup finishes */
		open_temp(t, PF_ACT_STARTUP_HOLD, "STARTUP TO HOLD", "Startup",
		          pf_json_num(t->status, "setpoint", 0) > 0 ? pf_json_num(t->status, "setpoint", 0) : pf_set_num("startup.start_to_mode.primary_setpoint", 225));
		return;
	case PF_ACT_STARTUP_SMOKE:
		c.type = PF_CMD_MODE; c.mode = PF_MODE_SMOKE; pf_cmdq_push(&c); break;
	case PF_ACT_HOLD:
		open_temp(t, PF_ACT_HOLD, "HOLD", "Start",
		          pf_json_num(t->status, "setpoint", 0) > 0 ? pf_json_num(t->status, "setpoint", 0) : pf_set_num("startup.start_to_mode.primary_setpoint", 225));
		return;
	case PF_ACT_SMOKE:
		open_confirm(t, PF_ACT_SMOKE, "Switch to Smoke Mode?", "Start", false);
		return;
	case PF_ACT_MONITOR:
		c.type = PF_CMD_MODE; c.mode = PF_MODE_MONITOR; pf_cmdq_push(&c); break;
	case PF_ACT_END_COOK:
		c.type = PF_CMD_MODE; c.mode = PF_MODE_SHUTDOWN; pf_cmdq_push(&c); break;
	case PF_ACT_STOP: case PF_ACT_CLEAR_ERROR:
		c.type = PF_CMD_STOP; pf_cmdq_push(&c); break;
	case PF_ACT_ESTOP:
		open_confirm(t, PF_ACT_STOP, "Emergency Stop?", "Stop", true);
		return;

	case PF_ACT_PROBE_TARGET: {
		const cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItem(t->status, "probes"), arg);
		if (!p) return;
		bool cel = false;
		temp_step(t, &cel);
		double cur = pf_json_num((cJSON *)p, "target", 0);
		pf_strlcpy(t->ui.temp_probe, pf_json_str((cJSON *)p, "label", ""), sizeof t->ui.temp_probe);
		open_temp(t, PF_ACT_PROBE_TARGET, "PROBE TARGET", "Save", cur > 0 ? cur : (cel ? 95 : 203));
		return;
	}
	case PF_ACT_BT_SCAN:
		bt_begin_scan(t, arg);
		return;
	case PF_ACT_BT_TOGGLE: {
		pf_bt_device devs[PF_BT_DEV_MAX];
		int nd = pf_bt_devices(t->status, devs, PF_BT_DEV_MAX);
		if (arg < 0 || arg >= nd) return;
		bt_set_enabled(t, devs[arg].device, !devs[arg].enabled);
		return;   /* stay on the list so several can be switched in a row */
	}
	case PF_ACT_BT_DELETE: {
		pf_bt_device devs[PF_BT_DEV_MAX];
		int nd = pf_bt_devices(t->status, devs, PF_BT_DEV_MAX);
		if (arg < 0 || arg >= nd) return;
		pf_strlcpy(t->ui.bt_label, devs[arg].device, sizeof t->ui.bt_label);
		char q[44];
		snprintf(q, sizeof q, "Remove %.22s?", devs[arg].name);
		open_confirm(t, PF_ACT_BT_DELETE, q, "Remove", true);
		return;
	}
	case PF_ACT_BT_ADD: {
		if (arg < 0 || arg >= t->ui.bt_n) return;
		char q[44];
		snprintf(q, sizeof q, "Connect %.24s?", t->ui.bt[arg].name);
		t->ui.confirm_focus = 0;
		open_confirm(t, PF_ACT_BT_ADD, q, "Connect", false);
		pf_nav_top(&t->ui)->index = arg;   /* remember which one was chosen */
		return;
	}

	case PF_ACT_RESTART: case PF_ACT_POWEROFF:
		open_confirm(t, act, act == PF_ACT_RESTART ? "Restart the controller?" : "Shut down the controller?",
		             act == PF_ACT_RESTART ? "Restart" : "Shut Down", true);
		return;
	default:
		(void)mode;
		return;
	}
	pf_nav_reset(&t->ui);
}

/* the action button on the temperature selector */
static void temp_confirm(tft_t *t)
{
	pf_cmd c = { 0 };
	switch (t->ui.temp_action) {
	case PF_ACT_STARTUP_HOLD:
		/* from Stop this routes through Startup and lands in Hold at the chosen target */
		c.type = PF_CMD_MODE; c.mode = PF_MODE_HOLD; c.num = t->ui.temp_value; pf_cmdq_push(&c);
		break;
	case PF_ACT_HOLD:
		if (!strcmp(pf_json_str(t->status, "mode", ""), "Hold")) { c.type = PF_CMD_SETPOINT; c.num = t->ui.temp_value; }
		else { c.type = PF_CMD_MODE; c.mode = PF_MODE_HOLD; c.num = t->ui.temp_value; }
		pf_cmdq_push(&c);
		break;
	case PF_ACT_PROBE_TARGET:
		c.type = PF_CMD_NOTIFY_TARGET; c.num = t->ui.temp_value;
		pf_strlcpy(c.str, t->ui.temp_probe, sizeof c.str);
		pf_cmdq_push(&c);
		break;
	default: break;
	}
	pf_nav_reset(&t->ui);
}

static void confirm_yes(tft_t *t)
{
	pf_action act = t->ui.confirm_action;
	if (act == PF_ACT_RESTART || act == PF_ACT_POWEROFF) {
		bool reboot = act == PF_ACT_RESTART;
		pf_cmd c = { .type = PF_CMD_STOP };
		pf_cmdq_push(&c);
		show_message(t, reboot ? "Restarting..." : "Shutting down...", 30);
		LOGW(TAG, "%s requested from the panel", reboot ? "restart" : "power off");
		redraw(t);
		pf_sleep_ms(400);
		pf_system_power(reboot);   /* the simulator never loads this driver */
		return;
	}
	if (act == PF_ACT_BT_ADD) { int i = pf_nav_top(&t->ui) ? pf_nav_top(&t->ui)->index : 0; pf_nav_pop(&t->ui); bt_add(t, i); return; }
	if (act == PF_ACT_BT_DELETE) { pf_nav_pop(&t->ui); bt_remove(t, t->ui.bt_label); return; }
	pf_nav_pop(&t->ui);
	do_action(t, act, 0);
}

static void manual_press(tft_t *t)
{
	static const char *const outs[3] = { "auger", "fan", "igniter" };
	static const char *const keys[3] = { "outputs.auger", "outputs.fan", "outputs.igniter" };
	int f = t->ui.manual_focus;
	if (f >= 0 && f < 3) {
		pf_cmd c = { .type = PF_CMD_MANUAL_OUTPUT, .flag = !pf_json_bool(t->status, keys[f], false) };
		pf_strlcpy(c.str, outs[f], sizeof c.str);
		pf_cmdq_push(&c);
		return;
	}
	/* Exit: everything this screen switched on goes off again */
	for (int i = 0; i < 3; i++) {
		pf_cmd c = { .type = PF_CMD_MANUAL_OUTPUT, .flag = false };
		pf_strlcpy(c.str, outs[i], sizeof c.str);
		pf_cmdq_push(&c);
	}
	pf_nav_pop(&t->ui);
}

/* ---------------- keys ---------------- */

static void handle_key(tft_t *t, pf_key k, double now)
{
	t->last_activity = now;
	if (!t->backlight_on) {
		backlight(t, true);
		if (k == PF_KEY_ENTER) open_menu(t);   /* one click from dark: straight into the menu */
		return;
	}
	if (!t->status) return;
	const char *mode = pf_json_str(t->status, "mode", "Stop");
	int dir = k == PF_KEY_UP ? 1 : k == PF_KEY_DOWN ? -1 : 0;

	switch (pf_nav_screen(&t->ui)) {
	case PF_SCR_MAIN:
		if (k == PF_KEY_ENTER) open_menu(t);
		else if (dir && !strcmp(mode, "Hold")) {
			open_temp(t, PF_ACT_HOLD, "HOLD", "Start", pf_json_num(t->status, "setpoint", 0));
			t->ui.temp_editing = true;
			spin_temp(t, dir, now);
		}
		break;

	case PF_SCR_LIST: {
		pf_menu_item items[PF_MENU_MAX];
		int n = pf_menu_build(t->status, &t->ui, items, PF_MENU_MAX);
		pf_nav *nav = pf_nav_top(&t->ui);
		if (n <= 0 || !nav) { pf_nav_pop(&t->ui); break; }
		if (dir) nav->index = ((nav->index + dir) % n + n) % n;
		else if (k == PF_KEY_ENTER) do_action(t, items[nav->index % n].act, items[nav->index % n].arg);
		break;
	}

	case PF_SCR_TEMP:
		if (t->ui.temp_editing) {
			if (dir) spin_temp(t, dir, now);
			else if (k == PF_KEY_ENTER) t->ui.temp_editing = false;
		} else if (dir) {
			/* three stops that clamp: the value sits between Back and the action button */
			int f = t->ui.temp_focus + (dir > 0 ? 1 : -1);
			if (f < 0) f = 0;
			if (f > 2) f = 2;
			/* turning forward from the value reaches the action button, backward reaches Back */
			t->ui.temp_focus = f;
		} else if (k == PF_KEY_ENTER) {
			if (t->ui.temp_focus == 0) t->ui.temp_editing = true;
			else if (t->ui.temp_focus == 1) temp_confirm(t);
			else pf_nav_pop(&t->ui);
		}
		break;

	case PF_SCR_CONFIRM:
		if (dir) t->ui.confirm_focus = dir > 0 ? 1 : 0;
		else if (k == PF_KEY_ENTER) {
			if (t->ui.confirm_focus == 1) confirm_yes(t);
			else pf_nav_pop(&t->ui);
		}
		break;

	case PF_SCR_BTSCAN: {
		if (t->ui.bt_scanning) break;
		pf_nav *nav = pf_nav_top(&t->ui);
		int rows = t->ui.bt_n + 1;
		if (!nav) break;
		if (dir) nav->index = ((nav->index + dir) % rows + rows) % rows;
		else if (k == PF_KEY_ENTER) {
			if (t->ui.bt_n == 0) { t->ui.bt_scanning = true; pf_ble_scan_start(8); }   /* scan again */
			else if (nav->index % rows == t->ui.bt_n) pf_nav_pop(&t->ui);
			else do_action(t, PF_ACT_BT_ADD, nav->index % rows);
		}
		break;
	}

	case PF_SCR_MANUAL:
		if (dir) t->ui.manual_focus = ((t->ui.manual_focus + dir) % 4 + 4) % 4;
		else if (k == PF_KEY_ENTER) manual_press(t);
		break;

	case PF_SCR_NETINFO:
	case PF_SCR_MESSAGE:
	default:
		pf_nav_pop(&t->ui);
		break;
	}
}

static void input(tft_t *t, pf_key k)
{
	pthread_mutex_lock(&t->mu);
	handle_key(t, k, pf_now());
	redraw(t);
	pthread_mutex_unlock(&t->mu);
}

/* ---------------- encoder thread ---------------- */

/* quadrature transition table: index = (prev_state << 2) | state, states are (CLK << 1) | DT */
static const int8_t QUAD[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };

static void *encoder_thread(void *arg)
{
	tft_t *t = arg;
	pthread_setname_np(pthread_self(), "pf-encoder");
	int state = ((pf_gpio_get(t->clk) > 0) << 1) | (pf_gpio_get(t->dt) > 0), accum = 0;
	bool pressed = pf_gpio_get(t->sw) > 0, long_sent = false;
	double press_t = 0, last_enter = 0;
	struct pollfd fds[3] = { { .fd = pf_gpio_fd(t->clk), .events = POLLIN }, { .fd = pf_gpio_fd(t->dt), .events = POLLIN }, { .fd = pf_gpio_fd(t->sw), .events = POLLIN } };
	while (atomic_load(&t->run)) {
		int r = poll(fds, 3, 50);
		double now = pf_now();
		if (r > 0) {
			if (fds[0].revents & POLLIN) pf_gpio_read_edge(t->clk, NULL);
			if (fds[1].revents & POLLIN) pf_gpio_read_edge(t->dt, NULL);
			if ((fds[0].revents | fds[1].revents) & POLLIN) {
				int ns = ((pf_gpio_get(t->clk) > 0) << 1) | (pf_gpio_get(t->dt) > 0);
				if (ns != state) {
					accum += QUAD[(state << 2) | ns];
					state = ns;
					if (ns == 3) {                      /* back at the detent: one click of the knob */
						if (accum >= 2) input(t, PF_KEY_UP);
						else if (accum <= -2) input(t, PF_KEY_DOWN);
						accum = 0;
					}
				}
			}
			if (fds[2].revents & POLLIN) {
				pf_gpio_read_edge(t->sw, NULL);
				bool down = pf_gpio_get(t->sw) > 0;
				if (down && !pressed) { pressed = true; press_t = now; long_sent = false; }
				else if (!down && pressed) {
					pressed = false;
					if (!long_sent && now - press_t >= 0.02 && now - last_enter >= SW_DEBOUNCE_S) { last_enter = now; input(t, PF_KEY_ENTER); }
				}
			}
		}
		if (pressed && !long_sent && now - press_t >= LONG_PRESS_S) {
			long_sent = true;
			pthread_mutex_lock(&t->mu);
			if (t->ui.depth > 0) { pf_nav_reset(&t->ui); redraw(t); }   /* long press backs out to the grill */
			else atomic_store(&t->estop, true);
			pthread_mutex_unlock(&t->mu);
		}
	}
	return NULL;
}

/* ---------------- ops ---------------- */

static void *create(const char *cfg_json, const pf_env *env)
{
	cJSON *c = cJSON_Parse(cfg_json);
	tft_t *t = calloc(1, sizeof *t);
	t->env = env;
	pthread_mutex_init(&t->mu, NULL);
	t->rotation = pf_json_int(c, "rotation", 0);
	t->bgr = pf_json_bool(c, "bgr", false);
	t->margin_right = pf_json_int(c, "margin_right", 16);
	t->margin_bottom = pf_json_int(c, "margin_bottom", 0);
	if (t->margin_right < 0 || t->margin_right > 60) t->margin_right = 16;
	if (t->margin_bottom < 0 || t->margin_bottom > 60) t->margin_bottom = 0;
	t->backlight_timeout = pf_json_num(c, "backlight_timeout_s", 5);
	int spi_dev = pf_json_int(c, "spi_device", 0), hz = pf_json_int(c, "spi_hz", 32000000);
	t->encoder = pf_json_bool(c, "encoder", true);
	pf_strlcpy(t->theme, pf_json_str(c, "theme", "dark"), sizeof t->theme);
	int dc = pf_json_int(c, "devices.display.dc", 24), rst = pf_json_int(c, "devices.display.rst", 25), led = pf_json_int(c, "devices.display.led", 5);
	int clk = pf_json_int(c, "devices.input.up_clk", 16), dt = pf_json_int(c, "devices.input.down_dt", 20), sw = pf_json_int(c, "devices.input.enter_sw", 21);
	cJSON_Delete(c);
	char chip[64];
	pf_set_str("platform.gpiochip", chip, sizeof chip, "/dev/gpiochip0");

	t->chipfd = pf_gpio_open_chip(chip);
	t->spi = pf_spi_open(0, spi_dev, 0, (uint32_t)hz);
	if (t->chipfd < 0 || t->spi < 0) { env->log(PF_LVL_ERROR, TAG, "cannot open gpio/spi (is SPI enabled?)"); free(t); return NULL; }
	t->dc = pf_gpio_request_output(t->chipfd, (unsigned)dc, false, false, "pifire-tft-dc");
	t->rst = rst >= 0 ? pf_gpio_request_output(t->chipfd, (unsigned)rst, false, true, "pifire-tft-rst") : NULL;
	t->led = led >= 0 ? pf_gpio_request_output(t->chipfd, (unsigned)led, false, true, "pifire-tft-led") : NULL;
	if (!t->dc) { env->log(PF_LVL_ERROR, TAG, "cannot claim DC GPIO%d", dc); free(t); return NULL; }
	bool landscape = t->rotation == 90 || t->rotation == 270;
	pf_gfx_init(&t->fb, landscape ? 320 : 240, landscape ? 240 : 320);
	t->fb.vw = t->fb.w - t->margin_right; t->fb.vh = t->fb.h - t->margin_bottom;   /* bezel hides the edge */
	init_panel(t);
	backlight(t, true);
	pf_nav_reset(&t->ui);
	apply_theme(t);
	pf_screens_render(&t->fb, NULL, &t->ui);
	push_frame(t);
	t->last_activity = pf_now();

	if (t->encoder) {
		/* as the original (pyky040): CLK/DT pulled down, switch pulled up and active-low regardless of buttonslevel */
		t->clk = pf_gpio_request_events(t->chipfd, (unsigned)clk, false, PF_GPIO_BIAS_PULL_DOWN, "pifire-enc-clk");
		t->dt = pf_gpio_request_events(t->chipfd, (unsigned)dt, false, PF_GPIO_BIAS_PULL_DOWN, "pifire-enc-dt");
		t->sw = pf_gpio_request_events(t->chipfd, (unsigned)sw, true, PF_GPIO_BIAS_PULL_UP, "pifire-enc-sw");
		if (t->clk && t->dt && t->sw) { atomic_store(&t->run, true); pthread_create(&t->tid, NULL, encoder_thread, t); }
		else env->log(PF_LVL_WARN, TAG, "encoder GPIOs unavailable; display is view-only");
	}
	env->log(PF_LVL_INFO, TAG, "ILI9341 %dx%d on spidev0.%d at %d MHz, rotation %d%s", t->fb.w, t->fb.h, spi_dev, hz / 1000000, t->rotation, t->clk ? ", encoder" : "");
	return t;
}

static void destroy(void *self)
{
	tft_t *t = self;
	if (atomic_load(&t->run)) { atomic_store(&t->run, false); pthread_join(t->tid, NULL); }
	backlight(t, false);
	pf_gpio_release(t->dc); pf_gpio_release(t->rst); pf_gpio_release(t->led);
	pf_gpio_release(t->clk); pf_gpio_release(t->dt); pf_gpio_release(t->sw);
	pf_spi_close(t->spi);
	pf_gpio_close_chip(t->chipfd);
	pf_gfx_free(&t->fb);
	cJSON_Delete(t->status);
	free(t);
}

/* display tick (2 Hz): new status, timeouts, sleep while stopped */
static void status(void *self, const char *json)
{
	tft_t *t = self;
	pthread_mutex_lock(&t->mu);
	cJSON_Delete(t->status);
	t->status = cJSON_Parse(json);
	double now = pf_now();
	t->ui.blink = !t->ui.blink;   /* 2 Hz tick -> 1 Hz flash */
	bool stopped = !strcmp(pf_json_str(t->status, "mode", ""), "Stop");
	bt_collect(t);
	if (t->ui.depth > 0 && pf_nav_screen(&t->ui) != PF_SCR_MESSAGE && !t->ui.bt_scanning && now - t->last_activity > MENU_TIMEOUT_S) pf_nav_reset(&t->ui);
	if (!stopped && !t->backlight_on) backlight(t, true);                       /* any active mode: screen on */
	if (stopped && t->backlight_on && t->backlight_timeout > 0 && pf_nav_screen(&t->ui) == PF_SCR_MAIN && now - t->last_activity > t->backlight_timeout) backlight(t, false);
	redraw(t);
	pthread_mutex_unlock(&t->mu);
}

static void text(void *self, const char *msg)
{
	tft_t *t = self;
	pthread_mutex_lock(&t->mu);
	pf_strlcpy(t->ui.message, msg, sizeof t->ui.message);
	pf_nav_reset(&t->ui);
	pf_nav_push(&t->ui, PF_SCR_MESSAGE, 0);
	t->ui.message_until = pf_now() + 4;
	if (!t->backlight_on) backlight(t, true);
	t->last_activity = pf_now();
	redraw(t);
	pthread_mutex_unlock(&t->mu);
}

static pf_key poll_input(void *self)
{
	tft_t *t = self;
	return atomic_exchange(&t->estop, false) ? PF_KEY_LONG_ENTER : PF_KEY_NONE;
}

static const pf_display_ops ops = { .abi = PF_DISPLAY_ABI, .id = "ili9341e", .name = "ILI9341 TFT + rotary encoder", .create = create, .destroy = destroy, .status = status, .text = text, .poll_input = poll_input };
const pf_display_ops *pf_display_ili9341(void) { return &ops; }
