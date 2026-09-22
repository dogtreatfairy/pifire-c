#define _GNU_SOURCE
#include "net/tailscale.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/util.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "tailscale"
#define HELPER "/usr/local/bin/pifire-tailscale"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_busy;
static char g_action[16], g_output[512];
static int g_last_rc = -1;
static bool g_have_result;

static void *runner(void *arg)
{
	char *verb = arg;
	pthread_setname_np(pthread_self(), "pf-tailscale");
	char port[16], hostname[64];
	snprintf(port, sizeof port, "%d", pf_set_int("web.port", 80));
	pf_set_str("network.tailscale_hostname", hostname, sizeof hostname, "pifire");
	for (char *q = hostname; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '-')) *q = '-';
	const char *argv[8] = { "sudo", "-n", HELPER, verb, NULL, NULL };
	if (!strcmp(verb, "up")) argv[4] = hostname;
	if (!strcmp(verb, "serve")) argv[4] = port;
	char out[512];
	int rc = pf_run_capture(argv, out, sizeof out, !strcmp(verb, "install") ? 600 : 60);
	LOGI(TAG, "%s -> rc %d: %.200s", verb, rc, out);
	pthread_mutex_lock(&g_mu);
	g_last_rc = rc;
	g_have_result = true;
	pf_strlcpy(g_output, out, sizeof g_output);
	pthread_mutex_unlock(&g_mu);
	if (rc == 0 && !strcmp(verb, "serve")) pf_set_put_bool("network.tailscale_https", true);
	if (rc == 0 && !strcmp(verb, "unserve")) pf_set_put_bool("network.tailscale_https", false);
	free(verb);
	atomic_store(&g_busy, false);
	return NULL;
}

int pf_tailscale_action(const char *verb, char *err, size_t n)
{
	static const char *const verbs[] = { "install", "up", "down", "logout", "serve", "unserve", NULL };
	bool ok = false;
	for (int i = 0; verbs[i]; i++) if (!strcmp(verbs[i], verb)) ok = true;
	if (!ok) { snprintf(err, n, "unknown action"); return -1; }
	pf_status st;
	pf_status_get(&st);
	if (st.sim) { snprintf(err, n, "not available in simulator"); return -1; }
	if (atomic_exchange(&g_busy, true)) { snprintf(err, n, "another Tailscale action is still running"); return -1; }
	pthread_mutex_lock(&g_mu);
	pf_strlcpy(g_action, verb, sizeof g_action);
	g_have_result = false;
	g_output[0] = 0;
	pthread_mutex_unlock(&g_mu);
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	char *arg = strdup(verb);
	if (!arg) { pthread_attr_destroy(&at); atomic_store(&g_busy, false); snprintf(err, n, "out of memory"); return -1; }
	if (pthread_create(&t, &at, runner, arg)) { free(arg); pthread_attr_destroy(&at); atomic_store(&g_busy, false); snprintf(err, n, "cannot start worker"); return -1; }
	pthread_attr_destroy(&at);
	return 0;
}

/* ---------------- cached summary for the status stream ---------------- */

static pthread_mutex_t g_brief_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_brief_configured, g_brief_online;
static char g_brief_name[128];
static double g_brief_t;
static atomic_bool g_brief_busy;

static void *brief_refresh(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-ts-brief");
	cJSON *j = pf_tailscale_status_json();
	const char *state = pf_json_str(j, "state", "");
	bool configured = cJSON_IsTrue(cJSON_GetObjectItem(j, "installed")) && strcmp(state, "NoState") && strcmp(state, "NeedsLogin");
	pthread_mutex_lock(&g_brief_mu);
	g_brief_configured = configured;
	g_brief_online = !strcmp(state, "Running") && pf_json_bool(j, "online", false);
	pf_strlcpy(g_brief_name, pf_json_str(j, "dns_name", ""), sizeof g_brief_name);
	g_brief_t = pf_now();
	pthread_mutex_unlock(&g_brief_mu);
	cJSON_Delete(j);
	atomic_store(&g_brief_busy, false);
	return NULL;
}

void pf_tailscale_brief(bool *configured, bool *online, char *name, size_t n)
{
	pthread_mutex_lock(&g_brief_mu);
	bool stale = pf_now() - g_brief_t > 30;
	if (configured) *configured = g_brief_configured;
	if (online) *online = g_brief_online;
	if (name) pf_strlcpy(name, g_brief_name, n);
	pthread_mutex_unlock(&g_brief_mu);
	if (!stale || atomic_exchange(&g_brief_busy, true)) return;
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, brief_refresh, NULL)) atomic_store(&g_brief_busy, false);
	pthread_attr_destroy(&at);
}

cJSON *pf_tailscale_status_json(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "busy", atomic_load(&g_busy));
	pthread_mutex_lock(&g_mu);
	cJSON_AddStringToObject(o, "last_action", g_action);
	if (g_have_result) { cJSON_AddBoolToObject(o, "last_ok", g_last_rc == 0); cJSON_AddStringToObject(o, "last_output", g_output); }
	pthread_mutex_unlock(&g_mu);
	char hostname[64];
	pf_set_str("network.tailscale_hostname", hostname, sizeof hostname, "pifire");
	cJSON_AddStringToObject(o, "hostname", hostname);
	cJSON_AddBoolToObject(o, "https", pf_set_bool("network.tailscale_https", false));
	cJSON_AddNumberToObject(o, "port", pf_set_int("web.port", 80));

	pf_status st;
	pf_status_get(&st);
	if (st.sim || !pf_file_exists(HELPER)) { cJSON_AddBoolToObject(o, "installed", false); cJSON_AddStringToObject(o, "state", st.sim ? "Simulator" : "NoHelper"); return o; }
	const char *argv[] = { "sudo", "-n", HELPER, "status", NULL };
	char *out = malloc(65536);
	if (!out) return o;
	int rc = pf_run_capture(argv, out, 65536, 15);
	cJSON *js = rc == 0 ? cJSON_Parse(out) : NULL;
	free(out);
	if (!js) { cJSON_AddBoolToObject(o, "installed", false); cJSON_AddStringToObject(o, "state", "Unknown"); return o; }
	cJSON *inst = cJSON_GetObjectItem(js, "installed");
	cJSON_AddBoolToObject(o, "installed", !cJSON_IsFalse(inst));
	cJSON_AddStringToObject(o, "state", pf_json_str(js, "BackendState", "NoState"));
	cJSON_AddStringToObject(o, "auth_url", pf_json_str(js, "AuthURL", ""));
	cJSON_AddStringToObject(o, "version", pf_json_str(js, "Version", ""));
	cJSON *ips = cJSON_GetObjectItem(js, "TailscaleIPs");
	cJSON_AddItemToObject(o, "ips", ips ? cJSON_Duplicate(ips, 1) : cJSON_CreateArray());
	cJSON *self = cJSON_GetObjectItem(js, "Self");
	char dns[128];
	pf_strlcpy(dns, pf_json_str(self, "DNSName", ""), sizeof dns);
	size_t dl = strlen(dns);
	if (dl && dns[dl - 1] == '.') dns[dl - 1] = 0;
	cJSON_AddStringToObject(o, "dns_name", dns);
	cJSON_AddBoolToObject(o, "online", pf_json_bool(self, "Online", false));
	cJSON_AddStringToObject(o, "tailnet", pf_json_str(cJSON_GetObjectItem(js, "CurrentTailnet"), "Name", ""));
	cJSON_Delete(js);
	return o;
}
