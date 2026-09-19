/* Raspberry Pi platform: relays on GPIO, optional DC fan (power gate + hardware PWM).
 * The PiFire PWM board's amplifier inverts the signal, so fan% = 100 - PWM duty. */
#include "platform/rpi.h"
#include "core/log.h"
#include "core/settings.h"
#include "hal/gpio.h"
#include "hal/pwm.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "rpi"

typedef struct {
	int chipfd;
	pf_gpio_line *out[PF_OUT_COUNT];
	pf_gpio_line *in[PF_IN_COUNT];
	pf_pwm *pwm;
	bool dc_fan;
	int pwm_hz;
	int fan_pct;
	bool state[PF_OUT_COUNT];
} rpi_t;

static int pin_of(cJSON *cfg, const char *path)
{
	cJSON *n = pf_json_path(cfg, path);
	return cJSON_IsNumber(n) ? (int)n->valuedouble : -1;
}

static void *create(const char *platform_json, const pf_env *env)
{
	(void)env;
	cJSON *cfg = cJSON_Parse(platform_json);
	if (!cfg) { LOGE(TAG, "bad platform config"); return NULL; }
	rpi_t *r = calloc(1, sizeof *r);
	const char *chip = pf_json_str(cfg, "gpiochip", "/dev/gpiochip0");
	r->chipfd = pf_gpio_open_chip(chip);
	if (r->chipfd < 0) { free(r); cJSON_Delete(cfg); return NULL; }

	bool active_low = !strcasecmp(pf_json_str(cfg, "triggerlevel", "LOW"), "LOW");
	bool btn_low = !strcasecmp(pf_json_str(cfg, "buttonslevel", "HIGH"), "LOW");
	r->dc_fan = pf_json_bool(cfg, "dc_fan", false);

	struct { pf_output o; const char *key; } outs[] = {
		{ PF_OUT_POWER, "outputs.power" }, { PF_OUT_AUGER, "outputs.auger" }, { PF_OUT_IGNITER, "outputs.igniter" },
		{ PF_OUT_FAN, r->dc_fan ? "outputs.dc_fan" : "outputs.fan" },
	};
	for (size_t i = 0; i < sizeof outs / sizeof outs[0]; i++) {
		int pin = pin_of(cfg, outs[i].key);
		if (pin < 0) { LOGE(TAG, "%s not configured", outs[i].key); continue; }
		r->out[outs[i].o] = pf_gpio_request_output(r->chipfd, (unsigned)pin, active_low, false, "pifire");
		if (!r->out[outs[i].o]) LOGE(TAG, "failed to claim GPIO%d for %s", pin, outs[i].key);
	}

	if (r->dc_fan) {
		int pwm_pin = pin_of(cfg, "outputs.pwm");
		int channel = (pwm_pin == 13 || pwm_pin == 19) ? 1 : 0;
		const char *pwmchip = pf_json_str(cfg, "pwmchip", "/sys/class/pwm/pwmchip0");
		r->pwm_hz = pf_json_int(cfg, "pwm_frequency", 25000);
		r->pwm = pf_pwm_open(pwmchip, channel);
		if (!r->pwm) LOGE(TAG, "hardware PWM unavailable (%s ch%d) - DC fan will run at full speed", pwmchip, channel);
		else pf_pwm_config(r->pwm, r->pwm_hz, 100.0); /* 100% duty == 0% fan (inverted) */
	}

	int sel = pin_of(cfg, "inputs.selector"), sd = pin_of(cfg, "inputs.shutdown");
	bool standalone = pf_json_bool(cfg, "standalone", true);
	if (!standalone && sel >= 0)
		r->in[PF_IN_SELECTOR] = pf_gpio_request_input(r->chipfd, (unsigned)sel, btn_low, btn_low ? PF_GPIO_BIAS_PULL_UP : PF_GPIO_BIAS_PULL_DOWN, "pifire-sel");
	if (sd >= 0 && sd != sel)
		r->in[PF_IN_SHUTDOWN] = pf_gpio_request_input(r->chipfd, (unsigned)sd, btn_low, btn_low ? PF_GPIO_BIAS_PULL_UP : PF_GPIO_BIAS_PULL_DOWN, "pifire-shutdown");

	cJSON_Delete(cfg);
	LOGI(TAG, "platform ready (%s relays, %s fan)", active_low ? "active-low" : "active-high", r->dc_fan ? "DC/PWM" : "AC");
	return r;
}

static void all_off(void *self);

static void destroy(void *self)
{
	rpi_t *r = self;
	if (!r) return;
	all_off(r);
	for (int i = 0; i < PF_OUT_COUNT; i++) pf_gpio_release(r->out[i]);
	for (int i = 0; i < PF_IN_COUNT; i++) pf_gpio_release(r->in[i]);
	pf_pwm_close(r->pwm);
	pf_gpio_close_chip(r->chipfd);
	free(r);
}

static int set_output(void *self, pf_output o, bool on)
{
	rpi_t *r = self;
	if (!r->out[o]) return -1;
	int rc = pf_gpio_set(r->out[o], on);
	if (rc == 0) r->state[o] = on;
	if (o == PF_OUT_FAN && r->pwm) {
		if (on) { if (r->fan_pct == 0) r->fan_pct = 100; pf_pwm_config(r->pwm, r->pwm_hz, 100 - r->fan_pct); pf_pwm_enable(r->pwm, true); }
		else { pf_pwm_enable(r->pwm, false); r->fan_pct = 0; }
	}
	return rc;
}

static int set_fan_pct(void *self, int pct)
{
	rpi_t *r = self;
	if (!r->dc_fan) return 0;
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	r->fan_pct = pct;
	if (!r->pwm) return 0;
	return pf_pwm_config(r->pwm, r->pwm_hz, 100 - pct);
}

static int set_pwm_frequency(void *self, int hz)
{
	rpi_t *r = self;
	if (hz <= 0) return -1;
	r->pwm_hz = hz;
	return r->pwm ? pf_pwm_config(r->pwm, hz, 100 - r->fan_pct) : 0;
}

static bool read_input(void *self, pf_input in)
{
	rpi_t *r = self;
	return r->in[in] ? pf_gpio_get(r->in[in]) == 1 : false;
}

static void all_off(void *self)
{
	rpi_t *r = self;
	for (int i = 0; i < PF_OUT_COUNT; i++) if (r->out[i]) { pf_gpio_set(r->out[i], false); r->state[i] = false; }
	if (r->pwm) pf_pwm_enable(r->pwm, false);
	r->fan_pct = 0;
}

static int status_json(void *self, char *out, size_t n)
{
	rpi_t *r = self;
	return snprintf(out, n, "{\"power\":%s,\"fan\":%s,\"auger\":%s,\"igniter\":%s,\"fan_pct\":%d,\"dc_fan\":%s}",
	                r->state[PF_OUT_POWER] ? "true" : "false", r->state[PF_OUT_FAN] ? "true" : "false",
	                r->state[PF_OUT_AUGER] ? "true" : "false", r->state[PF_OUT_IGNITER] ? "true" : "false",
	                r->fan_pct, r->dc_fan ? "true" : "false");
}

static const pf_platform_ops ops = {
	.abi = PF_PLATFORM_ABI, .id = "rpi", .name = "Raspberry Pi",
	.create = create, .destroy = destroy, .set_output = set_output, .set_fan_pct = set_fan_pct,
	.set_pwm_frequency = set_pwm_frequency, .read_input = read_input, .all_off = all_off, .status_json = status_json,
};

const pf_platform_ops *pf_platform_rpi(void) { return &ops; }
