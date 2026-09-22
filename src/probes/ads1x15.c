/* TI ADS1115 (16-bit) / ADS1015 (12-bit) I2C ADC, single-ended AIN0..AIN3, ±4.096 V range. */
#include "hal/i2c.h"
#include "core/settings.h"
#include "core/util.h"
#include "probes/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_CONVERSION 0x00
#define REG_CONFIG     0x01

static const char *port_names[4] = { "ADC0", "ADC1", "ADC2", "ADC3" };

typedef struct {
	const pf_env *env;
	int fd;
	uint8_t addr;
	bool is1015;
	int errors;
	int timeouts;
} ads_t;

static void *create(const char *device_json, const pf_env *env)
{
	cJSON *d = cJSON_Parse(device_json);
	ads_t *s = calloc(1, sizeof *s);
	s->env = env;
	const char *a = pf_json_str(d, "config.i2c_bus_addr", "0x48");
	s->addr = (uint8_t)strtoul(a, NULL, 0);
	const char *variant = pf_json_str(d, "config.variant", "ads1115");
	s->is1015 = !strcmp(variant, "ads1015");
	int bus = pf_json_int(d, "config.i2c_bus", 1);
	cJSON_Delete(d);
	s->fd = pf_i2c_open(bus);
	if (s->fd < 0) { env->log(PF_LVL_ERROR, "ads1x15", "cannot open /dev/i2c-%d (is I2C enabled?)", bus); free(s); return NULL; }
	env->log(PF_LVL_INFO, "ads1x15", "%s at 0x%02x on i2c-%d", s->is1015 ? "ADS1015" : "ADS1115", s->addr, bus);
	return s;
}

static void destroy(void *self) { ads_t *s = self; pf_i2c_close(s->fd); free(s); }

static int ports(void *self, const char **names, int max)
{
	(void)self;
	for (int i = 0; i < 4 && i < max; i++) names[i] = port_names[i];
	return 4;
}

static int read_channel(ads_t *s, int ch, double *mv)
{
	/* OS=1 start, MUX=100+ch (single-ended), PGA=001 (±4.096 V), MODE=1 single-shot,
	 * DR=100 (128 SPS on 1115 / 1600 SPS on 1015), comparator disabled */
	uint16_t cfg = 0x8000 | (uint16_t)((4 + ch) << 12) | 0x0200 | 0x0100 | 0x0080 | 0x0003;
	uint8_t w[3] = { REG_CONFIG, (uint8_t)(cfg >> 8), (uint8_t)cfg };
	if (pf_i2c_write(s->fd, s->addr, w, 3)) return -1;

	/* Wait for the conversion the chip says it has finished, not for a guess at how long it takes.
	 *
	 * A single-shot conversion at 128 SPS nominally takes 7.8 ms, but the ADS1115 clock is only
	 * specified to 10%, so it can take 8.7 ms. Sleeping a flat 9 ms left 3% in hand, and reading
	 * the conversion register early does not fail: it quietly returns the *previous* channel's
	 * result. Reading four channels round-robin, that means the pit probe can hand back whatever
	 * was on the last port, which on a grill with three empty jacks is not a temperature at all.
	 * The OS bit reads 1 when the chip is idle, so poll it. */
	pf_sleep_ms(s->is1015 ? 1 : 7);
	uint8_t creg = REG_CONFIG, cr[2];
	bool ready = false;
	for (int i = 0; i < 12 && !ready; i++) {
		if (pf_i2c_write_read(s->fd, s->addr, &creg, 1, cr, 2)) return -1;
		ready = (cr[0] & 0x80) != 0;
		if (!ready) pf_sleep_ms(1);
	}
	if (!ready) { s->timeouts++; return -1; }

	uint8_t reg = REG_CONVERSION, r[2];
	if (pf_i2c_write_read(s->fd, s->addr, &reg, 1, r, 2)) return -1;
	int16_t raw = (int16_t)((r[0] << 8) | r[1]);
	if (s->is1015) *mv = (raw >> 4) * 2.0;           /* 12-bit, 2 mV/LSB at ±4.096 V */
	else *mv = raw * 0.125;                           /* 16-bit, 125 µV/LSB */
	if (*mv < 0) *mv = 0;
	return 0;
}

static int read_(void *self, pf_probe_sample *out, int nports)
{
	ads_t *s = self;
	int ok = 0;
	for (int i = 0; i < nports && i < 4; i++) {
		/* A port with no enabled probe on it is an open jack sitting at the rail. Converting it
		 * costs nine milliseconds and leaves the mux holding a voltage a long way from the one the
		 * pit probe is about to be measured at, which is the worst possible neighbour for the one
		 * reading that matters. Leave it alone. */
		if (out[i].kind == PF_SAMPLE_SKIP) continue;
		double mv;
		if (read_channel(s, i, &mv) == 0) { out[i].kind = PF_SAMPLE_MV; out[i].value = mv; ok++; }
		else out[i].kind = PF_SAMPLE_INVALID;
	}
	if (!ok) {
		/* nothing was asked for is not the same as nothing answered */
		for (int i = 0; i < nports && i < 4; i++) if (out[i].kind != PF_SAMPLE_SKIP) return -1;
		return 0;
	}
	if (!ok) { if (++s->errors == 5) s->env->log(PF_LVL_ERROR, "ads1x15", "no response from 0x%02x", s->addr); return -1; }
	s->errors = 0;
	return 0;
}

static int status_json(void *self, char *out, size_t n)
{
	ads_t *s = self;
	return snprintf(out, n, "{\"connected\":%s,\"address\":\"0x%02x\",\"timeouts\":%d}",
	                s->errors < 5 ? "true" : "false", s->addr, s->timeouts);
}

static const pf_probe_ops ops = {
	.abi = PF_PROBE_ABI, .id = "ads1x15", .name = "ADS1115 / ADS1015 ADC", .transient = false, .poll_ms = 250,
	.create = create, .destroy = destroy, .ports = ports, .read = read_, .status_json = status_json,
};
const pf_probe_ops *pf_probe_ads1x15(void) { return &ops; }
