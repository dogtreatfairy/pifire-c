/* Apption Labs MEATER (Original and Pro). Temperature characteristic
 * 7edda774-045e-4bbf-909b-45d1991a2876 is polled at 1 Hz. Service UUID tells the generation:
 *   a75cc7fc-c956-488f-ac2a-2dbc08b63a04  Original: tip=u16[0], ambient from u16[1..2]; °C=(v+8)/16
 *   c9e2746c-59f1-4e54-a0dd-e1e54555cf8b  Pro: five int16 internals at 0..8, ambient at 10; °C=(v±8)/32
 * Battery comes from org.bluez.Battery1. */
#include "core/settings.h"
#include "core/util.h"
#include "probes/ble/bluez.h"
#include "probes/registry.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEMP_UUID "7edda774-045e-4bbf-909b-45d1991a2876"
#define SVC_ORIGINAL "a75cc7fc"
#define SVC_PRO "c9e2746c"

typedef struct {
	const pf_env *env;
	pf_ble_dev *dev;
	pthread_mutex_t mu;
	double tip_c, ambient_c;
	bool valid;
	bool pro;
	char address[20];
} meater_t;

static const char *port_names[2] = { "BT_Tip", "BT_Ambient" };

static int u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static int s16(const uint8_t *p) { int v = u16(p); return v >= 0x8000 ? v - 0x10000 : v; }

static void on_connected(pf_ble_dev *d, void *ctx)
{
	meater_t *s = ctx;
	s->pro = pf_ble_has_service(d, SVC_PRO) && !pf_ble_has_service(d, SVC_ORIGINAL);
	s->env->log(PF_LVL_INFO, "meater", "connected to %s (%s) - %s", pf_ble_name(d), pf_ble_address(d), s->pro ? "MEATER Pro" : "MEATER Original");
}

static void on_disconnected(pf_ble_dev *d, void *ctx)
{
	(void)d;
	meater_t *s = ctx;
	pthread_mutex_lock(&s->mu);
	s->valid = false;
	pthread_mutex_unlock(&s->mu);
}

static void on_value(pf_ble_dev *d, const char *uuid, const uint8_t *data, size_t len, void *ctx)
{
	(void)d; (void)uuid;
	meater_t *s = ctx;
	double tip, amb;
	if (s->pro) {
		if (len < 12) return;
		double mn = 1e9;
		for (int i = 0; i < 5; i++) {
			int v = s16(data + i * 2);
			double c = v > 0 ? (v + 8) / 32.0 : v < 0 ? (v - 8) / 32.0 : 0;
			if (c < mn) mn = c;
		}
		tip = mn;
		int a = s16(data + 10);
		amb = a > 0 ? (a + 8) / 32.0 : a < 0 ? (a - 8) / 32.0 : 0;
	} else {
		if (len < 6) return;
		int t = u16(data), ra = u16(data + 2), oa = u16(data + 4);
		int minoa = oa < 48 ? oa : 48;
		double ambient_raw = t + fmax(0, ((ra - minoa) * 16 * 589) / 1487.0);
		tip = (t + 8.0) / 16.0;
		amb = ((int)ambient_raw + 8.0) / 16.0;
	}
	pthread_mutex_lock(&s->mu);
	s->tip_c = tip;
	s->ambient_c = amb;
	s->valid = true;
	pthread_mutex_unlock(&s->mu);
}

static void *create(const char *device_json, const pf_env *env)
{
	cJSON *d = cJSON_Parse(device_json);
	meater_t *s = calloc(1, sizeof *s);
	s->env = env;
	pf_strlcpy(s->address, pf_json_str(d, "config.hardware_id", ""), sizeof s->address);
	cJSON_Delete(d);
	pthread_mutex_init(&s->mu, NULL);
	pf_ble_spec spec = {
		.name_match = "MEATER", .name_exclude = "MEATER+", .address = s->address[0] ? s->address : NULL,
		.poll_uuids = { TEMP_UUID, NULL }, .poll_ms = 1000,
		.on_connected = on_connected, .on_disconnected = on_disconnected, .on_value = on_value, .ctx = s,
	};
	s->dev = pf_ble_register(&spec);
	if (!s->dev) { env->log(PF_LVL_ERROR, "meater", "Bluetooth unavailable"); free(s); return NULL; }
	env->log(PF_LVL_INFO, "meater", "waiting for %s", s->address[0] ? s->address : "any MEATER probe");
	return s;
}

static void destroy(void *self) { meater_t *s = self; pf_ble_unregister(s->dev); free(s); }
static int ports(void *self, const char **names, int max) { (void)self; for (int i = 0; i < 2 && i < max; i++) names[i] = port_names[i]; return 2; }

static int read_(void *self, pf_probe_sample *out, int nports)
{
	meater_t *s = self;
	bool conn = pf_ble_connected(s->dev);
	pthread_mutex_lock(&s->mu);
	bool ok = conn && s->valid;
	if (nports > 0) { out[0].kind = ok ? PF_SAMPLE_CELSIUS : PF_SAMPLE_INVALID; out[0].value = s->tip_c; }
	if (nports > 1) { out[1].kind = ok ? PF_SAMPLE_CELSIUS : PF_SAMPLE_INVALID; out[1].value = s->ambient_c; }
	pthread_mutex_unlock(&s->mu);
	return conn ? 0 : -1;
}

static int status_json(void *self, char *out, size_t n)
{
	meater_t *s = self;
	return snprintf(out, n, "{\"connected\":%s,\"battery\":%d,\"hardware_id\":\"%s\",\"name\":\"%s\",\"model\":\"%s\"}",
	                pf_ble_connected(s->dev) ? "true" : "false", pf_ble_battery(s->dev), pf_ble_address(s->dev), pf_ble_name(s->dev), s->pro ? "Pro" : "Original");
}

static const pf_probe_ops ops = {
	.abi = PF_PROBE_ABI, .id = "meater", .name = "MEATER Bluetooth", .transient = true, .poll_ms = 1000,
	.create = create, .destroy = destroy, .ports = ports, .read = read_, .status_json = status_json,
};
const pf_probe_ops *pf_probe_meater(void) { return &ops; }
