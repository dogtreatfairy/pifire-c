/* Chef iQ CQ50 / CQ60 wireless probes: passive Bluetooth LE. The probe never needs a connection; it
 * broadcasts manufacturer-specific data under company id 0x05CD in three rotating packets:
 *   msg[0] low nibble = packet type (0 name, 1 temperatures, 3 status); protocol version
 *   major.minor.patch = msg[1]>>4, msg[1]&15, msg[0]>>4.
 *   V3 (major >= 3, verified on a CQ60 at 5.0.0): ambient[2:4] food[4:6] tip1..4[6:14]; battery and
 *   SoC temperature come in the status packet at [8] and [9].
 *   V2 (major 2): battery[2] soc[3] ambient[4:6] food[6:8] tip1..3[8:14].
 *   legacy: mac[2:8] battery[8] soc[9] food[10:12] ambient[12:14].
 * All temperatures are int16 LE tenths of a degree Celsius; readings outside -40..600 C are dropped.
 * Format after the chefiq-ble library (MIT) used by Home Assistant. */
#include "probes/ble/chefiq.h"
#include "core/settings.h"
#include "core/util.h"
#include "probes/ble/bluez.h"
#include "probes/registry.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PROBE_PAYLOAD 18

static double s16_tenths(const uint8_t *p)
{
	int v = p[0] | (p[1] << 8);
	if (v >= 0x8000) v -= 0x10000;
	double c = v / 10.0;
	return (c < -40 || c > 600) ? NAN : c;
}

int pf_chefiq_parse(const uint8_t *msg, size_t len, pf_chefiq_reading *out)
{
	memset(out, 0, sizeof *out);
	out->food_c = out->ambient_c = NAN;
	for (int i = 0; i < 4; i++) out->tip_c[i] = NAN;
	if (len < 2 || len > MAX_PROBE_PAYLOAD) return -1;   /* the iQ Sense hub uses the same id with a longer payload */
	out->packet_type = msg[0] & 0x0F;
	out->ver_major = msg[1] >> 4; out->ver_minor = msg[1] & 0x0F; out->ver_patch = msg[0] >> 4;
	if (out->packet_type != 0 && out->packet_type != 1 && out->packet_type != 3) return -1;
	int fmt = out->ver_major >= 3 ? 3 : out->ver_major == 2 ? 2 : 1;
	if (out->packet_type == 1) {
		int amb, food, tips, ntips;
		if (fmt == 3) { amb = 2; food = 4; tips = 6; ntips = 4; }
		else if (fmt == 2) { if (len < 14) return -1; out->has_battery = true; out->battery_pct = msg[2]; out->soc_c = msg[3]; amb = 4; food = 6; tips = 8; ntips = 3; }
		else { if (len < 14) return -1; out->has_battery = true; out->battery_pct = msg[8]; out->soc_c = msg[9]; amb = 12; food = 10; tips = 0; ntips = 0; }
		if ((size_t)amb + 2 <= len) out->ambient_c = s16_tenths(msg + amb);
		if ((size_t)food + 2 <= len) out->food_c = s16_tenths(msg + food);
		for (int i = 0; i < ntips && (size_t)tips + i * 2 + 2 <= len; i++) { out->tip_c[i] = s16_tenths(msg + tips + i * 2); out->ntips = i + 1; }
		out->has_temps = true;
	} else if (out->packet_type == 3) {
		if (fmt == 3 && len >= 10) { out->has_battery = true; out->battery_pct = msg[8]; out->soc_c = msg[9]; }
	}
	return 0;
}

/* ---------------- probe device ---------------- */

typedef struct {
	const pf_env *env;
	pf_ble_dev *dev;
	pthread_mutex_t mu;
	double food_c, ambient_c;
	bool valid;
	int battery;
	double last_update;
	char address[20];
} cq_t;

static const char *port_names[2] = { "BT_Food", "BT_Ambient" };

static void on_connected(pf_ble_dev *d, void *ctx)
{
	cq_t *s = ctx;
	s->env->log(PF_LVL_INFO, "chefiq", "receiving %s (%s)", pf_ble_name(d), pf_ble_address(d));
}

static void on_disconnected(pf_ble_dev *d, void *ctx)
{
	(void)d;
	cq_t *s = ctx;
	pthread_mutex_lock(&s->mu);
	s->valid = false;
	pthread_mutex_unlock(&s->mu);
	s->env->log(PF_LVL_WARN, "chefiq", "no advertisements for 60 s - probe off or out of range");
}

static void on_value(pf_ble_dev *d, const char *uuid, const uint8_t *data, size_t len, void *ctx)
{
	(void)d; (void)uuid;
	cq_t *s = ctx;
	pf_chefiq_reading r;
	if (pf_chefiq_parse(data, len, &r)) return;
	pthread_mutex_lock(&s->mu);
	if (r.has_temps) {
		if (!isnan(r.food_c)) s->food_c = r.food_c;
		if (!isnan(r.ambient_c)) s->ambient_c = r.ambient_c;
		s->valid = !isnan(r.food_c);
		s->last_update = pf_now();
	}
	if (r.has_battery) s->battery = r.battery_pct;
	pthread_mutex_unlock(&s->mu);
}

static void *create(const char *device_json, const pf_env *env)
{
	cJSON *d = cJSON_Parse(device_json);
	cq_t *s = calloc(1, sizeof *s);
	s->env = env;
	pthread_mutex_init(&s->mu, NULL);
	s->battery = -1;
	s->food_c = s->ambient_c = NAN;
	pf_strlcpy(s->address, pf_json_str(d, "config.hardware_id", ""), sizeof s->address);
	cJSON_Delete(d);
	pf_ble_spec spec = {
		.passive = true, .manufacturer_id = PF_CHEFIQ_MFR_ID, .name_match = "CQ", .address = s->address, .poll_ms = 1000,
		.on_connected = on_connected, .on_disconnected = on_disconnected, .on_value = on_value, .ctx = s,
	};
	s->dev = pf_ble_register(&spec);
	if (!s->dev) { env->log(PF_LVL_ERROR, "chefiq", "Bluetooth unavailable"); free(s); return NULL; }
	env->log(PF_LVL_INFO, "chefiq", "listening for Chef iQ probe %s", s->address[0] ? s->address : "(first found)");
	return s;
}

static void destroy(void *self)
{
	cq_t *s = self;
	pf_ble_unregister(s->dev);
	free(s);
}

static int ports(void *self, const char **names, int max)
{
	(void)self;
	for (int i = 0; i < 2 && i < max; i++) names[i] = port_names[i];
	return 2;
}

static int read_(void *self, pf_probe_sample *out, int nports)
{
	cq_t *s = self;
	pthread_mutex_lock(&s->mu);
	bool fresh = s->valid && pf_now() - s->last_update < 30;
	if (nports > 0) { out[0].kind = fresh ? PF_SAMPLE_CELSIUS : PF_SAMPLE_INVALID; out[0].value = s->food_c; }
	if (nports > 1) { out[1].kind = fresh && !isnan(s->ambient_c) ? PF_SAMPLE_CELSIUS : PF_SAMPLE_INVALID; out[1].value = s->ambient_c; }
	pthread_mutex_unlock(&s->mu);
	return fresh ? 0 : -1;
}

static int status_json(void *self, char *out, size_t n)
{
	cq_t *s = self;
	return snprintf(out, n, "{\"connected\":%s,\"battery\":%d,\"address\":\"%s\",\"name\":\"%s\",\"passive\":true}",
	                pf_ble_connected(s->dev) ? "true" : "false", s->battery, pf_ble_address(s->dev), pf_ble_name(s->dev));
}

static int link_(void *self, int *rssi, int *battery)
{
	cq_t *s = self;
	*rssi = pf_ble_rssi(s->dev);
	*battery = s->battery;
	return 0;
}

static const pf_probe_ops ops = {
	.abi = PF_PROBE_ABI, .id = "chefiq", .name = "Chef iQ Bluetooth (CQ50/CQ60)", .transient = true, .poll_ms = 1000,
	.create = create, .destroy = destroy, .ports = ports, .read = read_, .status_json = status_json, .link = link_,
};
const pf_probe_ops *pf_probe_chefiq(void) { return &ops; }
