#define _GNU_SOURCE
#include "net/netmgr.h"
#include "core/db.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "net/wifi.h"
#include "web/api.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TAG "net"

static pthread_t g_tid;
static atomic_bool g_run;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pf_net_state g_state = PF_NET_BOOT;
static char g_ssid[64], g_ip[32], g_err[256], g_hs_ssid[64], g_hs_psk[64];
static int g_signal;
static bool g_hs_active;
/* pending request */
static bool g_req_connect, g_req_hotspot, g_req_hotspot_on;
static char g_req_ssid[64], g_req_psk[64];

static const char *state_name(pf_net_state s)
{
	static const char *n[] = { "boot", "online", "hotspot", "connecting", "offline" };
	return n[s];
}

static void set_state(pf_net_state s)
{
	pthread_mutex_lock(&g_mu);
	g_state = s;
	pthread_mutex_unlock(&g_mu);
}

static void refresh(void)
{
	cJSON *st = pf_wifi_status();
	pthread_mutex_lock(&g_mu);
	pf_strlcpy(g_ssid, pf_json_str(st, "ssid", ""), sizeof g_ssid);
	pf_strlcpy(g_ip, pf_json_str(st, "ip", ""), sizeof g_ip);
	g_signal = pf_json_int(st, "signal", 0);
	g_hs_active = pf_json_bool(st, "hotspot", false);
	bool connected = pf_json_bool(st, "connected", false);
	if (g_state != PF_NET_CONNECTING) g_state = g_hs_active ? PF_NET_HOTSPOT : connected ? PF_NET_ONLINE : (g_state == PF_NET_BOOT ? PF_NET_BOOT : PF_NET_OFFLINE);
	pthread_mutex_unlock(&g_mu);
	cJSON_Delete(st);
	pf_api_set_hotspot_active(g_hs_active);
}

static void hotspot_up(void)
{
	if (pf_hotspot_start(g_hs_ssid, g_hs_psk) == 0) {
		if (pf_db_handle()) pf_db_event(PF_LVL_INFO, "NET_HOTSPOT", "Setup hotspot started");
	}
	refresh();
}

static void do_connect(void)
{
	char ssid[64], psk[64], err[256] = "";
	pthread_mutex_lock(&g_mu);
	pf_strlcpy(ssid, g_req_ssid, sizeof ssid);
	pf_strlcpy(psk, g_req_psk, sizeof psk);
	g_req_connect = false;
	g_state = PF_NET_CONNECTING;
	g_err[0] = 0;
	pthread_mutex_unlock(&g_mu);
	bool was_hotspot = pf_hotspot_is_up();
	LOGI(TAG, "joining '%s'%s", ssid, was_hotspot ? " (hotspot will drop)" : "");
	int rc = pf_wifi_connect(ssid, psk, err, sizeof err);
	if (rc == 0) {
		char msg[128];
		snprintf(msg, sizeof msg, "Joined Wi-Fi network %s", ssid);
		if (pf_db_handle()) pf_db_event(PF_LVL_INFO, "NET_JOINED", msg);
		set_state(PF_NET_ONLINE);
	} else {
		pthread_mutex_lock(&g_mu);
		snprintf(g_err, sizeof g_err, "Could not join %.60s: %.160s", ssid, err);
		pthread_mutex_unlock(&g_mu);
		set_state(PF_NET_OFFLINE);
		if (was_hotspot) hotspot_up();
	}
	refresh();
}

static void *net_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-net");
	double boot_deadline = pf_now() + pf_set_num("network.setup_timeout_s", 45);
	bool force = pf_set_bool("network.force_setup", false);
	refresh();
	while (atomic_load(&g_run)) {
		bool req_c, req_h, req_h_on;
		pthread_mutex_lock(&g_mu);
		req_c = g_req_connect; req_h = g_req_hotspot; req_h_on = g_req_hotspot_on; g_req_hotspot = false;
		pthread_mutex_unlock(&g_mu);

		if (req_c) do_connect();
		else if (req_h) {
			if (req_h_on) hotspot_up(); else { pf_hotspot_stop(); refresh(); }
		} else if (g_state == PF_NET_BOOT) {
			if (force) { LOGI(TAG, "setup mode forced by settings"); pf_set_put_bool("network.force_setup", false); pf_settings_save(); hotspot_up(); }
			else if (pf_now() > boot_deadline) { LOGW(TAG, "no network after boot wait; starting setup hotspot"); hotspot_up(); }
			else refresh();
		} else {
			static int tick;
			if (++tick % 10 == 0) refresh();
		}
		pf_sleep_ms(1000);
	}
	return NULL;
}

int pf_netmgr_start(bool sim)
{
	pf_wifi_init(sim);
	char ssid[64];
	pf_set_str("network.hotspot_ssid", ssid, sizeof ssid, "");
	if (!ssid[0]) pf_hotspot_default_ssid(ssid, sizeof ssid);
	pf_strlcpy(g_hs_ssid, ssid, sizeof g_hs_ssid);
	pf_set_str("network.hotspot_password", g_hs_psk, sizeof g_hs_psk, "pifire1234");
	if (strlen(g_hs_psk) < 8) pf_strlcpy(g_hs_psk, "pifire1234", sizeof g_hs_psk);
	atomic_store(&g_run, true);
	return pthread_create(&g_tid, NULL, net_thread, NULL);
}

void pf_netmgr_stop(void)
{
	if (!atomic_load(&g_run)) return;
	atomic_store(&g_run, false);
	pthread_join(g_tid, NULL);
}

cJSON *pf_netmgr_status(void)
{
	cJSON *o = cJSON_CreateObject();
	pthread_mutex_lock(&g_mu);
	cJSON_AddStringToObject(o, "state", state_name(g_state));
	cJSON_AddStringToObject(o, "ssid", g_ssid);
	cJSON_AddStringToObject(o, "ip", g_ip);
	cJSON_AddNumberToObject(o, "signal", g_signal);
	cJSON_AddStringToObject(o, "iface", pf_wifi_iface());
	cJSON_AddStringToObject(o, "last_error", g_err);
	cJSON *hs = cJSON_AddObjectToObject(o, "hotspot");
	cJSON_AddStringToObject(hs, "ssid", g_hs_ssid);
	cJSON_AddStringToObject(hs, "password", g_hs_psk);
	cJSON_AddBoolToObject(hs, "active", g_hs_active);
	pthread_mutex_unlock(&g_mu);
	return o;
}

int pf_netmgr_connect(const char *ssid, const char *psk)
{
	pthread_mutex_lock(&g_mu);
	int rc = -1;
	if (g_state != PF_NET_CONNECTING && !g_req_connect) {
		pf_strlcpy(g_req_ssid, ssid, sizeof g_req_ssid);
		pf_strlcpy(g_req_psk, psk ? psk : "", sizeof g_req_psk);
		g_req_connect = true;
		g_state = PF_NET_CONNECTING;
		g_err[0] = 0;
		rc = 0;
	}
	pthread_mutex_unlock(&g_mu);
	return rc;
}

int pf_netmgr_hotspot(bool on)
{
	pthread_mutex_lock(&g_mu);
	g_req_hotspot = true;
	g_req_hotspot_on = on;
	pthread_mutex_unlock(&g_mu);
	return 0;
}
