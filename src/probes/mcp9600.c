/* Microchip MCP9600 thermocouple amplifier (I2C). Hot-junction temperature in °C. */
#include "hal/i2c.h"
#include "core/settings.h"
#include "probes/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_HOT_JUNCTION 0x00
#define REG_STATUS       0x04
#define REG_TC_CONFIG    0x05

typedef struct { const pf_env *env; int fd; uint8_t addr; int errors; } mcp_t;
static const char *port_names[1] = { "KTT0" };

static void *create(const char *device_json, const pf_env *env)
{
	cJSON *d = cJSON_Parse(device_json);
	mcp_t *s = calloc(1, sizeof *s);
	if (!s) { cJSON_Delete(d); return NULL; }
	s->env = env;
	s->addr = (uint8_t)strtoul(pf_json_str(d, "config.i2c_bus_addr", "0x67"), NULL, 0);
	const char *type = pf_json_str(d, "config.tc_type", "K");
	int bus = pf_json_int(d, "config.i2c_bus", 1);
	cJSON_Delete(d);
	s->fd = pf_i2c_open(bus);
	if (s->fd < 0) { env->log(PF_LVL_ERROR, "mcp9600", "cannot open /dev/i2c-%d", bus); free(s); return NULL; }
	static const char types[] = "KJTNSEBR";
	const char *p = strchr(types, type[0]);
	uint8_t tc = (uint8_t)((p ? (p - types) : 0) << 4); /* type in bits 6:4, filter 0 */
	uint8_t w[2] = { REG_TC_CONFIG, tc };
	pf_i2c_write(s->fd, s->addr, w, 2);
	env->log(PF_LVL_INFO, "mcp9600", "type %c at 0x%02x", type[0], s->addr);
	return s;
}

static void destroy(void *self) { mcp_t *s = self; pf_i2c_close(s->fd); free(s); }
static int ports(void *self, const char **names, int max) { (void)self; if (max > 0) names[0] = port_names[0]; return 1; }

static int read_(void *self, pf_probe_sample *out, int nports)
{
	mcp_t *s = self;
	if (nports < 1) return 0;
	out[0].kind = PF_SAMPLE_INVALID;
	uint8_t reg = REG_HOT_JUNCTION, r[2];
	if (pf_i2c_write_read(s->fd, s->addr, &reg, 1, r, 2)) { if (++s->errors == 5) s->env->log(PF_LVL_ERROR, "mcp9600", "no response from 0x%02x", s->addr); return -1; }
	s->errors = 0;
	uint8_t st = 0;
	reg = REG_STATUS;
	if (pf_i2c_write_read(s->fd, s->addr, &reg, 1, &st, 1) == 0 && (st & 0x10)) return 0; /* input range / open */
	double t = ((int16_t)((r[0] << 8) | r[1])) / 16.0;
	out[0].kind = PF_SAMPLE_CELSIUS;
	out[0].value = t;
	return 0;
}

static int status_json(void *self, char *out, size_t n)
{
	mcp_t *s = self;
	return snprintf(out, n, "{\"connected\":%s,\"address\":\"0x%02x\"}", s->errors < 5 ? "true" : "false", s->addr);
}

static const pf_probe_ops ops = {
	.abi = PF_PROBE_ABI, .id = "mcp9600", .name = "MCP9600 Thermocouple", .transient = false, .poll_ms = 500,
	.create = create, .destroy = destroy, .ports = ports, .read = read_, .status_json = status_json,
};
const pf_probe_ops *pf_probe_mcp9600(void) { return &ops; }
