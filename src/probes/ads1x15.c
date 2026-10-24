/* TI ADS1115 (16-bit) / ADS1015 (12-bit) I2C ADC, single-ended AIN0..AIN3, ±4.096 V range.
 *
 * One reading is sixteen conversions, not one. A probe on a long lead beside an auger motor and
 * an igniter relay picks up mains hum and switching spikes, and a single 2 ms conversion lands
 * wherever the noise is at that instant. Sixteen at 475 SPS span about sixty milliseconds --
 * three or four cycles of 50 or 60 Hz, so the hum averages out -- and the reading is the mean
 * of the middle eight, so a spike that lands in the window is thrown away rather than averaged
 * in. The spread of the sixteen is kept per port and reported, which is how a noisy lead is
 * told from a noisy fire afterwards.
 *
 * The reading is ratiometric where the board allows it. The divider is fed from the 3.3 V rail,
 * and the chip measures against its own reference, so anything the rail does -- sag under a
 * relay, ripple from the Pi -- reads as temperature. A port with nothing plugged in sits at the
 * rail through its pull-up, so once a second one is converted as the rail's own reading and the
 * others are scaled by it, which cancels the rail from the result. The reading is only used
 * when it is within a tenth of the configured reference, since a port that is not really empty
 * reads something else entirely. */
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
	double vs_cfg;            /* the reference voltage configured for the dividers */
	double vs_meas;           /* the rail as an empty port reads it, 0 when none is known */
	double ref_t;             /* when the rail was last read */
	double spread_mv[4];      /* max - min of the last reading's conversions, per port */
	int    nconv;             /* conversions per reading */
} ads_t;

#define OVERSAMPLE 16
#define REF_OVERSAMPLE 8
#define REF_EVERY_S 1.0

static int cmp_double(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static void *create(const char *device_json, const pf_env *env)
{
	cJSON *d = cJSON_Parse(device_json);
	ads_t *s = calloc(1, sizeof *s);
	if (!s) { cJSON_Delete(d); return NULL; }
	s->env = env;
	const char *a = pf_json_str(d, "config.i2c_bus_addr", "0x48");
	s->addr = (uint8_t)strtoul(a, NULL, 0);
	const char *variant = pf_json_str(d, "config.variant", "ads1115");
	s->is1015 = !strcmp(variant, "ads1015");
	int bus = pf_json_int(d, "config.i2c_bus", 1);
	s->vs_cfg = pf_json_num(d, "config.voltage_ref", 3.28);
	s->nconv = OVERSAMPLE;
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

/* one conversion, waited for */
static int convert_once(ads_t *s, int ch, double *mv)
{
	/* OS=1 start, MUX=100+ch (single-ended), PGA=001 (±4.096 V), MODE=1 single-shot,
	 * DR=110 (475 SPS on 1115 / 2400 SPS on 1015), comparator disabled */
	uint16_t cfg = 0x8000 | (uint16_t)((4 + ch) << 12) | 0x0200 | 0x0100 | 0x00C0 | 0x0003;
	uint8_t w[3] = { REG_CONFIG, (uint8_t)(cfg >> 8), (uint8_t)cfg };
	if (pf_i2c_write(s->fd, s->addr, w, 3)) return -1;

	/* Wait for the conversion the chip says it has finished, not for a guess at how long it takes.
	 *
	 * A single-shot conversion at 475 SPS nominally takes 2.1 ms, but the ADS1115 clock is only
	 * specified to 10%, so it can take 2.3 ms. Sleeping a flat time left little in hand, and reading
	 * the conversion register early does not fail: it quietly returns the *previous* channel's
	 * result. Reading four channels round-robin, that means the pit probe can hand back whatever
	 * was on the last port, which on a grill with three empty jacks is not a temperature at all.
	 * The OS bit reads 1 when the chip is idle, so poll it. */
	pf_sleep_ms(s->is1015 ? 1 : 2);
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

/* a reading: n conversions, the mean of the middle half, and their spread */
static int read_channel(ads_t *s, int ch, int n, double *mv, double *spread)
{
	double v[OVERSAMPLE];
	int k = 0;
	if (n > OVERSAMPLE) n = OVERSAMPLE;
	for (int i = 0; i < n; i++) if (convert_once(s, ch, &v[k]) == 0) k++;
	if (k < n / 2 || k < 1) return -1;
	qsort(v, k, sizeof v[0], cmp_double);
	int lo = k / 4, hi = k - k / 4;
	double sum = 0;
	for (int i = lo; i < hi; i++) sum += v[i];
	*mv = sum / (hi - lo);
	if (spread) *spread = v[k - 1] - v[0];
	return 0;
}

static int read_(void *self, pf_probe_sample *out, int nports)
{
	ads_t *s = self;
	int ok = 0;
	/* the rail, from a port with nothing on it, once a second */
	double now = pf_now();
	if (now - s->ref_t >= REF_EVERY_S) {
		s->ref_t = now;
		int empty = -1;
		for (int i = 0; i < nports && i < 4; i++) if (out[i].kind == PF_SAMPLE_SKIP) { empty = i; break; }
		double ref = 0;
		if (empty >= 0 && read_channel(s, empty, REF_OVERSAMPLE, &ref, NULL) == 0 && s->vs_cfg > 0 && ref >= s->vs_cfg * 900.0 && ref <= s->vs_cfg * 1100.0) {
			double v = ref / 1000.0;
			s->vs_meas = s->vs_meas > 0 ? s->vs_meas + (v - s->vs_meas) * 0.2 : v;
		} else s->vs_meas = 0;
	}
	for (int i = 0; i < nports && i < 4; i++) {
		/* A port with no enabled probe on it is an open jack sitting at the rail. Converting it
		 * costs nine milliseconds and leaves the mux holding a voltage a long way from the one the
		 * pit probe is about to be measured at, which is the worst possible neighbour for the one
		 * reading that matters. Leave it alone. */
		if (out[i].kind == PF_SAMPLE_SKIP) continue;
		double mv, spread = 0;
		if (read_channel(s, i, OVERSAMPLE, &mv, &spread) == 0) {
			/* scaled to the configured reference, so the divider arithmetic downstream sees the
			 * rail the port was really fed from: Vo * Vcfg / Vmeas against Vcfg is Vo against Vmeas */
			if (s->vs_meas > 0) mv *= s->vs_cfg / s->vs_meas;
			out[i].kind = PF_SAMPLE_MV; out[i].value = mv; s->spread_mv[i] = spread; ok++;
		} else out[i].kind = PF_SAMPLE_INVALID;
	}
	if (!ok) {
		/* Nothing answered is not the same as nothing being asked for. A poll where every port was
		 * skipped is a success with no readings; a poll where a port was asked for and did not
		 * answer is the chip being unreachable, and has to be counted, or a dead ADC goes on
		 * reporting itself connected for ever. */
		bool asked = false;
		for (int i = 0; i < nports && i < 4; i++) if (out[i].kind != PF_SAMPLE_SKIP) asked = true;
		if (!asked) return 0;
		if (++s->errors == 5) s->env->log(PF_LVL_ERROR, "ads1x15", "no response from 0x%02x", s->addr);
		return -1;
	}
	s->errors = 0;
	return 0;
}

static int status_json(void *self, char *out, size_t n)
{
	ads_t *s = self;
	return snprintf(out, n, "{\"connected\":%s,\"address\":\"0x%02x\",\"timeouts\":%d,\"conversions\":%d,\"rail_v\":%.3f,\"noise_mv\":[%.2f,%.2f,%.2f,%.2f]}",
	                s->errors < 5 ? "true" : "false", s->addr, s->timeouts, s->nconv, s->vs_meas,
	                s->spread_mv[0], s->spread_mv[1], s->spread_mv[2], s->spread_mv[3]);
}

static const pf_probe_ops ops = {
	.abi = PF_PROBE_ABI, .id = "ads1x15", .name = "ADS1115 / ADS1015 ADC", .transient = false, .poll_ms = 250,
	.create = create, .destroy = destroy, .ports = ports, .read = read_, .status_json = status_json,
};
const pf_probe_ops *pf_probe_ads1x15(void) { return &ops; }
