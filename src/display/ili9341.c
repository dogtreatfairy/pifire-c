/* ILI9341 320x240 SPI TFT (optionally with a KY-040 rotary encoder for an on-device menu).
 * Pins: platform.devices.display {dc, led, rst}, SPI0 CE per settings.display.spi_device,
 * encoder: platform.devices.input {up_clk, down_dt, enter_sw}. */
#define _GNU_SOURCE
#include "core/cmdq.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "display/gfx.h"
#include "display/registry.h"
#include "display/screens.h"
#include "hal/gpio.h"
#include "hal/spi.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "ili9341"
#define SPI_CHUNK 4096

typedef struct {
	const pf_env *env;
	int chipfd, spi;
	pf_gpio_line *dc, *rst, *led;
	pf_gpio_line *clk, *dt, *sw;
	int rotation;
	bool bgr;
	bool encoder;
	pf_gfx fb;
	pf_ui_state ui;
	cJSON *status;
	pthread_mutex_t mu;
	pthread_t tid;
	atomic_bool run;
	atomic_int pending_key;
	double last_activity, backlight_timeout;
	char theme[8];          /* follow | dark | light */
	bool backlight_on;
	unsigned last_hash;
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
	t->backlight_on = on;
}

/* ---------------- encoder ---------------- */

static void *encoder_thread(void *arg)
{
	tft_t *t = arg;
	pthread_setname_np(pthread_self(), "pf-encoder");
	double press_t = 0;
	bool pressed = false;
	int last_clk = pf_gpio_get(t->clk);
	while (atomic_load(&t->run)) {
		uint64_t ts;
		int e = pf_gpio_wait_edge(t->clk, 20, &ts);
		if (e >= 0) {
			int clk = pf_gpio_get(t->clk), dt = pf_gpio_get(t->dt);
			if (clk != last_clk && clk == 0) atomic_store(&t->pending_key, dt ? PF_KEY_UP : PF_KEY_DOWN);  /* falling CLK: DT tells direction */
			last_clk = clk;
		}
		int sw = pf_gpio_get(t->sw);
		double now = pf_now();
		if (sw == 1 && !pressed) { pressed = true; press_t = now; }
		else if (sw == 0 && pressed) { pressed = false; if (now - press_t < 1.5 && now - press_t > 0.03) atomic_store(&t->pending_key, PF_KEY_ENTER); }
		else if (pressed && now - press_t >= 1.5) { pressed = false; atomic_store(&t->pending_key, PF_KEY_LONG_ENTER); while (pf_gpio_get(t->sw) == 1 && atomic_load(&t->run)) pf_sleep_ms(50); }
	}
	return NULL;
}

/* ---------------- ops ---------------- */

static void apply_theme(tft_t *t);

static void *create(const char *cfg_json, const pf_env *env)
{
	cJSON *c = cJSON_Parse(cfg_json);
	tft_t *t = calloc(1, sizeof *t);
	t->env = env;
	pthread_mutex_init(&t->mu, NULL);
	t->rotation = pf_json_int(c, "rotation", 0);
	t->bgr = pf_json_bool(c, "bgr", false);
	t->backlight_timeout = pf_json_num(c, "backlight_timeout_s", 0);
	int spi_dev = pf_json_int(c, "spi_device", 0), hz = pf_json_int(c, "spi_hz", 24000000);
	t->encoder = pf_json_bool(c, "encoder", true);
	pf_strlcpy(t->theme, pf_json_str(c, "theme", "dark"), sizeof t->theme);
	int dc = pf_json_int(c, "devices.display.dc", 24), rst = pf_json_int(c, "devices.display.rst", 25), led = pf_json_int(c, "devices.display.led", 5);
	int clk = pf_json_int(c, "devices.input.up_clk", 16), dt = pf_json_int(c, "devices.input.down_dt", 20), sw = pf_json_int(c, "devices.input.enter_sw", 21);
	cJSON_Delete(c);
	char chip[64];
	pf_set_str("platform.gpiochip", chip, sizeof chip, "/dev/gpiochip0");
	bool btn_low = false;
	{ char lv[8]; pf_set_str("platform.buttonslevel", lv, sizeof lv, "HIGH"); btn_low = !strcasecmp(lv, "LOW"); }

	t->chipfd = pf_gpio_open_chip(chip);
	t->spi = pf_spi_open(0, spi_dev, 0, (uint32_t)hz);
	if (t->chipfd < 0 || t->spi < 0) { env->log(PF_LVL_ERROR, TAG, "cannot open gpio/spi (is SPI enabled?)"); free(t); return NULL; }
	t->dc = pf_gpio_request_output(t->chipfd, (unsigned)dc, false, false, "pifire-tft-dc");
	t->rst = rst >= 0 ? pf_gpio_request_output(t->chipfd, (unsigned)rst, false, true, "pifire-tft-rst") : NULL;
	t->led = led >= 0 ? pf_gpio_request_output(t->chipfd, (unsigned)led, false, true, "pifire-tft-led") : NULL;
	if (!t->dc) { env->log(PF_LVL_ERROR, TAG, "cannot claim DC GPIO%d", dc); free(t); return NULL; }
	bool landscape = t->rotation == 90 || t->rotation == 270;
	pf_gfx_init(&t->fb, landscape ? 320 : 240, landscape ? 240 : 320);
	init_panel(t);
	backlight(t, true);
	t->ui.screen = PF_SCR_MAIN;
	apply_theme(t);
	pf_screens_render(&t->fb, NULL, &t->ui);
	push_frame(t);
	t->last_activity = pf_now();

	if (t->encoder) {
		pf_gpio_bias bias = btn_low ? PF_GPIO_BIAS_PULL_UP : PF_GPIO_BIAS_PULL_DOWN;
		t->clk = pf_gpio_request_events(t->chipfd, (unsigned)clk, false, PF_GPIO_BIAS_PULL_UP, "pifire-enc-clk");
		t->dt = pf_gpio_request_input(t->chipfd, (unsigned)dt, false, PF_GPIO_BIAS_PULL_UP, "pifire-enc-dt");
		t->sw = pf_gpio_request_input(t->chipfd, (unsigned)sw, btn_low, bias, "pifire-enc-sw");
		if (t->clk && t->dt && t->sw) { atomic_store(&t->run, true); pthread_create(&t->tid, NULL, encoder_thread, t); }
		else env->log(PF_LVL_WARN, TAG, "encoder GPIOs unavailable; display is view-only");
	}
	env->log(PF_LVL_INFO, TAG, "ILI9341 %dx%d on spidev0.%d, rotation %d%s", t->fb.w, t->fb.h, spi_dev, t->rotation, t->clk ? ", encoder" : "");
	return t;
}

static void destroy(void *self)
{
	tft_t *t = self;
	if (atomic_load(&t->run)) { atomic_store(&t->run, false); pthread_join(t->tid, NULL); }
	backlight(t, false);
	cmd(t, 0x28); /* display off */
	pf_gpio_release(t->dc); pf_gpio_release(t->rst); pf_gpio_release(t->led);
	pf_gpio_release(t->clk); pf_gpio_release(t->dt); pf_gpio_release(t->sw);
	pf_spi_close(t->spi);
	pf_gpio_close_chip(t->chipfd);
	pf_gfx_free(&t->fb);
	cJSON_Delete(t->status);
	free(t);
}

static unsigned hash_fb(const pf_gfx *g)
{
	unsigned h = 2166136261u;
	const unsigned char *p = (const unsigned char *)g->px;
	for (size_t i = 0, n = (size_t)g->w * g->h * 2; i < n; i += 7) { h ^= p[i]; h *= 16777619u; }
	return h;
}

static void apply_theme(tft_t *t)
{
	pf_gfx_set_theme(&t->fb, !strcmp(t->theme, "light") ? "light" : "dark");
}

static void redraw(tft_t *t)
{
	apply_theme(t);
	if (t->ui.screen == PF_SCR_MESSAGE && pf_now() > t->ui.message_until) t->ui.screen = PF_SCR_MAIN;
	pf_screens_render(&t->fb, t->status, &t->ui);
	unsigned h = hash_fb(&t->fb);
	if (h != t->last_hash) { t->last_hash = h; push_frame(t); }
}

static void handle_key(tft_t *t, pf_key k)
{
	t->last_activity = pf_now();
	if (!t->backlight_on) { backlight(t, true); return; }
	const char *units = pf_json_str(t->status, "units", "F");
	double step = 5;
	pf_menu_item items[PF_MENU_MAX];
	int n;
	switch (t->ui.screen) {
	case PF_SCR_MAIN:
		if (k == PF_KEY_ENTER) { t->ui.screen = PF_SCR_MENU; t->ui.menu_index = 0; }
		break;
	case PF_SCR_MENU:
		n = pf_menu_build(t->status, items, PF_MENU_MAX);
		if (n <= 0) { t->ui.screen = PF_SCR_MAIN; break; }
		if (t->ui.menu_index >= n) t->ui.menu_index = n - 1;
		if (k == PF_KEY_UP) t->ui.menu_index = (t->ui.menu_index + 1) % n;
		else if (k == PF_KEY_DOWN) t->ui.menu_index = (t->ui.menu_index + n - 1) % n;
		else if (k == PF_KEY_ENTER) {
			pf_cmd c = { 0 };
			double sp = pf_json_num(t->status, "setpoint", 0);
			if (sp <= 0) sp = units[0] == 'C' ? 107 : 225;
			switch (items[t->ui.menu_index].id) {
			case PF_MI_START_SMOKE: c.type = PF_CMD_MODE; c.mode = PF_MODE_SMOKE; pf_cmdq_push(&c); break;
			case PF_MI_START_HOLD:
			case PF_MI_HOLD: t->ui.edit_setpoint = sp; t->ui.edit_is_change = false; t->ui.screen = PF_SCR_SETPOINT; return;
			case PF_MI_SETPOINT: t->ui.edit_setpoint = sp; t->ui.edit_is_change = true; t->ui.screen = PF_SCR_SETPOINT; return;
			case PF_MI_MONITOR: c.type = PF_CMD_MODE; c.mode = PF_MODE_MONITOR; pf_cmdq_push(&c); break;
			case PF_MI_SMOKE: c.type = PF_CMD_MODE; c.mode = PF_MODE_SMOKE; pf_cmdq_push(&c); break;
			case PF_MI_SMOKE_PLUS: c.type = PF_CMD_SMOKE_PLUS; c.flag = !pf_json_bool(t->status, "s_plus", false); pf_cmdq_push(&c); break;
			case PF_MI_SHUTDOWN: c.type = PF_CMD_MODE; c.mode = PF_MODE_SHUTDOWN; pf_cmdq_push(&c); break;
			case PF_MI_STOP:
			case PF_MI_CLEAR: c.type = PF_CMD_STOP; pf_cmdq_push(&c); break;
			default: break;
			}
			t->ui.screen = PF_SCR_MAIN;
		}
		break;
	case PF_SCR_SETPOINT: {
		double max = units[0] == 'C' ? 320 : 600, min = units[0] == 'C' ? 40 : 100;
		if (k == PF_KEY_UP) t->ui.edit_setpoint = fmin(max, t->ui.edit_setpoint + step);
		else if (k == PF_KEY_DOWN) t->ui.edit_setpoint = fmax(min, t->ui.edit_setpoint - step);
		else if (k == PF_KEY_ENTER) {
			pf_cmd c = t->ui.edit_is_change ? (pf_cmd){ .type = PF_CMD_SETPOINT, .num = t->ui.edit_setpoint }
			                                : (pf_cmd){ .type = PF_CMD_MODE, .mode = PF_MODE_HOLD, .num = t->ui.edit_setpoint };
			pf_cmdq_push(&c);
			t->ui.screen = PF_SCR_MAIN;
		}
		else if (k == PF_KEY_LONG_ENTER) t->ui.screen = PF_SCR_MAIN;
		break;
	}
	default: t->ui.screen = PF_SCR_MAIN; break;
	}
}

static void status(void *self, const char *json)
{
	tft_t *t = self;
	pthread_mutex_lock(&t->mu);
	cJSON_Delete(t->status);
	t->status = cJSON_Parse(json);
	int k;
	while ((k = atomic_exchange(&t->pending_key, PF_KEY_NONE)) != PF_KEY_NONE && k != PF_KEY_LONG_ENTER) handle_key(t, (pf_key)k);
	if (t->backlight_timeout > 0 && t->backlight_on && pf_now() - t->last_activity > t->backlight_timeout && !strcmp(pf_json_str(t->status, "mode", ""), "Stop")) backlight(t, false);
	redraw(t);
	pthread_mutex_unlock(&t->mu);
}

static void text(void *self, const char *msg)
{
	tft_t *t = self;
	pthread_mutex_lock(&t->mu);
	pf_strlcpy(t->ui.message, msg, sizeof t->ui.message);
	t->ui.screen = PF_SCR_MESSAGE;
	t->ui.message_until = pf_now() + 4;
	redraw(t);
	pthread_mutex_unlock(&t->mu);
}

static pf_key poll_input(void *self)
{
	tft_t *t = self;
	/* long press is reported to the daemon (e-stop); short keys are consumed by the on-device menu */
	int k = atomic_load(&t->pending_key);
	if (k == PF_KEY_LONG_ENTER && t->ui.screen != PF_SCR_SETPOINT) { atomic_store(&t->pending_key, PF_KEY_NONE); return PF_KEY_LONG_ENTER; }
	return PF_KEY_NONE;
}

static const pf_display_ops ops = { .abi = PF_DISPLAY_ABI, .id = "ili9341e", .name = "ILI9341 TFT + rotary encoder", .create = create, .destroy = destroy, .status = status, .text = text, .poll_input = poll_input };
const pf_display_ops *pf_display_ili9341(void) { return &ops; }
