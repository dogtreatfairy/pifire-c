#define _GNU_SOURCE
#include "controllers/registry.h"
#include "controllers/pid_common.h"
#include "core/log.h"
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define TAG "controllers"
#define MAX_CONTROLLERS 32

static const pf_controller_ops *g_list[MAX_CONTROLLERS];
static int g_count;

static void add(const pf_controller_ops *ops, const char *origin)
{
	if (!ops) return;
	if (ops->abi != PF_CONTROLLER_ABI) {
		LOGE(TAG, "%s: controller '%s' has ABI %u, expected %u - ignored", origin, ops->id ? ops->id : "?", ops->abi, PF_CONTROLLER_ABI);
		return;
	}
	if (!ops->id || !ops->create || !ops->update || !ops->reset) {
		LOGE(TAG, "%s: controller missing required fields - ignored", origin);
		return;
	}
	if (pf_controller_find(ops->id)) {
		LOGW(TAG, "%s: controller '%s' already registered - ignored", origin, ops->id);
		return;
	}
	if (g_count >= MAX_CONTROLLERS) return;
	g_list[g_count++] = ops;
	LOGI(TAG, "registered controller '%s' (%s)", ops->id, origin);
}

void pf_controllers_init(const char *plugin_dir)
{
	g_count = 0;
	add(pf_controller_pid(), "builtin");

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
		pf_controller_export_fn fn = (pf_controller_export_fn)dlsym(h, "pf_controller_export");
		if (!fn) { LOGE(TAG, "%s: no pf_controller_export symbol", path); dlclose(h); continue; }
		add(fn(), path);
	}
	closedir(d);
}

const pf_controller_ops *pf_controller_find(const char *id)
{
	for (int i = 0; i < g_count; i++)
		if (!strcmp(g_list[i]->id, id)) return g_list[i];
	return NULL;
}

int pf_controller_count(void) { return g_count; }
const pf_controller_ops *pf_controller_at(int i) { return i >= 0 && i < g_count ? g_list[i] : NULL; }
