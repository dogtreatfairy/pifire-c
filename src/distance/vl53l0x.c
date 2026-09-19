/* ST VL53L0X time-of-flight ranging sensor (I2C 0x29). Initialisation follows the well-known
 * Pololu sequence (SPAD configuration, tuning defaults, reference calibration); ranging is
 * single-shot with the default ~33 ms timing budget. */
#include "core/settings.h"
#include "core/util.h"
#include "distance/registry.h"
#include "hal/i2c.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const pf_env *env; int fd; uint8_t addr, stop_variable; int errors; } vl_t;

static int wr(vl_t *v, uint8_t reg, uint8_t val) { uint8_t b[2] = { reg, val }; return pf_i2c_write(v->fd, v->addr, b, 2); }
static int rd(vl_t *v, uint8_t reg) { uint8_t val = 0; return pf_i2c_write_read(v->fd, v->addr, &reg, 1, &val, 1) ? -1 : val; }
static int rd16(vl_t *v, uint8_t reg) { uint8_t b[2]; return pf_i2c_write_read(v->fd, v->addr, &reg, 1, b, 2) ? -1 : (b[0] << 8) | b[1]; }
static int wr16(vl_t *v, uint8_t reg, uint16_t val) { uint8_t b[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val }; return pf_i2c_write(v->fd, v->addr, b, 3); }

static bool wait_until_bit(vl_t *v, uint8_t reg, uint8_t mask, bool set, int timeout_ms)
{
	double deadline = pf_now() + timeout_ms / 1000.0;
	for (;;) {
		int r = rd(v, reg);
		if (r >= 0 && (((r & mask) != 0) == set)) return true;
		if (pf_now() > deadline) return false;
		pf_sleep_ms(2);
	}
}

static bool ref_calibration(vl_t *v, uint8_t vhv_init)
{
	wr(v, 0x00, 0x01 | vhv_init);
	if (!wait_until_bit(v, 0x13, 0x07, true, 500)) return false;
	wr(v, 0x0B, 0x01);
	wr(v, 0x00, 0x00);
	return true;
}

static bool init_sensor(vl_t *v)
{
	if (rd(v, 0xC0) != 0xEE) return false;                 /* model id */
	wr(v, 0x88, 0x00);
	wr(v, 0x80, 0x01); wr(v, 0xFF, 0x01); wr(v, 0x00, 0x00);
	v->stop_variable = (uint8_t)rd(v, 0x91);
	wr(v, 0x00, 0x01); wr(v, 0xFF, 0x00); wr(v, 0x80, 0x00);
	wr(v, 0x60, (uint8_t)(rd(v, 0x60) | 0x12));            /* disable SIGNAL_RATE_MSRC / PRE_RANGE limit checks */
	wr16(v, 0x44, (uint16_t)(0.25 * (1 << 7)));            /* signal rate limit 0.25 MCPS */
	wr(v, 0x01, 0xFF);                                     /* SYSTEM_SEQUENCE_CONFIG */

	/* SPAD info */
	wr(v, 0x80, 0x01); wr(v, 0xFF, 0x01); wr(v, 0x00, 0x00); wr(v, 0xFF, 0x06);
	wr(v, 0x83, (uint8_t)(rd(v, 0x83) | 0x04));
	wr(v, 0xFF, 0x07); wr(v, 0x81, 0x01); wr(v, 0x80, 0x01); wr(v, 0x94, 0x6B); wr(v, 0x83, 0x00);
	if (!wait_until_bit(v, 0x83, 0xFF, true, 500)) return false;
	wr(v, 0x83, 0x01);
	int tmp = rd(v, 0x92);
	int spad_count = tmp & 0x7F;
	bool spad_aperture = (tmp >> 7) & 1;
	wr(v, 0x81, 0x00); wr(v, 0xFF, 0x06);
	wr(v, 0x83, (uint8_t)(rd(v, 0x83) & ~0x04));
	wr(v, 0xFF, 0x01); wr(v, 0x00, 0x01); wr(v, 0xFF, 0x00); wr(v, 0x80, 0x00);

	uint8_t reg = 0xB0, map[6];
	if (pf_i2c_write_read(v->fd, v->addr, &reg, 1, map, 6)) return false;
	wr(v, 0xFF, 0x01); wr(v, 0x4F, 0x00); wr(v, 0x4E, 0x2C); wr(v, 0xFF, 0x00); wr(v, 0xB6, 0xB4);
	int first = spad_aperture ? 12 : 0, enabled = 0;
	for (int i = 0; i < 48; i++) {
		if (i < first || enabled == spad_count) map[i / 8] &= (uint8_t)~(1 << (i % 8));
		else if ((map[i / 8] >> (i % 8)) & 1) enabled++;
	}
	uint8_t out[7] = { 0xB0 };
	memcpy(out + 1, map, 6);
	pf_i2c_write(v->fd, v->addr, out, 7);

	/* default tuning settings */
	static const uint8_t tuning[][2] = {
		{0xFF,0x01},{0x00,0x00},{0xFF,0x00},{0x09,0x00},{0x10,0x00},{0x11,0x00},{0x24,0x01},{0x25,0xFF},{0x75,0x00},{0xFF,0x01},{0x4E,0x2C},{0x48,0x00},{0x30,0x20},
		{0xFF,0x00},{0x30,0x09},{0x54,0x00},{0x31,0x04},{0x32,0x03},{0x40,0x83},{0x46,0x25},{0x60,0x00},{0x27,0x00},{0x50,0x06},{0x51,0x00},{0x52,0x96},{0x56,0x08},
		{0x57,0x30},{0x61,0x00},{0x62,0x00},{0x64,0x00},{0x65,0x00},{0x66,0xA0},{0xFF,0x01},{0x22,0x32},{0x47,0x14},{0x49,0xFF},{0x4A,0x00},{0xFF,0x00},{0x7A,0x0A},
		{0x7B,0x00},{0x78,0x21},{0xFF,0x01},{0x23,0x34},{0x42,0x00},{0x44,0xFF},{0x45,0x26},{0x46,0x05},{0x40,0x40},{0x0E,0x06},{0x20,0x1A},{0x43,0x40},{0xFF,0x00},
		{0x34,0x03},{0x35,0x44},{0xFF,0x01},{0x31,0x04},{0x4B,0x09},{0x4C,0x05},{0x4D,0x04},{0xFF,0x00},{0x44,0x00},{0x45,0x20},{0x47,0x08},{0x48,0x28},{0x67,0x00},
		{0x70,0x04},{0x71,0x01},{0x72,0xFE},{0x76,0x00},{0x77,0x00},{0xFF,0x01},{0x0D,0x01},{0xFF,0x00},{0x80,0x01},{0x01,0xF8},{0xFF,0x01},{0x8E,0x01},{0x00,0x01},
		{0xFF,0x00},{0x80,0x00},
	};
	for (size_t i = 0; i < sizeof tuning / sizeof tuning[0]; i++) wr(v, tuning[i][0], tuning[i][1]);

	wr(v, 0x0A, 0x04);                                     /* interrupt on new sample */
	wr(v, 0x84, (uint8_t)(rd(v, 0x84) & ~0x10));           /* GPIO active low */
	wr(v, 0x0B, 0x01);
	wr(v, 0x01, 0xE8);                                     /* sequence: DSS, PRE_RANGE, FINAL_RANGE */
	wr(v, 0x01, 0x01); if (!ref_calibration(v, 0x40)) return false;   /* VHV */
	wr(v, 0x01, 0x02); if (!ref_calibration(v, 0x00)) return false;   /* phase */
	wr(v, 0x01, 0xE8);
	return true;
}

static double read_cm(void *self)
{
	vl_t *v = self;
	wr(v, 0x80, 0x01); wr(v, 0xFF, 0x01); wr(v, 0x00, 0x00); wr(v, 0x91, v->stop_variable); wr(v, 0x00, 0x01); wr(v, 0xFF, 0x00); wr(v, 0x80, 0x00);
	wr(v, 0x00, 0x01);                                     /* SYSRANGE_START single shot */
	if (!wait_until_bit(v, 0x00, 0x01, false, 200)) { v->errors++; return -1; }
	if (!wait_until_bit(v, 0x13, 0x07, true, 200)) { v->errors++; return -1; }
	int mm = rd16(v, 0x14 + 10);
	wr(v, 0x0B, 0x01);                                     /* clear interrupt */
	if (mm < 0) { v->errors++; return -1; }
	v->errors = 0;
	if (mm >= 8190) return -1;                              /* out of range */
	/* median of three for stability */
	return mm / 10.0;
}

static void *create(const char *cfg_json, const pf_env *env)
{
	cJSON *c = cJSON_Parse(cfg_json);
	vl_t *v = calloc(1, sizeof *v);
	v->env = env;
	v->addr = (uint8_t)strtoul(pf_json_str(c, "i2c_bus_addr", "0x29"), NULL, 0);
	int bus = pf_json_int(c, "i2c_bus", 1);
	cJSON_Delete(c);
	v->fd = pf_i2c_open(bus);
	if (v->fd < 0) { env->log(PF_LVL_ERROR, "vl53l0x", "cannot open /dev/i2c-%d", bus); free(v); return NULL; }
	if (!init_sensor(v)) { env->log(PF_LVL_ERROR, "vl53l0x", "no sensor at 0x%02x or init failed", v->addr); pf_i2c_close(v->fd); free(v); return NULL; }
	env->log(PF_LVL_INFO, "vl53l0x", "ready at 0x%02x", v->addr);
	return v;
}

static void destroy(void *self) { vl_t *v = self; pf_i2c_close(v->fd); free(v); }

static const pf_distance_ops ops = { .abi = PF_DISTANCE_ABI, .id = "vl53l0x", .name = "VL53L0X time-of-flight", .create = create, .destroy = destroy, .read_cm = read_cm };
const pf_distance_ops *pf_distance_vl53l0x(void) { return &ops; }
