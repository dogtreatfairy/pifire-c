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
	"safety.coldstart.delta_rise", "startup.exit_rise", NULL
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

/* Built-in notification rules are adopted once each, by id. A rule a new release introduces appears
 * for people who already have a settings file, and one the user deleted stays deleted, because the
 * ids that have ever been offered are remembered alongside the rules. */
static int adopt_builtin_rules(cJSON *root, cJSON *defaults)
{
	cJSON *drules = pf_json_path(defaults, "notify.rules");
	cJSON *notify = pf_json_path(root, "notify");
	if (!cJSON_IsArray(drules) || !notify) return 0;
	cJSON *rules = cJSON_GetObjectItem(notify, "rules");
	if (!cJSON_IsArray(rules)) rules = cJSON_AddArrayToObject(notify, "rules");
	cJSON *seen = cJSON_GetObjectItem(notify, "builtins");
	if (!cJSON_IsArray(seen)) seen = cJSON_AddArrayToObject(notify, "builtins");
	int added = 0, offered = 0;
	cJSON *d;
	cJSON_ArrayForEach(d, drules) {
		const char *id = pf_json_str(d, "id", "");
		if (!id[0]) continue;
		bool known = false, have = false;
		cJSON *it;
		cJSON_ArrayForEach(it, seen) if (cJSON_IsString(it) && !strcmp(it->valuestring, id)) known = true;
		if (known) continue;
		offered++;
		cJSON_ArrayForEach(it, rules) if (!strcmp(pf_json_str(it, "id", ""), id)) have = true;
		if (!have && rules != drules) { cJSON_AddItemToArray(rules, cJSON_Duplicate(d, 1)); added++; }
	}
	/* record the ids only after the walk, so appending to `seen` cannot disturb it */
	cJSON_ArrayForEach(d, drules) {
		const char *id = pf_json_str(d, "id", "");
		if (!id[0]) continue;
		bool known = false;
		cJSON *it;
		cJSON_ArrayForEach(it, seen) if (cJSON_IsString(it) && !strcmp(it->valuestring, id)) known = true;
		if (!known) cJSON_AddItemToArray(seen, cJSON_CreateString(id));
	}
	if (added) LOGI(TAG, "%d new built-in notification rule(s) added", added);
	return offered;
}

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
		g_root = loaded;
		/* schema migrations for settings written by older builds */
		int ver = (int)pf_json_num(g_root, "schema_version", 1);
		if (ver < 2) {
			/* v1 shipped "never turn the panel off" (0) as the default; the panel now sleeps 5 s after the
			 * last input while stopped, and nobody chose 0 on purpose */
			cJSON *bt = pf_json_path(g_root, "display.backlight_timeout_s");
			if (cJSON_IsNumber(bt) && bt->valuedouble <= 0) cJSON_SetNumberValue(bt, 5);
			cJSON *sv = cJSON_GetObjectItem(g_root, "schema_version");
			if (sv) cJSON_SetNumberValue(sv, 2); else cJSON_AddNumberToObject(g_root, "schema_version", 2);
			LOGI(TAG, "settings migrated to schema 2");
			added = 1;
		}
		if (ver < 3) {
			/* Chef iQ port BT_Food was renamed BT_Probe (the internal sensor; BT_Ambient is the handle) */
			cJSON *devs = pf_json_path(g_root, "probe_settings.probe_map.probe_devices"), *d;
			cJSON_ArrayForEach(d, devs) {
				if (strcmp(pf_json_str(d, "module", ""), "chefiq")) continue;
				cJSON *ports = cJSON_GetObjectItem(d, "ports"), *pt;
				cJSON_ArrayForEach(pt, ports) if (cJSON_IsString(pt) && !strcmp(pt->valuestring, "BT_Food")) cJSON_SetValuestring(pt, "BT_Probe");
				cJSON *infos = pf_json_path(g_root, "probe_settings.probe_map.probe_info"), *pi;
				cJSON_ArrayForEach(pi, infos) {
					if (strcmp(pf_json_str(pi, "device", ""), pf_json_str(d, "device", "")) || strcmp(pf_json_str(pi, "port", ""), "BT_Food")) continue;
					cJSON *pp = cJSON_GetObjectItem(pi, "port");
					if (cJSON_IsString(pp)) cJSON_SetValuestring(pp, "BT_Probe");
				}
			}
			cJSON *sv = cJSON_GetObjectItem(g_root, "schema_version");
			if (sv) cJSON_SetNumberValue(sv, 3); else cJSON_AddNumberToObject(g_root, "schema_version", 3);
			LOGI(TAG, "settings migrated to schema 3");
			added = 1;
		}
		if (ver < 4) {
			/* earlier builds removed a Bluetooth probe's readings but left its device entry behind, so
			 * settings accumulated stale devices (often several for one physical probe). A wireless
			 * device without readings, or a second entry for an address that already has one, is
			 * dropped: those addresses were being hidden from the pairing list forever. */
			cJSON *devs = pf_json_path(g_root, "probe_settings.probe_map.probe_devices");
			cJSON *infos = pf_json_path(g_root, "probe_settings.probe_map.probe_info");
			int removed = 0;
			for (int pass = 0; pass < 2; pass++) {
				/* pass 0: wireless devices without readings; pass 1: a second device for an address that
				 * already has one (keeping the earlier entry, whose readings the user has been using) */
				for (int i = cJSON_GetArraySize(devs) - 1; i >= 0; i--) {
					cJSON *d = cJSON_GetArrayItem(devs, i);
					const char *mod = pf_json_str(d, "module", "");
					if (strcmp(mod, "chefiq") && strcmp(mod, "meater") && strcmp(mod, "ibbq")) continue;
					const char *name = pf_json_str(d, "device", ""), *addr = pf_json_str(d, "config.hardware_id", "");
					bool drop = false;
					if (pass == 0) {
						drop = true;
						cJSON *pi;
						cJSON_ArrayForEach(pi, infos) if (!strcmp(pf_json_str(pi, "device", ""), name)) { drop = false; break; }
					} else {
						for (int j = 0; j < i && addr[0]; j++)
							if (!strcasecmp(pf_json_str(cJSON_GetArrayItem(devs, j), "config.hardware_id", ""), addr)) { drop = true; break; }
					}
					if (!drop) continue;
					LOGW(TAG, "dropping stale Bluetooth device '%s' (%s)", name, pass == 0 ? "no readings" : "duplicate address");
					for (int k = cJSON_GetArraySize(infos) - 1; k >= 0; k--)
						if (!strcmp(pf_json_str(cJSON_GetArrayItem(infos, k), "device", ""), name)) cJSON_DeleteItemFromArray(infos, k);
					cJSON_DeleteItemFromArray(devs, i);
					removed++;
				}
			}
			cJSON *sv = cJSON_GetObjectItem(g_root, "schema_version");
			if (sv) cJSON_SetNumberValue(sv, 4); else cJSON_AddNumberToObject(g_root, "schema_version", 4);
			LOGI(TAG, "settings migrated to schema 4 (%d stale Bluetooth device%s removed)", removed, removed == 1 ? "" : "s");
			added = 1;
		}
		if (ver < 5) {
			/* conditional notifications: the fixed alerts become editable rules. fill_defaults() has
			 * already put the built-in set in place, so all that is left is to carry the one setting
			 * the old predictive warning had into the rule that replaces it. */
			double warn = pf_json_num(g_root, "notify.eta_warn_min", 15);
			cJSON *rules = pf_json_path(g_root, "notify.rules"), *r;
			cJSON_ArrayForEach(r, rules) {
				if (strcmp(pf_json_str(r, "id", ""), "probe-eta")) continue;
				cJSON *conds = pf_json_path(r, "when.conditions"), *c;
				cJSON_ArrayForEach(c, conds)
					if (!strcmp(pf_json_str(c, "trait", ""), "eta") && !strcmp(pf_json_str(c, "op", ""), "<=")) {
						cJSON *v = cJSON_GetObjectItem(c, "value");
						if (cJSON_IsNumber(v)) cJSON_SetNumberValue(v, warn * 60);
					}
				if (warn <= 0) cJSON_ReplaceItemInObject(r, "enabled", cJSON_CreateFalse());
			}
			cJSON *sv = cJSON_GetObjectItem(g_root, "schema_version");
			if (sv) cJSON_SetNumberValue(sv, 5); else cJSON_AddNumberToObject(g_root, "schema_version", 5);
			LOGI(TAG, "settings migrated to schema 5 (notification rules)");
			added = 1;
		}
		if (ver < 6) {
			/* the hopper now has its own low and critical rules, which say more than the single
			 * built-in warning did, so the old one steps aside rather than doubling up */
			cJSON *we = pf_json_path(g_root, "pelletlevel.warning_enabled");
			if (cJSON_IsBool(we)) cJSON_ReplaceItemInObject(pf_json_path(g_root, "pelletlevel"), "warning_enabled", cJSON_CreateFalse());
			cJSON *sv = cJSON_GetObjectItem(g_root, "schema_version");
			if (sv) cJSON_SetNumberValue(sv, 6); else cJSON_AddNumberToObject(g_root, "schema_version", 6);
			LOGI(TAG, "settings migrated to schema 6 (grill and hopper rules)");
			added = 1;
		}
		if (ver < 7) {
			/* The grill-hot and grill-cold rules fired while the grill was simply on its way to a
			 * new set point, which is not a fault, it is a climb. They now wait until the grill
			 * has had twenty minutes to get there, so what they report is a stall. Existing copies
			 * are updated in place rather than offered as new rules, and one a user has already
			 * rewritten to their own conditions is left alone. */
			int fixed = 0;
			cJSON *rules = pf_json_path(g_root, "notify.rules"), *r;
			cJSON_ArrayForEach(r, rules) {
				const char *id = pf_json_str(r, "id", "");
				bool hot = !strcmp(id, "grill-hot"), cold = !strcmp(id, "grill-cold");
				if (!hot && !cold) continue;
				cJSON *conds = pf_json_path(r, "when.conditions");
				if (!cJSON_IsArray(conds)) continue;
				cJSON *cd; bool already = false;
				cJSON_ArrayForEach(cd, conds) if (!strcmp(pf_json_str(cd, "trait", ""), "aiming_s")) already = true;
				if (already) continue;
				cJSON *add = cJSON_CreateObject();
				cJSON_AddStringToObject(add, "entity", "grill");
				cJSON_AddStringToObject(add, "trait", "aiming_s");
				cJSON_AddStringToObject(add, "op", ">");
				cJSON_AddNumberToObject(add, "value", 1200);
				cJSON_AddItemToArray(conds, add);
				cJSON_ReplaceItemInObject(r, "name", cJSON_CreateString(hot ? "Grill Stalled Hot" : "Grill Stalled Cold"));
				cJSON_ReplaceItemInObject(r, "title", cJSON_CreateString(hot ? "{grill} is not coming down" : "{grill} is not getting there"));
				cJSON_ReplaceItemInObject(r, "body", cJSON_CreateString(hot
					? "Still {grill_temp} against a {setpoint} target, twenty minutes after being asked for it."
					: "Still {grill_temp} against a {setpoint} target, twenty minutes after being asked for it. Check the fire and the hopper."));
				cJSON *fs = cJSON_GetObjectItem(r, "for_s");
				if (fs) cJSON_SetNumberValue(fs, 120);
				fixed++;
			}
			cJSON *sv = cJSON_GetObjectItem(g_root, "schema_version");
			if (sv) cJSON_SetNumberValue(sv, 7); else cJSON_AddNumberToObject(g_root, "schema_version", 7);
			LOGI(TAG, "settings migrated to schema 7 (%d deviation rule%s now wait for a stall)", fixed, fixed == 1 ? "" : "s");
			added = 1;
		}
		if (ver < 8) {
			/* Probe rules fired for probes nobody was using: a grill can have nine configured and
			 * two in the meat, and the other seven sat in a drawer announcing that they were
			 * offline or had reached a target of zero. They now require the probe to be part of
			 * the cook, and to be a cook at all. A rule the user has rewritten is left alone. */
			int fixed = 0;
			cJSON *rules = pf_json_path(g_root, "notify.rules"), *r;
			cJSON_ArrayForEach(r, rules) {
				const char *id = pf_json_str(r, "id", "");
				if (strncmp(id, "probe-", 6)) continue;
				cJSON *conds = pf_json_path(r, "when.conditions");
				if (!cJSON_IsArray(conds)) continue;
				cJSON *cd; bool already = false;
				cJSON_ArrayForEach(cd, conds) if (!strcmp(pf_json_str(cd, "trait", ""), "in_use")) already = true;
				if (!already) {
					cJSON *add = cJSON_CreateObject();
					cJSON_AddStringToObject(add, "trait", "in_use");
					cJSON_AddStringToObject(add, "op", "is_on");
					cJSON_InsertItemInArray(conds, 0, add);
					fixed++;
				}
				cJSON *owc = cJSON_GetObjectItem(r, "only_while_cooking");
				if (owc && !cJSON_IsTrue(owc)) cJSON_ReplaceItemInObject(r, "only_while_cooking", cJSON_CreateTrue());
			}
			cJSON *sv = cJSON_GetObjectItem(g_root, "schema_version");
			if (sv) cJSON_SetNumberValue(sv, 8); else cJSON_AddNumberToObject(g_root, "schema_version", 8);
			LOGI(TAG, "settings migrated to schema 8 (%d probe rule%s now need the probe to be in the cook)", fixed, fixed == 1 ? "" : "s");
			added = 1;
		}
		/* after the migrations so a new release's built-in rules reach an existing settings file */
		if (adopt_builtin_rules(g_root, defaults)) added = 1;
		cJSON_Delete(defaults);
	} else {
		g_root = defaults;
		adopt_builtin_rules(g_root, g_root);   /* record what shipped, so none is offered twice */
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
