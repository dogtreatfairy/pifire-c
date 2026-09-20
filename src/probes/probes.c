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

static int find_port(const device_t *d, const char *port)
{
	for (int i = 0; i < d->nports; i++)
		if (!strcmp(d->port_names[i], port)) return i;
	return -1;
}

static void teardown(void)
{
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
		r->ambient = pf_json_bool(pi, "ambient", false) || (r->role == PF_PROBE_AUX && strcasestr(r->name, "ambient") != NULL);
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
		pf_tempq_init(&pv->q, 2.64); /* PiFire's 4.75 F outlier gate, in C */
		if (r->role == PF_PROBE_PRIMARY && r->enabled && g_snap.primary < 0) g_snap.primary = n;
		n++;
	}
	g_snap.n = n;
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

	/* I/O outside the lock: device reads may block briefly */
	for (int i = 0; i < g_ndev; i++) {
		device_t *dev = &g_dev[i];
		if (!dev->inst) continue;
		if (now < dev->next_poll) continue;
		dev->next_poll = now + (dev->ops->poll_ms > 0 ? dev->ops->poll_ms : 250) / 1000.0;
		int rc = dev->ops->read(dev->inst, samples[i], dev->nports);
		if (rc < 0) { for (int p = 0; p < dev->nports; p++) samples[i][p].kind = PF_SAMPLE_INVALID; }
		polled[i] = true;
	}

	pthread_mutex_lock(&g_mu);
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
		r->raw_c = c;
		r->ohms = ohms > 0 ? ohms : 0;
		if (!isnan(c)) {
			r->temp_c = pf_tempq_push(&pv->q, c);
			r->valid = true;
			r->last_valid_t = now;
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
