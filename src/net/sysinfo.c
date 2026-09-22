#define _GNU_SOURCE
#include "net/sysinfo.h"
#include "core/util.h"
#include "pifire/common.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>

static double read_num(const char *path, double dflt)
{
	char *s = pf_read_file(path, NULL);
	if (!s) return dflt;
	double v = strtod(s, NULL);
	free(s);
	return v;
}

static long meminfo(const char *key)
{
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) return 0;
	char line[128];
	long v = 0;
	size_t kl = strlen(key);
	while (fgets(line, sizeof line, f))
		/* The key matched the first kl characters, which says nothing about there being anything
		 * after them: stepping to kl + 1 on a line that ended at the key reads past the string. */
		if (!strncmp(line, key, kl) && line[kl]) { v = atol(line + kl + 1) * 1024; break; }
	fclose(f);
	return v;
}

static double wifi_quality(void)
{
	FILE *f = fopen("/proc/net/wireless", "r");
	if (!f) return -1;
	char line[256];
	double q = -1;
	while (fgets(line, sizeof line, f)) {
		char name[32];
		double status, link;
		if (sscanf(line, " %31[^:]: %lf %lf", name, &status, &link) == 3) { q = link / 70.0 * 100.0; break; }
	}
	fclose(f);
	return q > 100 ? 100 : q;
}

cJSON *pf_sysinfo_json(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "version", PF_VERSION);
	struct sysinfo si;
	if (sysinfo(&si) == 0) {
		cJSON_AddNumberToObject(o, "uptime_s", (double)si.uptime);
		cJSON_AddNumberToObject(o, "load1", si.loads[0] / 65536.0);
	}
	double t = read_num("/sys/class/thermal/thermal_zone0/temp", -1);
	cJSON_AddNumberToObject(o, "cpu_temp_c", t > 0 ? t / 1000.0 : -1);
	/* Raspberry Pi firmware throttle flags (same bits as vcgencmd get_throttled) */
	char *thr = pf_read_file("/sys/devices/platform/soc/soc:firmware/get_throttled", NULL);
	if (thr) {
		unsigned v = (unsigned)strtoul(thr, NULL, 16);
		free(thr);
		cJSON_AddBoolToObject(o, "under_voltage", (v & 0x1) != 0);
		cJSON_AddBoolToObject(o, "throttled", (v & 0x4) != 0);
		cJSON_AddBoolToObject(o, "under_voltage_occurred", (v & 0x10000) != 0);
	}
	cJSON_AddNumberToObject(o, "mem_total", (double)meminfo("MemTotal:"));
	cJSON_AddNumberToObject(o, "mem_available", (double)meminfo("MemAvailable:"));
	cJSON_AddNumberToObject(o, "wifi_quality_pct", wifi_quality());
	char host[64] = "";
	gethostname(host, sizeof host);
	cJSON_AddStringToObject(o, "hostname", host);

	cJSON *ifs = cJSON_AddArrayToObject(o, "interfaces");
	struct ifaddrs *ifa = NULL;
	if (getifaddrs(&ifa) == 0) {
		for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
			if (!p->ifa_addr || (p->ifa_flags & IFF_LOOPBACK) || !(p->ifa_flags & IFF_UP)) continue;
			if (p->ifa_addr->sa_family != AF_INET) continue;
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, ip, sizeof ip);
			cJSON *e = cJSON_CreateObject();
			cJSON_AddStringToObject(e, "name", p->ifa_name);
			cJSON_AddStringToObject(e, "ip", ip);
			for (struct ifaddrs *q = ifa; q; q = q->ifa_next) {
				if (q->ifa_addr && q->ifa_addr->sa_family == AF_PACKET && !strcmp(q->ifa_name, p->ifa_name)) {
					struct sockaddr_ll *ll = (struct sockaddr_ll *)q->ifa_addr;
					char mac[18];
					snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", ll->sll_addr[0], ll->sll_addr[1], ll->sll_addr[2], ll->sll_addr[3], ll->sll_addr[4], ll->sll_addr[5]);
					cJSON_AddStringToObject(e, "mac", mac);
				}
			}
			cJSON_AddItemToArray(ifs, e);
		}
		freeifaddrs(ifa);
	}
	return o;
}
