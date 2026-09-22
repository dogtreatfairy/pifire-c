/* DS18B20 1-Wire thermometer via the kernel w1 bus (/sys/bus/w1/devices/28-*). A conversion takes
 * ~750 ms, so reads happen on a helper thread and read() returns the latest value. */
#define _GNU_SOURCE
#include "core/settings.h"
#include "core/util.h"
#include "probes/registry.h"
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const pf_env *env;
	char id[32];         /* configured hardware id or "" for first found */
	char found[32];
	pthread_t tid;
	atomic_bool run;
	pthread_mutex_t mu;
	double temp_c;
	bool valid;
	double last_ok;
} ds_t;
static const char *port_names[1] = { "DS0" };

static bool find_device(ds_t *s)
{
	DIR *d = opendir("/sys/bus/w1/devices");
	if (!d) return false;
	struct dirent *e;
	bool ok = false;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "28-", 3)) continue;
		if (!s->id[0] || !strcmp(s->id, e->d_name)) { pf_strlcpy(s->found, e->d_name, sizeof s->found); ok = true; break; }
	}
	closedir(d);
	return ok;
}

static void *worker(void *arg)
{
	ds_t *s = arg;
	pthread_setname_np(pthread_self(), "pf-ds18b20");
	while (atomic_load(&s->run)) {
		if (!s->found[0] && !find_device(s)) { pf_sleep_ms(5000); continue; }
		char path[128];
		snprintf(path, sizeof path, "/sys/bus/w1/devices/%s/temperature", s->found);
		char *txt = pf_read_file(path, NULL);   /* blocks ~750 ms during conversion */
		double t = NAN;
		if (txt && *txt) t = atof(txt) / 1000.0;
		free(txt);
		pthread_mutex_lock(&s->mu);
		if (!isnan(t) && t > -55 && t < 125 && t != 85.0) { s->temp_c = t; s->valid = true; s->last_ok = pf_now(); }
		else if (pf_now() - s->last_ok > 10) { s->valid = false; if (!txt) s->found[0] = 0; }
		pthread_mutex_unlock(&s->mu);
		pf_sleep_ms(1000);
	}
	return NULL;
}

static void *create(const char *device_json, const pf_env *env)
{
	cJSON *d = cJSON_Parse(device_json);
	ds_t *s = calloc(1, sizeof *s);
	if (!s) { cJSON_Delete(d); return NULL; }
	s->env = env;
	pf_strlcpy(s->id, pf_json_str(d, "config.hardware_id", ""), sizeof s->id);
	cJSON_Delete(d);
	pthread_mutex_init(&s->mu, NULL);
	atomic_store(&s->run, true);
	pthread_create(&s->tid, NULL, worker, s);
	env->log(PF_LVL_INFO, "ds18b20", "watching 1-Wire bus%s%s", s->id[0] ? " for " : "", s->id);
	return s;
}

static void destroy(void *self)
{
	ds_t *s = self;
	atomic_store(&s->run, false);
	pthread_join(s->tid, NULL);
	free(s);
}

static int ports(void *self, const char **names, int max) { (void)self; if (max > 0) names[0] = port_names[0]; return 1; }

static int read_(void *self, pf_probe_sample *out, int nports)
{
	ds_t *s = self;
	if (nports < 1) return 0;
	pthread_mutex_lock(&s->mu);
	if (s->valid) { out[0].kind = PF_SAMPLE_CELSIUS; out[0].value = s->temp_c; }
	else out[0].kind = PF_SAMPLE_INVALID;
	pthread_mutex_unlock(&s->mu);
	return 0;
}

static int status_json(void *self, char *out, size_t n)
{
	ds_t *s = self;
	return snprintf(out, n, "{\"connected\":%s,\"hardware_id\":\"%s\"}", s->valid ? "true" : "false", s->found);
}

static const pf_probe_ops ops = {
	.abi = PF_PROBE_ABI, .id = "ds18b20", .name = "DS18B20 1-Wire", .transient = true, .poll_ms = 1000,
	.create = create, .destroy = destroy, .ports = ports, .read = read_, .status_json = status_json,
};
const pf_probe_ops *pf_probe_ds18b20(void) { return &ops; }
