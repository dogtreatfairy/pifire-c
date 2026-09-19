/* MAX31865 RTD-to-digital converter (SPI). Reports resistance; the profile maps ohms to °C. */
#include "hal/spi.h"
#include "core/settings.h"
#include "core/util.h"
#include "probes/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const pf_env *env;
	int fd;
	double ref_resistor;
	int errors;
	uint8_t fault;
} max_t;

static const char *port_names[1] = { "RTD0" };

static int reg_write(max_t *s, uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = { (uint8_t)(reg | 0x80), val }, rx[2];
	return pf_spi_xfer(s->fd, tx, rx, 2);
}

static int reg_read(max_t *s, uint8_t reg, uint8_t *buf, size_t n)
{
	uint8_t tx[8] = { reg }, rx[8];
	if (n > 7) n = 7;
	if (pf_spi_xfer(s->fd, tx, rx, n + 1)) return -1;
	memcpy(buf, rx + 1, n);
	return 0;
}

static void *create(const char *device_json, const pf_env *env)
{
	cJSON *d = cJSON_Parse(device_json);
	max_t *s = calloc(1, sizeof *s);
	s->env = env;
	int cs = pf_json_int(d, "config.cs", 1);
	int wires = pf_json_int(d, "config.wires", 2);
	s->ref_resistor = pf_json_num(d, "config.ref_resistor", 4300);
	cJSON_Delete(d);
	s->fd = pf_spi_open(0, cs, 1, 500000); /* mode 1 (CPOL 0, CPHA 1) */
	if (s->fd < 0) { env->log(PF_LVL_ERROR, "max31865", "cannot open /dev/spidev0.%d (is SPI enabled?)", cs); free(s); return NULL; }
	/* V_BIAS on, auto conversion, 3-wire if selected, clear faults, 60 Hz filter */
	uint8_t cfg = 0x80 | 0x40 | (wires == 3 ? 0x10 : 0) | 0x02;
	reg_write(s, 0x00, cfg);
	pf_sleep_ms(100);
	env->log(PF_LVL_INFO, "max31865", "ready on CE%d (%d-wire, Rref %.0f)", cs, wires, s->ref_resistor);
	return s;
}

static void destroy(void *self) { max_t *s = self; reg_write(s, 0x00, 0x00); pf_spi_close(s->fd); free(s); }

static int ports(void *self, const char **names, int max)
{
	(void)self;
	if (max > 0) names[0] = port_names[0];
	return 1;
}

static int read_(void *self, pf_probe_sample *out, int nports)
{
	max_t *s = self;
	if (nports < 1) return 0;
	uint8_t r[2];
	out[0].kind = PF_SAMPLE_INVALID;
	if (reg_read(s, 0x01, r, 2)) { if (++s->errors == 5) s->env->log(PF_LVL_ERROR, "max31865", "SPI read failed"); return -1; }
	s->errors = 0;
	uint16_t rtd = (uint16_t)(((r[0] << 8) | r[1]) >> 1);
	if (r[1] & 0x01) {
		uint8_t f = 0;
		reg_read(s, 0x07, &f, 1);
		s->fault = f;
		reg_write(s, 0x00, 0x80 | 0x40 | 0x02); /* clear fault */
		return 0;
	}
	s->fault = 0;
	if (rtd == 0 || rtd == 0x7fff) return 0; /* open / short */
	out[0].kind = PF_SAMPLE_OHMS;
	out[0].value = (double)rtd / 32768.0 * s->ref_resistor;
	return 0;
}

static int status_json(void *self, char *out, size_t n)
{
	max_t *s = self;
	return snprintf(out, n, "{\"connected\":%s,\"fault\":%u}", s->errors < 5 ? "true" : "false", s->fault);
}

static const pf_probe_ops ops = {
	.abi = PF_PROBE_ABI, .id = "max31865", .name = "MAX31865 RTD", .transient = false, .poll_ms = 250,
	.create = create, .destroy = destroy, .ports = ports, .read = read_, .status_json = status_json,
};
const pf_probe_ops *pf_probe_max31865(void) { return &ops; }
