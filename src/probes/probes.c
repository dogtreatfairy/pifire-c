#define _GNU_SOURCE
#include "probes/probes.h"
#include "core/env.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "probes/registry.h"
#include "probes/shh.h"
#include "probes/tempq.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "probes"

typedef struct {
	char name[PF_LABEL_LEN];
	const pf_probe_ops *ops;
	void *inst;
	pf_env env;
	int nports;
	const char *port_names[PF_MAX_PORTS];
	double rd[PF_MAX_PORTS];   /* resistor divider per port */
	double vs;                 /* reference volts */
	double next_poll;
	bool ok;
	bool wireless;             /* ops->link present */
	int rssi, battery;         /* last link() result */
} device_t;

typedef struct {
	int dev;                   /* index into devices */
	int port;                  /* index into device ports */
	pf_shh shh;
	pf_tempq q;
} probe_priv_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static device_t g_dev[PF_MAX_DEVICES];
static int g_ndev;
static pf_sensors g_snap;
static probe_priv_t g_priv[PF_MAX_PROBES];
static bool g_cooking;

/* Device reads happen outside the lock, because a Bluetooth read can block for a moment and the
 * whole probe layer must not stall behind it. That leaves a window: a settings reload tears the
 * devices down and frees the very instance a read is using. This counts the reads in flight, and
 * teardown waits for them to finish before freeing anything. */
static int g_reading;
static pthread_cond_t g_idle = PTHREAD_COND_INITIALIZER;

void pf_probes_set_cooking(bool cooking)
{
	pthread_mutex_lock(&g_mu);
	if (!cooking && g_cooking)
		for (int i = 0; i < g_snap.n; i++) g_snap.p[i].in_use = false;   /* the cook is over */
	g_cooking = cooking;
	pthread_mutex_unlock(&g_mu);
}

static int find_port(const device_t *d, const char *port)
{
	for (int i = 0; i < d->nports; i++)
		if (!strcmp(d->port_names[i], port)) return i;
	return -1;
}

/* caller holds g_mu */
static void teardown(void)
{
	while (g_reading > 0) pthread_cond_wait(&g_idle, &g_mu);   /* let the readers out first */
	for (int i = 0; i < g_ndev; i++)
		if (g_dev[i].inst && g_dev[i].ops->destroy) g_dev[i].ops->destroy(g_dev[i].inst);
	memset(g_dev, 0, sizeof g_dev);
	g_ndev = 0;
	memset(&g_snap, 0, sizeof g_snap);
	g_snap.primary = -1;
}

int pf_probes_init(void)
{
	cJSON *map = pf_set_dup("probe_settings.probe_map");
	cJSON *profiles = pf_set_dup("probe_settings.probe_profiles");
	pthread_mutex_lock(&g_mu);
	teardown();
	if (!map) { pthread_mutex_unlock(&g_mu); cJSON_Delete(profiles); LOGE(TAG, "no probe_map in settings"); return -1; }

	cJSON *devs = cJSON_GetObjectItemCaseSensitive(map, "probe_devices");
	cJSON *d;
	cJSON_ArrayForEach(d, devs) {
		if (g_ndev >= PF_MAX_DEVICES) break;
		const char *module = pf_json_str(d, "module", "");
		const char *name = pf_json_str(d, "device", module);
		const pf_probe_ops *ops = pf_probe_driver_find(module);
		if (!ops) { LOGE(TAG, "device '%s': unknown module '%s'", name, module); continue; }
		device_t *dev = &g_dev[g_ndev];
		pf_strlcpy(dev->name, name, sizeof dev->name);
		dev->ops = ops;
		char ns[96];
		snprintf(ns, sizeof ns, "probe.%s", name);
		pf_env_init(&dev->env, ns);
		char *json = cJSON_PrintUnformatted(d);
		dev->inst = ops->create(json, &dev->env);
		free(json);
		if (!dev->inst) { LOGE(TAG, "device '%s' (%s) failed to initialise", name, module); continue; }
		dev->nports = ops->ports(dev->inst, dev->port_names, PF_MAX_PORTS);
		if (dev->nports > PF_MAX_PORTS) dev->nports = PF_MAX_PORTS;
		dev->vs = pf_json_num(d, "config.voltage_ref", 3.28);
		for (int p = 0; p < dev->nports; p++) {
			char key[48];
			snprintf(key, sizeof key, "config.%s_rd", dev->port_names[p]);
			dev->rd[p] = pf_json_num(d, key, 10000);
		}
		dev->ok = true;
		g_ndev++;
		LOGI(TAG, "device '%s' (%s): %d ports", name, module, dev->nports);
	}

	cJSON *info = cJSON_GetObjectItemCaseSensitive(map, "probe_info");
	cJSON *pi;
	int n = 0;
	cJSON_ArrayForEach(pi, info) {
		if (n >= PF_MAX_PROBES) break;
		pf_probe_reading *r = &g_snap.p[n];
		probe_priv_t *pv = &g_priv[n];
		memset(r, 0, sizeof *r);
		memset(pv, 0, sizeof *pv);
		pf_strlcpy(r->label, pf_json_str(pi, "label", "?"), sizeof r->label);
		pf_strlcpy(r->name, pf_json_str(pi, "name", r->label), sizeof r->name);
		pf_strlcpy(r->device, pf_json_str(pi, "device", ""), sizeof r->device);
		pf_strlcpy(r->port, pf_json_str(pi, "port", ""), sizeof r->port);
		const char *type = pf_json_str(pi, "type", "Food");
		r->role = !strcasecmp(type, "Primary") ? PF_PROBE_PRIMARY : !strcasecmp(type, "Aux") ? PF_PROBE_AUX : PF_PROBE_FOOD;
		r->enabled = pf_json_bool(pi, "enabled", true);
		r->home = pf_json_bool(pi, "show_on_home", true);
		/* The explicit flag only. Whether an Aux probe named "ambient" is the ambient REFERENCE --
		 * the outdoor temperature the feed-forward and the cold start are judged against -- cannot
		 * be decided here, because the ambient sensor built into a Bluetooth food probe is also
		 * called ambient and measures the air a few inches from the meat. That is settled below,
		 * once it is known which probes are companions of another. */
		r->ambient = pf_json_bool(pi, "ambient", false);
		r->temp_c = NAN;
		r->raw_c = NAN;
		pv->dev = -1;
		for (int i = 0; i < g_ndev; i++)
			if (!strcmp(g_dev[i].name, r->device)) { pv->dev = i; pv->port = find_port(&g_dev[i], r->port); break; }
		if (pv->dev < 0 || pv->port < 0) LOGW(TAG, "probe '%s': device '%s' port '%s' not available", r->label, r->device, r->port);

		/* profile may be an id string or an inline {A,B,C} object */
		cJSON *prof = cJSON_GetObjectItemCaseSensitive(pi, "profile");
		cJSON *pobj = cJSON_IsString(prof) && profiles ? cJSON_GetObjectItemCaseSensitive(profiles, prof->valuestring) : prof;
		pv->shh.A = pf_json_num(pobj, "A", 0);
		pv->shh.B = pf_json_num(pobj, "B", 0);
		pv->shh.C = pf_json_num(pobj, "C", 0);
		pf_tempq_init(&pv->q);
		if (r->role == PF_PROBE_PRIMARY && r->enabled && g_snap.primary < 0) g_snap.primary = n;
		n++;
	}
	g_snap.n = n;
	/* wireless devices with an ambient port (Chef iQ BT_Ambient, MEATER BT_Ambient): pair that reading
	 * with the meat reading of the same device so the UI shows both inside one card */
	for (int i = 0; i < n; i++) {
		pf_probe_reading *r = &g_snap.p[i];
		r->companion = -1;
		r->battery = -1;
		if (g_priv[i].dev < 0 || !g_dev[g_priv[i].dev].ops->link || strcasestr(r->port, "Ambient")) continue;
		for (int j = 0; j < n; j++) {
			pf_probe_reading *a = &g_snap.p[j];
			if (j == i || g_priv[j].dev != g_priv[i].dev || !a->enabled || a->role == PF_PROBE_PRIMARY || !strcasestr(a->port, "Ambient")) continue;
			r->companion = j;
			a->is_companion = true;
			break;
		}
	}
	/* An Aux probe named "ambient" is the ambient reference unless it belongs to a food probe: the
	 * air inside the grill beside a piece of meat is not the weather, and feeding it to the learning
	 * as the outdoor temperature would teach the grill that it is 200 degrees outside. */
	for (int i = 0; i < n; i++) {
		pf_probe_reading *r = &g_snap.p[i];
		if (r->ambient || r->role != PF_PROBE_AUX || r->is_companion || !strcasestr(r->name, "ambient")) continue;
		/* Sharing a device with another probe is enough to disqualify it, whether or not the pairing
		 * above succeeded: a two-port device is a food probe with an air sensor on it, and if its
		 * driver failed to load we would otherwise promote that air sensor to "the weather" exactly
		 * when there is least reason to trust it. */
		bool shared = false;
		for (int j = 0; j < n && !shared; j++) shared = j != i && r->device[0] && !strcmp(g_snap.p[j].device, r->device);
		if (!shared) r->ambient = true;
	}
	pthread_mutex_unlock(&g_mu);
	cJSON_Delete(map);
	cJSON_Delete(profiles);
	LOGI(TAG, "%d probes configured, primary=%s", n, g_snap.primary >= 0 ? g_snap.p[g_snap.primary].label : "none");
	return 0;
}

void pf_probes_shutdown(void)
{
	pthread_mutex_lock(&g_mu);
	teardown();
	pthread_mutex_unlock(&g_mu);
}

/* average / highest / lowest / median of the listed probes' current filtered values (lock held) */
static double virtual_value(device_t *dev, int self_index)
{
	const char *labels[8];
	int n = pf_virtual_inputs(dev->inst, labels, 8);
	double vals[8];
	int k = 0;
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < g_snap.n; j++)
			if (j != self_index && !strcmp(g_snap.p[j].label, labels[i]) && g_snap.p[j].valid) { vals[k++] = g_snap.p[j].temp_c; break; }
	}
	if (!k) return NAN;
	const char *mode = pf_virtual_mode(dev->inst);
	if (!strcmp(mode, "highest")) { double m = vals[0]; for (int i = 1; i < k; i++) if (vals[i] > m) m = vals[i]; return m; }
	if (!strcmp(mode, "lowest")) { double m = vals[0]; for (int i = 1; i < k; i++) if (vals[i] < m) m = vals[i]; return m; }
	if (!strcmp(mode, "median")) {
		for (int i = 1; i < k; i++) for (int j = i; j > 0 && vals[j - 1] > vals[j]; j--) { double t = vals[j]; vals[j] = vals[j - 1]; vals[j - 1] = t; }
		return k & 1 ? vals[k / 2] : 0.5 * (vals[k / 2 - 1] + vals[k / 2]);
	}
	double s = 0;
	for (int i = 0; i < k; i++) s += vals[i];
	return s / k;
}

void pf_probes_poll(double now)
{
	pf_probe_sample samples[PF_MAX_DEVICES][PF_MAX_PORTS];
	bool polled[PF_MAX_DEVICES] = { 0 };

	/* I/O outside the lock: device reads may block briefly. Registering as a reader first keeps a
	 * settings reload from freeing these instances while they are in use. */
	pthread_mutex_lock(&g_mu);
	int ndev = g_ndev;
	g_reading++;
	pthread_mutex_unlock(&g_mu);

	for (int i = 0; i < ndev; i++) {
		device_t *dev = &g_dev[i];
		if (!dev->inst) continue;
		if (now < dev->next_poll) continue;
		dev->next_poll = now + (dev->ops->poll_ms > 0 ? dev->ops->poll_ms : 250) / 1000.0;
		/* tell the driver which ports are worth reading: one with no enabled probe on it is not */
		for (int p = 0; p < dev->nports && p < PF_MAX_PORTS; p++) samples[i][p].kind = PF_SAMPLE_SKIP;
		for (int k = 0; k < g_snap.n; k++)
			if (g_snap.p[k].enabled && g_priv[k].dev == i && g_priv[k].port >= 0 && g_priv[k].port < dev->nports)
				samples[i][g_priv[k].port].kind = PF_SAMPLE_INVALID;
		int rc = dev->ops->read(dev->inst, samples[i], dev->nports);
		if (rc < 0) { for (int p = 0; p < dev->nports; p++) samples[i][p].kind = PF_SAMPLE_INVALID; }
		if (dev->ops->link) {
			int r = 0, b = -1;
			dev->wireless = dev->ops->link(dev->inst, &r, &b) == 0;
			dev->rssi = rc < 0 ? 0 : r;   /* unreachable device: no bars */
			dev->battery = b;
		}
		polled[i] = true;
	}

	pthread_mutex_lock(&g_mu);
	if (--g_reading == 0) pthread_cond_broadcast(&g_idle);
	g_snap.t = now;
	for (int n = 0; n < g_snap.n; n++) {
		pf_probe_reading *r = &g_snap.p[n];
		probe_priv_t *pv = &g_priv[n];
		if (!r->enabled || pv->dev < 0 || pv->port < 0 || !polled[pv->dev]) continue;
		device_t *dev = &g_dev[pv->dev];
		pf_probe_sample s = samples[pv->dev][pv->port];
		double c = NAN, ohms = 0;
		switch (s.kind) {
		case PF_SAMPLE_MV:
			ohms = pf_shh_mv_to_ohms(s.value, dev->rd[pv->port], dev->vs);
			if (ohms > 0) c = pf_shh_ohms_to_c(ohms, &pv->shh);
			break;
		case PF_SAMPLE_OHMS:
			ohms = s.value;
			c = pf_shh_ohms_to_c(ohms, &pv->shh);
			break;
		case PF_SAMPLE_CELSIUS:
			c = s.value;
			break;
		default: break;
		}
		if (!strcmp(dev->ops->id, "virtual")) c = virtual_value(dev, n);
		r->wireless = dev->wireless;
		r->rssi = dev->wireless ? dev->rssi : 0;
		r->battery = dev->wireless ? dev->battery : -1;
		r->raw_c = c;
		r->ohms = ohms > 0 ? ohms : 0;
		if (!isnan(c)) {
			r->temp_c = pf_tempq_push(&pv->q, c);
			r->valid = true;
			r->last_valid_t = now;
			/* it is reading while a cook is on, so it is part of the cook from here until the end */
			if (g_cooking && r->enabled) r->in_use = true;
			if (r->enabled && r->target_c > 0) r->in_use = true;   /* given a job, so it is in use */
		} else {
			r->valid = false;
			/* keep last filtered value visible briefly for the UI, but mark invalid */
			if (now - r->last_valid_t > 10) { r->temp_c = NAN; pf_tempq_reset(&pv->q); }
		}
	}
	pthread_mutex_unlock(&g_mu);
}

void pf_probes_snapshot(pf_sensors *out)
{
	pthread_mutex_lock(&g_mu);
	*out = g_snap;
	pthread_mutex_unlock(&g_mu);
}

cJSON *pf_probes_device_status(void)
{
	cJSON *arr = cJSON_CreateArray();
	pthread_mutex_lock(&g_mu);
	for (int i = 0; i < g_ndev; i++) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "device", g_dev[i].name);
		cJSON_AddStringToObject(o, "module", g_dev[i].ops->id);
		char buf[512] = "{}";
		if (g_dev[i].inst && g_dev[i].ops->status_json) g_dev[i].ops->status_json(g_dev[i].inst, buf, sizeof buf);
		cJSON *st = cJSON_Parse(buf);
		cJSON_AddItemToObject(o, "status", st ? st : cJSON_CreateObject());
		cJSON_AddItemToArray(arr, o);
	}
	pthread_mutex_unlock(&g_mu);
	return arr;
}

int pf_probes_find(const pf_sensors *s, const char *label)
{
	for (int i = 0; i < s->n; i++)
		if (!strcmp(s->p[i].label, label)) return i;
	return -1;
}
