#define _GNU_SOURCE
#include "probes/registry.h"
#include "core/log.h"
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define TAG "probes"
#define MAX_DRIVERS 32

static const pf_probe_ops *g_list[MAX_DRIVERS];
static int g_count;

static void add(const pf_probe_ops *ops, const char *origin)
{
	if (!ops) return;
	if (ops->abi != PF_PROBE_ABI) { LOGE(TAG, "%s: probe driver ABI %u != %u - ignored", origin, ops->abi, PF_PROBE_ABI); return; }
	if (!ops->id || !ops->create || !ops->read || !ops->ports) { LOGE(TAG, "%s: probe driver incomplete - ignored", origin); return; }
	if (pf_probe_driver_find(ops->id)) return;
	if (g_count >= MAX_DRIVERS) return;
	g_list[g_count++] = ops;
	LOGD(TAG, "registered probe driver '%s' (%s)", ops->id, origin);
}

void pf_probe_drivers_init(const char *plugin_dir)
{
	g_count = 0;
	add(pf_probe_sim(), "builtin");
	add(pf_probe_ads1x15(), "builtin");
	add(pf_probe_max31865(), "builtin");
	add(pf_probe_mcp9600(), "builtin");
	add(pf_probe_ds18b20(), "builtin");
	add(pf_probe_virtual(), "builtin");
	add(pf_probe_ibbq(), "builtin");
	add(pf_probe_meater(), "builtin");
	if (!plugin_dir) return;
	DIR *d = opendir(plugin_dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (l < 4 || strcmp(e->d_name + l - 3, ".so")) continue;
		char path[512];
		snprintf(path, sizeof path, "%s/%s", plugin_dir, e->d_name);
		void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (!h) { LOGE(TAG, "dlopen %s: %s", path, dlerror()); continue; }
		pf_probe_export_fn fn = (pf_probe_export_fn)dlsym(h, "pf_probe_export");
		if (!fn) { LOGE(TAG, "%s: no pf_probe_export", path); dlclose(h); continue; }
		add(fn(), path);
	}
	closedir(d);
}

const pf_probe_ops *pf_probe_driver_find(const char *id)
{
	for (int i = 0; i < g_count; i++)
		if (!strcmp(g_list[i]->id, id)) return g_list[i];
	return NULL;
}
int pf_probe_driver_count(void) { return g_count; }
const pf_probe_ops *pf_probe_driver_at(int i) { return i >= 0 && i < g_count ? g_list[i] : NULL; }
