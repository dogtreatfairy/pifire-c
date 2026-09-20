#include "core/settings.h"
#include "core/embedded.h"
#include "core/log.h"
#include "core/util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "settings"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static cJSON *g_root;
static char g_path[512];
static unsigned g_gen;

/* Settings whose values are temperatures and must be converted when units change. */
static const char *const temp_paths[] = {
	"safety.minstartuptemp", "safety.maxstartuptemp", "safety.maxtemp", "safety.restart_hot_temp",
	"startup.startup_exit_temp", "startup.start_to_mode.primary_setpoint", "startup.smartstart.exit_temp",
	"keep_warm.temp", "smoke_plus.min_temp", "smoke_plus.max_temp",
	NULL
};
/* Temperature *deltas* (no offset). */
static const char *const delta_paths[] = {
	"safety.coldstart.delta_rise", NULL
};

/* ---------------- generic JSON path helpers ---------------- */

cJSON *pf_json_path(cJSON *root, const char *path)
{
	if (!root) return NULL;
	if (!path || !*path) return root;
	char buf[256];
	pf_strlcpy(buf, path, sizeof buf);
	cJSON *cur = root;
	char *save = NULL;
	for (char *tok = strtok_r(buf, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
		if (cJSON_IsArray(cur)) {
			char *end;
			long idx = strtol(tok, &end, 10);
			if (*end) return NULL;
			cur = cJSON_GetArrayItem(cur, (int)idx);
		} else {
			cur = cJSON_GetObjectItemCaseSensitive(cur, tok);
		}
		if (!cur) return NULL;
	}
	return cur;
}

double pf_json_num(cJSON *root, const char *path, double dflt)
{
	cJSON *n = pf_json_path(root, path);
	if (cJSON_IsNumber(n)) return n->valuedouble;
	if (cJSON_IsBool(n)) return cJSON_IsTrue(n) ? 1 : 0;
	if (cJSON_IsString(n)) { char *e; double v = strtod(n->valuestring, &e); if (e != n->valuestring) return v; }
	return dflt;
}
int pf_json_int(cJSON *root, const char *path, int dflt) { return (int)pf_json_num(root, path, dflt); }
bool pf_json_bool(cJSON *root, const char *path, bool dflt)
{
	cJSON *n = pf_json_path(root, path);
	if (cJSON_IsBool(n)) return cJSON_IsTrue(n);
	if (cJSON_IsNumber(n)) return n->valuedouble != 0;
	if (cJSON_IsString(n)) return !strcasecmp(n->valuestring, "true") || !strcmp(n->valuestring, "1");
	return dflt;
}
const char *pf_json_str(cJSON *root, const char *path, const char *dflt)
{
	cJSON *n = pf_json_path(root, path);
	return cJSON_IsString(n) ? n->valuestring : dflt;
}

void pf_json_merge(cJSON *dst, const cJSON *src)
{
	if (!cJSON_IsObject(dst) || !cJSON_IsObject(src)) return;
	for (const cJSON *it = src->child; it; it = it->next) {
		cJSON *d = cJSON_GetObjectItemCaseSensitive(dst, it->string);
		if (d && cJSON_IsObject(d) && cJSON_IsObject(it)) {
			pf_json_merge(d, it);
		} else {
			cJSON *dup = cJSON_Duplicate(it, 1);
			if (d) cJSON_ReplaceItemInObjectCaseSensitive(dst, it->string, dup);
			else cJSON_AddItemToObject(dst, it->string, dup);
		}
	}
}

/* Fill keys missing in dst from src (never overwrite). Returns number of keys added. */
static int fill_defaults(cJSON *dst, const cJSON *src)
{
	int added = 0;
	if (!cJSON_IsObject(dst) || !cJSON_IsObject(src)) return 0;
	for (const cJSON *it = src->child; it; it = it->next) {
		cJSON *d = cJSON_GetObjectItemCaseSensitive(dst, it->string);
		if (!d) { cJSON_AddItemToObject(dst, it->string, cJSON_Duplicate(it, 1)); added++; }
		else if (cJSON_IsObject(d) && cJSON_IsObject(it)) added += fill_defaults(d, it);
	}
	return added;
}

/* ---------------- validation ---------------- */

static int validate(cJSON *root, char *err, size_t errn)
{
#define CHECK(cond, ...) do { if (!(cond)) { snprintf(err, errn, __VA_ARGS__); return -1; } } while (0)
	const char *units = pf_json_str(root, "globals.units", "F");
	CHECK(!strcmp(units, "F") || !strcmp(units, "C"), "globals.units must be F or C");
	double umin = pf_json_num(root, "cycle_data.u_min", 0.1), umax = pf_json_num(root, "cycle_data.u_max", 0.9);
	CHECK(umin >= 0 && umin < umax && umax <= 1.0, "cycle_data.u_min/u_max must satisfy 0 <= u_min < u_max <= 1");
	CHECK(pf_json_num(root, "cycle_data.HoldCycleTime", 25) >= 5, "cycle_data.HoldCycleTime must be >= 5 s");
	double maxtemp = pf_json_num(root, "safety.maxtemp", 550);
	CHECK(maxtemp > 100, "safety.maxtemp too low");
	CHECK(pf_json_num(root, "safety.auger_max_on_s", 60) >= 5, "safety.auger_max_on_s must be >= 5");
	CHECK(pf_json_num(root, "safety.igniter_max_on_s", 1200) >= 60, "safety.igniter_max_on_s must be >= 60");
	CHECK(pf_json_num(root, "safety.coldstart.delta_rise", 12) > 0, "safety.coldstart.delta_rise must be > 0");
	int port = pf_json_int(root, "web.port", 80);
	CHECK(port > 0 && port < 65536, "web.port out of range");
	CHECK(pf_json_num(root, "history.sample_s", 3) >= 1, "history.sample_s must be >= 1");
	CHECK(strlen(pf_json_str(root, "network.hotspot_password", "pifire1234")) >= 8, "network.hotspot_password must be at least 8 characters");
	return 0;
#undef CHECK
}

/* ---------------- lifecycle ---------------- */

static cJSON *load_defaults(void)
{
	const pf_embedded_file *f = pf_embedded_share("settings.default.json");
	if (!f) { LOGE(TAG, "embedded settings.default.json missing"); return NULL; }
	cJSON *d = cJSON_ParseWithLength((const char *)f->data, f->len);
	if (!d) LOGE(TAG, "embedded defaults do not parse: %s", cJSON_GetErrorPtr());
	return d;
}

static int save_locked(void)
{
	char *txt = cJSON_Print(g_root);
	if (!txt) return -1;
	int rc = pf_write_file_atomic(g_path, txt, strlen(txt));
	free(txt);
	if (rc) LOGE(TAG, "save %s failed: %s", g_path, strerror(-rc));
	else g_gen++;
	return rc;
}

int pf_settings_init(const char *path)
{
	pf_strlcpy(g_path, path, sizeof g_path);
	cJSON *defaults = load_defaults();
	if (!defaults) return -1;

	cJSON *loaded = NULL;
	size_t len = 0;
	char *txt = pf_read_file(path, &len);
	if (txt) {
		loaded = cJSON_ParseWithLength(txt, len);
		if (!loaded) {
			LOGE(TAG, "%s is not valid JSON near '%.20s' - keeping a copy as %s.bad and using defaults",
			     path, cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?", path);
			char bad[600];
			snprintf(bad, sizeof bad, "%s.bad", path);
			pf_write_file_atomic(bad, txt, len);
		}
		free(txt);
	}

	int added = 0;
	if (loaded) {
		added = fill_defaults(loaded, defaults);
		cJSON_Delete(defaults);
		g_root = loaded;
	} else {
		g_root = defaults;
		added = 1;
	}

	char err[256];
	if (validate(g_root, err, sizeof err)) {
		LOGW(TAG, "settings validation: %s (continuing with stored values)", err);
	}

	pthread_mutex_lock(&g_mu);
	int rc = 0;
	if (added) {
		char dir[512];
		pf_strlcpy(dir, path, sizeof dir);
		char *s = strrchr(dir, '/');
		if (s) { *s = 0; pf_mkdir_p(dir); }
		rc = save_locked();
		LOGI(TAG, "%s %s (%d default keys added)", loaded ? "updated" : "created", path, added);
	} else {
		LOGI(TAG, "loaded %s", path);
	}
	pthread_mutex_unlock(&g_mu);
	return rc;
}

void pf_settings_shutdown(void)
{
	pthread_mutex_lock(&g_mu);
	cJSON_Delete(g_root);
	g_root = NULL;
	pthread_mutex_unlock(&g_mu);
}

cJSON *pf_settings_lock(void) { pthread_mutex_lock(&g_mu); return g_root; }
void pf_settings_unlock(void) { pthread_mutex_unlock(&g_mu); }

int pf_settings_save(void)
{
	pthread_mutex_lock(&g_mu);
	int rc = save_locked();
	pthread_mutex_unlock(&g_mu);
	return rc;
}

unsigned pf_settings_generation(void) { return __atomic_load_n(&g_gen, __ATOMIC_RELAXED); }

/* ---------------- typed accessors ---------------- */

double pf_set_num(const char *path, double dflt)
{
	pthread_mutex_lock(&g_mu);
	double v = pf_json_num(g_root, path, dflt);
	pthread_mutex_unlock(&g_mu);
	return v;
}
int pf_set_int(const char *path, int dflt) { return (int)pf_set_num(path, dflt); }
bool pf_set_bool(const char *path, bool dflt)
{
	pthread_mutex_lock(&g_mu);
	bool v = pf_json_bool(g_root, path, dflt);
	pthread_mutex_unlock(&g_mu);
	return v;
}
bool pf_set_str(const char *path, char *buf, size_t n, const char *dflt)
{
	pthread_mutex_lock(&g_mu);
	const char *s = pf_json_str(g_root, path, NULL);
	bool found = s != NULL;
	pf_strlcpy(buf, s ? s : (dflt ? dflt : ""), n);
	pthread_mutex_unlock(&g_mu);
	return found;
}
cJSON *pf_set_dup(const char *path)
{
	pthread_mutex_lock(&g_mu);
	cJSON *n = pf_json_path(g_root, path);
	cJSON *d = n ? cJSON_Duplicate(n, 1) : NULL;
	pthread_mutex_unlock(&g_mu);
	return d;
}

/* Walk/create parents; returns parent object and leaf key. */
static cJSON *parent_of(cJSON *root, const char *path, char *leaf, size_t leafn, bool create)
{
	char buf[256];
	pf_strlcpy(buf, path, sizeof buf);
	char *last = strrchr(buf, '.');
	if (!last) { pf_strlcpy(leaf, buf, leafn); return root; }
	*last = 0;
	pf_strlcpy(leaf, last + 1, leafn);
	cJSON *cur = root;
	char *save = NULL;
	for (char *tok = strtok_r(buf, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
		cJSON *next = cJSON_GetObjectItemCaseSensitive(cur, tok);
		if (!next) {
			if (!create) return NULL;
			next = cJSON_AddObjectToObject(cur, tok);
		}
		cur = next;
	}
	return cur;
}

int pf_set_put(const char *path, cJSON *v)
{
	char leaf[64];
	pthread_mutex_lock(&g_mu);
	cJSON *p = parent_of(g_root, path, leaf, sizeof leaf, true);
	int rc = -1;
	if (p && cJSON_IsObject(p)) {
		if (cJSON_GetObjectItemCaseSensitive(p, leaf)) cJSON_ReplaceItemInObjectCaseSensitive(p, leaf, v);
		else cJSON_AddItemToObject(p, leaf, v);
		rc = 0;
	} else {
		cJSON_Delete(v);
	}
	pthread_mutex_unlock(&g_mu);
	return rc;
}
int pf_set_put_num(const char *path, double v) { return pf_set_put(path, cJSON_CreateNumber(v)); }
int pf_set_put_bool(const char *path, bool v) { return pf_set_put(path, cJSON_CreateBool(v)); }
int pf_set_put_str(const char *path, const char *v) { return pf_set_put(path, cJSON_CreateString(v)); }

int pf_settings_patch(const char *path, const char *json, char *err, size_t errn)
{
	cJSON *patch = cJSON_Parse(json);
	if (!patch) { snprintf(err, errn, "invalid JSON"); return -1; }

	pthread_mutex_lock(&g_mu);
	cJSON *trial = cJSON_Duplicate(g_root, 1);
	int rc = -1;
	cJSON *target = trial;
	if (path && *path) {
		char leaf[64];
		cJSON *p = parent_of(trial, path, leaf, sizeof leaf, true);
		target = p ? cJSON_GetObjectItemCaseSensitive(p, leaf) : NULL;
		if (!target) { target = cJSON_AddObjectToObject(p, leaf); }
	}
	if (cJSON_IsObject(target) && cJSON_IsObject(patch)) {
		pf_json_merge(target, patch);
		rc = 0;
	} else if (path && *path) {
		char leaf[64];
		cJSON *p = parent_of(trial, path, leaf, sizeof leaf, true);
		cJSON_ReplaceItemInObjectCaseSensitive(p, leaf, cJSON_Duplicate(patch, 1));
		rc = 0;
	} else {
		snprintf(err, errn, "root patch must be an object");
	}
	cJSON_Delete(patch);

	if (rc == 0 && validate(trial, err, errn) == 0) {
		/* units change requested? keep the OLD units string in the tree so set_units converts */
		char old_u[4], new_u[4];
		pf_strlcpy(old_u, pf_json_str(g_root, "globals.units", "F"), sizeof old_u);
		pf_strlcpy(new_u, pf_json_str(trial, "globals.units", "F"), sizeof new_u);
		cJSON_Delete(g_root);
		g_root = trial;
		if (strcmp(old_u, new_u)) {
			cJSON *n = pf_json_path(g_root, "globals.units");
			if (n) cJSON_SetValuestring(n, old_u);
			pthread_mutex_unlock(&g_mu);
			pf_settings_set_units(new_u[0] == 'C' ? PF_UNITS_C : PF_UNITS_F);
			return 0;
		}
		rc = save_locked();
		if (rc) snprintf(err, errn, "save failed");
	} else {
		cJSON_Delete(trial);
		rc = -1;
	}
	pthread_mutex_unlock(&g_mu);
	return rc;
}

pf_units pf_settings_units(void)
{
	char u[4];
	pf_set_str("globals.units", u, sizeof u, "F");
	return u[0] == 'C' ? PF_UNITS_C : PF_UNITS_F;
}

static void convert_paths(cJSON *root, const char *const *paths, double (*fn)(double))
{
	for (int i = 0; paths[i]; i++) {
		cJSON *n = pf_json_path(root, paths[i]);
		if (cJSON_IsNumber(n)) cJSON_SetNumberValue(n, (double)(long)(fn(n->valuedouble) + 0.5));
	}
}
static double f2c(double f) { return pf_f_to_c(f); }
static double c2f(double c) { return pf_c_to_f(c); }
static double df2c(double d) { return d * 5.0 / 9.0; }
static double dc2f(double d) { return d * 9.0 / 5.0; }

int pf_settings_set_units(pf_units u)
{
	pthread_mutex_lock(&g_mu);
	const char *cur = pf_json_str(g_root, "globals.units", "F");
	pf_units cu = cur[0] == 'C' ? PF_UNITS_C : PF_UNITS_F;
	int rc = 0;
	if (cu != u) {
		convert_paths(g_root, temp_paths, u == PF_UNITS_C ? f2c : c2f);
		convert_paths(g_root, delta_paths, u == PF_UNITS_C ? df2c : dc2f);
		cJSON *n = pf_json_path(g_root, "globals.units");
		if (n) cJSON_SetValuestring(n, u == PF_UNITS_C ? "C" : "F");
		LOGI(TAG, "units changed to %s; temperature settings converted", u == PF_UNITS_C ? "C" : "F");
	}
	rc = save_locked();
	pthread_mutex_unlock(&g_mu);
	return rc;
}

void pf_settings_force_sim(void)
{
	pf_set_put_str("platform.system_type", "sim");
	pf_set_put_str("modules.grillplat", "sim");
	pf_set_put_str("modules.display", "none");
	pf_set_put_str("modules.dist", "none");
	cJSON *root = pf_settings_lock();
	cJSON *devs = pf_json_path(root, "probe_settings.probe_map.probe_devices"), *d;
	cJSON_ArrayForEach(d, devs) {
		const char *m = pf_json_str(d, "module", "");
		if (strcmp(m, "sim") && strcmp(m, "virtual")) cJSON_ReplaceItemInObject(d, "module", cJSON_CreateString("sim"));
	}
	pf_settings_unlock();
}
