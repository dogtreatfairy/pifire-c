/* iBBQ / xBBQ / Inkbird Bluetooth thermometers (4 or 6 probes). GATT service 0xFFF0:
 *   FFF2 login, FFF4 realtime temps (notify, uint16 LE tenths of °C per probe, 0xFFFF unplugged),
 *   FFF1 settings results (notify, 0x24 = battery frame), FFF5 commands. */
#include "core/settings.h"
#include "core/util.h"
#include "probes/ble/bluez.h"
#include "probes/registry.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const pf_env *env;
	pf_ble_dev *dev;
	int nprobes;
	pf_units units;
	pthread_mutex_t mu;
	double temp_c[6];
	bool valid[6];
	int batt;
	char u_fff1[40], u_fff2[40], u_fff4[40], u_fff5[40];
	char address[20];
} ibbq_t;

static const char *port_names[6] = { "BT0", "BT1", "BT2", "BT3", "BT4", "BT5" };

static const uint8_t LOGIN[]   = { 0x21, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0xb8, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t RT_ON[]   = { 0x0B, 0x01, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t UNITS_F[] = { 0x02, 0x01, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t UNITS_C[] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t BATT[]    = { 0x08, 0x24, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t X0823[]   = { 0x08, 0x23, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t X0825[]   = { 0x08, 0x25, 0x00, 0x00, 0x00, 0x00 };

static void on_connected(pf_ble_dev *d, void *ctx)
{
	ibbq_t *s = ctx;
	s->env->log(PF_LVL_INFO, "ibbq", "connected to %s (%s)", pf_ble_name(d), pf_ble_address(d));
	pf_ble_write(d, s->u_fff2, LOGIN, sizeof LOGIN, true);
	pf_ble_write(d, s->u_fff5, X0823, sizeof X0823, true);
	pf_ble_write(d, s->u_fff5, BATT, sizeof BATT, true);
	pf_ble_write(d, s->u_fff5, X0825, sizeof X0825, true);
	pf_ble_write(d, s->u_fff5, RT_ON, sizeof RT_ON, true);
	pf_ble_write(d, s->u_fff5, UNITS_F, sizeof UNITS_F, true); /* "secure mode" frame in the original */
	pf_ble_write(d, s->u_fff5, s->units == PF_UNITS_C ? UNITS_C : UNITS_F, 6, true);
	pf_ble_write(d, s->u_fff5, BATT, sizeof BATT, true);
}

static void on_disconnected(pf_ble_dev *d, void *ctx)
{
	(void)d;
	ibbq_t *s = ctx;
	pthread_mutex_lock(&s->mu);
	for (int i = 0; i < 6; i++) s->valid[i] = false;
	pthread_mutex_unlock(&s->mu);
}

static void on_value(pf_ble_dev *d, const char *uuid, const uint8_t *data, size_t len, void *ctx)
{
	(void)d;
	ibbq_t *s = ctx;
	if (!strcasecmp(uuid, s->u_fff4)) {
		pthread_mutex_lock(&s->mu);
		for (size_t i = 0; i < 6 && i * 2 + 1 < len; i++) {
			unsigned v = data[i * 2] | (data[i * 2 + 1] << 8);
			if (v == 0xFFFF || v == 65526) s->valid[i] = false;
			else { s->temp_c[i] = v / 10.0; s->valid[i] = true; }
		}
		pthread_mutex_unlock(&s->mu);
	} else if (!strcasecmp(uuid, s->u_fff1) && len >= 5 && data[0] == 0x24) {
		unsigned cur = data[1] | (data[2] << 8), max = data[3] | (data[4] << 8);
		if (max == 0) max = 6580;
		pthread_mutex_lock(&s->mu);
		s->batt = (int)(100.0 * cur / max);
		if (s->batt > 100) s->batt = 100;
		pthread_mutex_unlock(&s->mu);
	}
}

static void *create(const char *device_json, const pf_env *env)
{
	cJSON *d = cJSON_Parse(device_json);
	ibbq_t *s = calloc(1, sizeof *s);
	s->env = env;
	s->nprobes = pf_json_int(d, "config.num_probes", 4);
	if (s->nprobes < 1 || s->nprobes > 6) s->nprobes = 4;
	pf_strlcpy(s->address, pf_json_str(d, "config.hardware_id", ""), sizeof s->address);
	cJSON_Delete(d);
	s->units = pf_settings_units();
	s->batt = -1;
	pthread_mutex_init(&s->mu, NULL);
	pf_ble_uuid16("fff1", s->u_fff1); pf_ble_uuid16("fff2", s->u_fff2);
	pf_ble_uuid16("fff4", s->u_fff4); pf_ble_uuid16("fff5", s->u_fff5);
	pf_ble_spec spec = {
		.name_match = "BBQ", .address = s->address[0] ? s->address : NULL,
		.notify_uuids = { s->u_fff4, s->u_fff1, NULL },
		.on_connected = on_connected, .on_disconnected = on_disconnected, .on_value = on_value, .ctx = s,
	};
	s->dev = pf_ble_register(&spec);
	if (!s->dev) { env->log(PF_LVL_ERROR, "ibbq", "Bluetooth unavailable"); free(s); return NULL; }
	env->log(PF_LVL_INFO, "ibbq", "waiting for %s", s->address[0] ? s->address : "any iBBQ/xBBQ device");
	return s;
}

static void destroy(void *self) { ibbq_t *s = self; pf_ble_unregister(s->dev); free(s); }

static int ports(void *self, const char **names, int max)
{
	ibbq_t *s = self;
	int n = s->nprobes < max ? s->nprobes : max;
	for (int i = 0; i < n; i++) names[i] = port_names[i];
	return s->nprobes;
}

static int read_(void *self, pf_probe_sample *out, int nports)
{
	ibbq_t *s = self;
	bool conn = pf_ble_connected(s->dev);
	pthread_mutex_lock(&s->mu);
	for (int i = 0; i < nports && i < 6; i++) {
		if (conn && s->valid[i]) { out[i].kind = PF_SAMPLE_CELSIUS; out[i].value = s->temp_c[i]; }
		else out[i].kind = PF_SAMPLE_INVALID;
	}
	pthread_mutex_unlock(&s->mu);
	return conn ? 0 : -1;
}

static int status_json(void *self, char *out, size_t n)
{
	ibbq_t *s = self;
	int b = s->batt >= 0 ? s->batt : pf_ble_battery(s->dev);
	return snprintf(out, n, "{\"connected\":%s,\"battery\":%d,\"hardware_id\":\"%s\",\"name\":\"%s\"}",
	                pf_ble_connected(s->dev) ? "true" : "false", b, pf_ble_address(s->dev), pf_ble_name(s->dev));
}

static void set_units(void *self, pf_units u)
{
	ibbq_t *s = self;
	s->units = u;
	if (pf_ble_connected(s->dev)) pf_ble_write(s->dev, s->u_fff5, u == PF_UNITS_C ? UNITS_C : UNITS_F, 6, true);
}

static const pf_probe_ops ops = {
	.abi = PF_PROBE_ABI, .id = "ibbq", .name = "iBBQ / Inkbird Bluetooth", .transient = true, .poll_ms = 1000,
	.create = create, .destroy = destroy, .ports = ports, .read = read_, .status_json = status_json, .set_units = set_units,
};
const pf_probe_ops *pf_probe_ibbq(void) { return &ops; }
