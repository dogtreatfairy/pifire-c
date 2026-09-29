#define _GNU_SOURCE
#include "net/netmgr.h"
#include "core/db.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "net/wifi.h"
#include "core/status.h"
#include "features/tuner.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
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
static char g_data_dir[256];
static bool g_sim;

void pf_netmgr_set_data_dir(const char *dir) { pf_strlcpy(g_data_dir, dir ? dir : "", sizeof g_data_dir); }

/* ---------------- the radio's watchdog ----------------
 * The Pi's Wi-Fi firmware can stop answering the driver altogether ("brcmf ... -110" in the kernel
 * log, SDIO checksum errors). NetworkManager goes on reporting a connection, nothing reaches the
 * grill -- not the app, not Tailscale, not SSH -- and nothing recovers it short of power: it
 * happened on 2026-09-29 in the middle of a cook. So once the grill has been online, it keeps
 * checking that it can reach its own gateway. When it cannot for a minute it asks whether the radio
 * has hung (a router that is down is NetworkManager's business, not this); a hung radio gets its
 * driver reloaded, which does not touch the cook; and if two reloads do not bring it back, the Pi
 * restarts and the cook resumes through the power-loss path. At most one such restart an hour. */
static bool gateway(char *gw, size_t n)
{
	char out[256] = "";
	const char *argv[] = { "ip", "route", "show", "default", NULL };
	if (pf_run_capture(argv, out, sizeof out, 5) != 0) return false;
	const char *v = strstr(out, "via ");
	if (!v) return false;
	v += 4;
	size_t l = strcspn(v, " \n");
	if (!l || l >= n) return false;
	memcpy(gw, v, l); gw[l] = 0;
	return true;
}
static bool reachable(void)
{
	char gw[64];
	if (!gateway(gw, sizeof gw)) return false;
	char out[128];
	const char *argv[] = { "ping", "-c", "1", "-W", "3", gw, NULL };
	return pf_run_capture(argv, out, sizeof out, 6) == 0;
}
static int helper(const char *verb, char *out, size_t n, int timeout_s)
{
	const char *argv[] = { "sudo", "-n", "/usr/local/bin/pifire-wifi-reset", verb, NULL };
	return pf_run_capture(argv, out, n, timeout_s);
}
static double last_reboot_wall(void)
{
	char p[320]; snprintf(p, sizeof p, "%s/.net_reboot", g_data_dir);
	char *t = pf_read_file(p, NULL);
	double v = t ? atof(t) : 0;
	free(t);
	return v;
}
static void watchdog(double now)
{
	static double next, fail_since;
	static int resets;
	static bool ever_online;
	if (g_sim || now < next) return;
	next = now + 15;
	if (g_state == PF_NET_ONLINE) ever_online = true;
	if (!ever_online || g_hs_active || g_state == PF_NET_HOTSPOT || g_state == PF_NET_CONNECTING) { fail_since = 0; return; }
	if (reachable()) {
		if (fail_since > 0) LOGI(TAG, "network reachable again after %.0f s", now - fail_since);
		fail_since = 0; resets = 0;
		return;
	}
	if (fail_since == 0) { fail_since = now; LOGW(TAG, "cannot reach the gateway"); return; }
	if (now - fail_since < 60) return;
	char out[256] = "";
	helper("diagnose", out, sizeof out, 20);
	if (strncmp(out, "hung", 4)) {
		/* the radio is answering: the router or the signal, which NetworkManager deals with */
		if ((int)(now - fail_since) % 600 < 15) LOGW(TAG, "no network for %.0f s; the radio is answering (%s)", now - fail_since, out);
		return;
	}
	if (resets < 2) {
		resets++;
		LOGW(TAG, "the Wi-Fi radio has stopped answering (%s); reloading its driver (try %d)", out, resets);
		if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "NET_RADIO_RESET", "The Wi-Fi radio stopped answering; its driver was reloaded");
		int rc = helper("reset", out, sizeof out, 90);
		LOGI(TAG, "radio reset -> %d: %.120s", rc, out);
		next = now + 30;
		return;
	}
	/* two reloads have not brought it back: restart the Pi, and resume */
	pf_status st;
	pf_status_get(&st);
	bool cooking = st.mode == PF_MODE_STARTUP || st.mode == PF_MODE_REIGNITE || st.mode == PF_MODE_SMOKE || st.mode == PF_MODE_HOLD;
	if (st.mode == PF_MODE_SHUTDOWN || st.mode == PF_MODE_PRIME || st.mode == PF_MODE_MANUAL || pf_tuner_active(NULL, NULL, NULL)) return;   /* after that */
	if (cooking && !pf_set_bool("safety.power_loss.recovery", true)) {
		if ((int)(now - fail_since) % 900 < 15) LOGW(TAG, "the radio is hung and power-loss recovery is off: carrying on without a network rather than restarting a cook that would not resume");
		return;
	}
	if (pf_wall() - last_reboot_wall() < 3600) {
		if ((int)(now - fail_since) % 900 < 15) LOGW(TAG, "the radio is hung, and the grill restarted for this within the hour; carrying on without a network");
		return;
	}
	char p[320], txt[64];
	snprintf(p, sizeof p, "%s/.net_reboot", g_data_dir);
	snprintf(txt, sizeof txt, "%.0f", pf_wall());
	pf_write_file_atomic(p, txt, strlen(txt));
	snprintf(p, sizeof p, "%s/restart_resume", g_data_dir);
	const char *why = "Wi-Fi radio not responding";
	pf_write_file_atomic(p, why, strlen(why));
	LOGW(TAG, "restarting the grill to recover the Wi-Fi radio%s", cooking ? "; the cook resumes" : "");
	if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "NET_RADIO_REBOOT", "Restarting to recover the Wi-Fi radio");
	helper("reboot", out, sizeof out, 5);   /* the helper outlives us; it forces the restart if need be */
}

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
			watchdog(pf_now());
		}
		pf_sleep_ms(1000);
	}
	return NULL;
}

int pf_netmgr_start(bool sim)
{
	g_sim = sim;
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

void pf_netmgr_brief(char *ip, size_t ipn, char *ssid, size_t ssidn, int *signal, bool *hotspot)
{
	pthread_mutex_lock(&g_mu);
	if (ip) pf_strlcpy(ip, g_ip, ipn);
	if (ssid) pf_strlcpy(ssid, g_hs_active ? g_hs_ssid : g_ssid, ssidn);
	if (signal) *signal = g_hs_active ? 0 : g_signal;
	if (hotspot) *hotspot = g_hs_active;
	pthread_mutex_unlock(&g_mu);
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
