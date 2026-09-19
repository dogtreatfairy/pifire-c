#define _GNU_SOURCE
#include "display/registry.h"
#include "core/cmdq.h"
#include "core/env.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/status.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "display"

static const pf_display_ops *g_ops;
static void *g_inst;
static pf_env g_env;

/* ---- none ---- */
static void *none_create(const char *cfg, const pf_env *env) { (void)cfg; (void)env; return (void *)1; }
static void none_destroy(void *self) { (void)self; }
static void none_status(void *self, const char *json) { (void)self; (void)json; }
static void none_text(void *self, const char *msg) { (void)self; (void)msg; }
static pf_key none_poll(void *self) { (void)self; return PF_KEY_NONE; }
static const pf_display_ops none_ops = { .abi = PF_DISPLAY_ABI, .id = "none", .name = "None", .create = none_create, .destroy = none_destroy, .status = none_status, .text = none_text, .poll_input = none_poll };
const pf_display_ops *pf_display_none(void) { return &none_ops; }

static const pf_display_ops *find(const char *id)
{
	if (!id || !*id || !strcmp(id, "none")) return &none_ops;
	/* out-of-tree drivers: /usr/lib/pifire/display/<id>.so exporting pf_display_export */
	char path[256];
	snprintf(path, sizeof path, "/usr/lib/pifire/display/%s.so", id);
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) { LOGW(TAG, "display '%s' not available (%s); using none", id, dlerror()); return &none_ops; }
	const pf_display_ops *(*fn)(void) = (const pf_display_ops * (*)(void))dlsym(h, "pf_display_export");
	const pf_display_ops *ops = fn ? fn() : NULL;
	if (!ops || ops->abi != PF_DISPLAY_ABI) { LOGE(TAG, "%s: bad or missing pf_display_export", path); dlclose(h); return &none_ops; }
	return ops;
}

int pf_display_init(void)
{
	char id[32];
	pf_set_str("modules.display", id, sizeof id, "none");
	g_ops = find(id);
	pf_env_init(&g_env, "display");
	cJSON *cfg = pf_set_dup("display");
	cJSON *pins = pf_set_dup("platform.devices");
	if (!cfg) cfg = cJSON_CreateObject();
	if (pins) cJSON_AddItemToObject(cfg, "devices", pins);
	char *js = cJSON_PrintUnformatted(cfg);
	cJSON_Delete(cfg);
	g_inst = g_ops->create(js, &g_env);
	free(js);
	if (!g_inst) { LOGW(TAG, "display '%s' failed to initialise; using none", g_ops->id); g_ops = &none_ops; g_inst = none_create("", &g_env); }
	if (strcmp(g_ops->id, "none")) LOGI(TAG, "display: %s", g_ops->name);
	return 0;
}

void pf_display_shutdown(void)
{
	if (g_ops && g_inst) g_ops->destroy(g_inst);
	g_inst = NULL;
}

void pf_display_tick(const char *status_json)
{
	if (!g_ops || !g_inst) return;
	g_ops->status(g_inst, status_json);
	pf_key k = g_ops->poll_input(g_inst);
	if (k == PF_KEY_LONG_ENTER) pf_cmd_simple(PF_CMD_STOP);
	/* UP/DOWN/ENTER are for a future on-device menu; a long press is always the e-stop */
}

void pf_display_text(const char *msg)
{
	if (g_ops && g_inst) g_ops->text(g_inst, msg);
}
