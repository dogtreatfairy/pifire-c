#define _GNU_SOURCE
#include "net/wifi.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "wifi"
#define HOTSPOT_CON "PiFire-Hotspot"

static bool g_sim;
static char g_iface[16] = "wlan0";
static bool g_sim_hotspot;
static char g_sim_ssid[64] = "SimNet";

void pf_wifi_init(bool sim)
{
	g_sim = sim;
	if (sim) return;
	char out[2048];
	const char *argv[] = { "nmcli", "-t", "-f", "DEVICE,TYPE", "dev", "status", NULL };
	if (pf_run_capture(argv, out, sizeof out, 10) == 0) {
		char *save = NULL;
		for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char *colon = strchr(line, ':');
			if (colon && !strcmp(colon + 1, "wifi")) { *colon = 0; pf_strlcpy(g_iface, line, sizeof g_iface); break; }
		}
	}
	LOGI(TAG, "wifi interface: %s", g_iface);
}

const char *pf_wifi_iface(void) { return g_iface; }

static const char *iface_ip(const char *iface)
{
	static char ip[INET_ADDRSTRLEN];
	ip[0] = 0;
	struct ifaddrs *ifa = NULL;
	if (getifaddrs(&ifa) != 0) return ip;
	for (struct ifaddrs *p = ifa; p; p = p->ifa_next)
		if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !strcmp(p->ifa_name, iface)) {
			inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, ip, sizeof ip);
			break;
		}
	freeifaddrs(ifa);
	return ip;
}

/* nmcli -t escapes ':' in fields as '\:' */
static void unescape(char *s)
{
	char *w = s;
	for (; *s; s++) { if (*s == '\\' && s[1]) s++; *w++ = *s; }
	*w = 0;
}

static int split_fields(char *line, char **f, int max)
{
	int n = 0;
	char *p = line;
	f[n++] = p;
	while (*p && n < max) {
		if (*p == '\\' && p[1]) { p += 2; continue; }
		if (*p == ':') { *p = 0; f[n++] = p + 1; }
		p++;
	}
	for (int i = 0; i < n; i++) unescape(f[i]);
	return n;
}

cJSON *pf_wifi_scan(bool rescan)
{
	cJSON *arr = cJSON_CreateArray();
	if (g_sim) {
		const char *names[] = { "SimNet", "Neighbors 5G", "CoffeeShop" };
		int sig[] = { 82, 55, 30 };
		for (int i = 0; i < 3; i++) {
			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "ssid", names[i]);
			cJSON_AddNumberToObject(o, "signal", sig[i]);
			cJSON_AddStringToObject(o, "security", i == 2 ? "" : "WPA2");
			cJSON_AddBoolToObject(o, "active", !g_sim_hotspot && !strcmp(names[i], g_sim_ssid));
			cJSON_AddItemToArray(arr, o);
		}
		return arr;
	}
	char *out = malloc(65536);
	const char *argv[] = { "nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL,SECURITY", "dev", "wifi", "list", "--rescan", rescan ? "yes" : "auto", NULL };
	int rc = pf_run_capture(argv, out, 65536, rescan ? 25 : 10);
	if (rc != 0) { LOGW(TAG, "nmcli wifi list failed (%d): %.120s", rc, out); free(out); return arr; }
	char *save = NULL;
	for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[4];
		if (split_fields(line, f, 4) < 4 || !f[1][0]) continue;
		/* dedupe by ssid keeping the strongest */
		cJSON *dup = NULL, *it;
		cJSON_ArrayForEach(it, arr) if (!strcmp(pf_json_str(it, "ssid", ""), f[1])) { dup = it; break; }
		int sig = atoi(f[2]);
		if (dup) { if (sig > pf_json_int(dup, "signal", 0)) cJSON_SetNumberValue(cJSON_GetObjectItem(dup, "signal"), sig); if (!strcmp(f[0], "yes")) cJSON_ReplaceItemInObject(dup, "active", cJSON_CreateBool(true)); continue; }
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "ssid", f[1]);
		cJSON_AddNumberToObject(o, "signal", sig);
		cJSON_AddStringToObject(o, "security", f[3]);
		cJSON_AddBoolToObject(o, "active", !strcmp(f[0], "yes"));
		cJSON_AddItemToArray(arr, o);
	}
	free(out);
	/* sort strongest first (small arrays; simple insertion) */
	int n = cJSON_GetArraySize(arr);
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0; j--) {
			cJSON *a = cJSON_GetArrayItem(arr, j - 1), *b = cJSON_GetArrayItem(arr, j);
			if (pf_json_int(a, "signal", 0) >= pf_json_int(b, "signal", 0)) break;
			cJSON *det = cJSON_DetachItemFromArray(arr, j);
			cJSON_InsertItemInArray(arr, j - 1, det);
		}
	return arr;
}

cJSON *pf_wifi_status(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "iface", g_iface);
	if (g_sim) {
		cJSON_AddBoolToObject(o, "connected", !g_sim_hotspot);
		cJSON_AddStringToObject(o, "ssid", g_sim_hotspot ? "" : g_sim_ssid);
		cJSON_AddNumberToObject(o, "signal", g_sim_hotspot ? 0 : 82);
		cJSON_AddStringToObject(o, "ip", g_sim_hotspot ? "10.42.0.1" : "192.168.1.50");
		cJSON_AddBoolToObject(o, "hotspot", g_sim_hotspot);
		return o;
	}
	char out[4096];
	const char *argv[] = { "nmcli", "-t", "-f", "GENERAL.STATE,GENERAL.CONNECTION", "dev", "show", g_iface, NULL };
	bool connected = false;
	char con[128] = "";
	if (pf_run_capture(argv, out, sizeof out, 10) == 0) {
		char *save = NULL;
		for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if (!strncmp(line, "GENERAL.STATE:", 14)) connected = atoi(line + 14) == 100;
			else if (!strncmp(line, "GENERAL.CONNECTION:", 19)) pf_strlcpy(con, line + 19, sizeof con);
		}
	}
	bool hotspot = !strcmp(con, HOTSPOT_CON);
	cJSON_AddBoolToObject(o, "connected", connected && !hotspot);
	cJSON_AddBoolToObject(o, "hotspot", hotspot);
	cJSON_AddStringToObject(o, "ssid", hotspot ? "" : con);
	cJSON_AddStringToObject(o, "ip", iface_ip(g_iface));
	int signal = 0;
	if (connected && !hotspot) {
		cJSON *scan = pf_wifi_scan(false), *it;
		cJSON_ArrayForEach(it, scan) if (pf_json_bool(it, "active", false)) { signal = pf_json_int(it, "signal", 0); break; }
		cJSON_Delete(scan);
	}
	cJSON_AddNumberToObject(o, "signal", signal);
	return o;
}

int pf_wifi_connect(const char *ssid, const char *psk, char *err, size_t errn)
{
	if (g_sim) {
		pf_sleep_ms(1500);
		if (psk && strlen(psk) > 0 && strlen(psk) < 8) { snprintf(err, errn, "Secrets were required, but not provided (simulated failure)"); return -1; }
		g_sim_hotspot = false;
		pf_strlcpy(g_sim_ssid, ssid, sizeof g_sim_ssid);
		return 0;
	}
	char out[2048];
	int rc;
	if (psk && *psk) {
		const char *argv[] = { "nmcli", "-w", "35", "dev", "wifi", "connect", ssid, "password", psk, "ifname", g_iface, NULL };
		rc = pf_run_capture(argv, out, sizeof out, 45);
	} else {
		const char *argv[] = { "nmcli", "-w", "35", "dev", "wifi", "connect", ssid, "ifname", g_iface, NULL };
		rc = pf_run_capture(argv, out, sizeof out, 45);
	}
	if (rc != 0) {
		LOGW(TAG, "connect to '%s' failed (%d): %.200s", ssid, rc, out);
		snprintf(err, errn, "%.200s", out[0] ? out : "connection failed");
		pf_wifi_forget(ssid);
		return -1;
	}
	LOGI(TAG, "connected to '%s'", ssid);
	return 0;
}

int pf_wifi_forget(const char *ssid)
{
	if (g_sim) return 0;
	const char *argv[] = { "nmcli", "con", "delete", ssid, NULL };
	return pf_run_capture(argv, NULL, 0, 10) == 0 ? 0 : -1;
}

cJSON *pf_wifi_saved(void)
{
	cJSON *arr = cJSON_CreateArray();
	if (g_sim) { cJSON_AddItemToArray(arr, cJSON_CreateString(g_sim_ssid)); return arr; }
	char out[8192];
	const char *argv[] = { "nmcli", "-t", "-f", "NAME,TYPE", "con", "show", NULL };
	if (pf_run_capture(argv, out, sizeof out, 10) != 0) return arr;
	char *save = NULL;
	for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[2];
		if (split_fields(line, f, 2) < 2) continue;
		if (strstr(f[1], "wireless") && strcmp(f[0], HOTSPOT_CON)) cJSON_AddItemToArray(arr, cJSON_CreateString(f[0]));
	}
	return arr;
}

void pf_hotspot_default_ssid(char *out, size_t n)
{
	char path[64], mac[32] = "";
	snprintf(path, sizeof path, "/sys/class/net/%s/address", g_iface);
	char *m = pf_read_file(path, NULL);
	if (m) { pf_strlcpy(mac, m, sizeof mac); free(m); }
	char suffix[8] = "0000";
	size_t l = strlen(mac);
	if (l >= 5) { suffix[0] = mac[l - 6]; suffix[1] = mac[l - 5]; suffix[2] = mac[l - 3]; suffix[3] = mac[l - 2]; suffix[4] = 0; }
	for (char *p = suffix; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char)(*p - 32);
	snprintf(out, n, "PiFire-%s", suffix);
}

int pf_hotspot_start(const char *ssid, const char *psk)
{
	if (g_sim) { g_sim_hotspot = true; LOGI(TAG, "simulated hotspot '%s' up", ssid); return 0; }
	char out[2048];
	const char *argv[] = { "nmcli", "dev", "wifi", "hotspot", "ifname", g_iface, "con-name", HOTSPOT_CON, "ssid", ssid, "password", psk, NULL };
	int rc = pf_run_capture(argv, out, sizeof out, 30);
	if (rc != 0) { LOGE(TAG, "hotspot start failed (%d): %.200s", rc, out); return -1; }
	const char *argv2[] = { "nmcli", "con", "modify", HOTSPOT_CON, "connection.autoconnect", "no", NULL };
	pf_run_capture(argv2, NULL, 0, 10);
	LOGI(TAG, "hotspot '%s' up on %s", ssid, g_iface);
	return 0;
}

int pf_hotspot_stop(void)
{
	if (g_sim) { g_sim_hotspot = false; return 0; }
	const char *argv[] = { "nmcli", "con", "down", HOTSPOT_CON, NULL };
	return pf_run_capture(argv, NULL, 0, 15) == 0 ? 0 : -1;
}

bool pf_hotspot_is_up(void)
{
	if (g_sim) return g_sim_hotspot;
	cJSON *st = pf_wifi_status();
	bool up = pf_json_bool(st, "hotspot", false);
	cJSON_Delete(st);
	return up;
}
