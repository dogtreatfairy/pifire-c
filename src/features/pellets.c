#include "features/pellets.h"
#include "core/db.h"
#include "core/env.h"
#include "core/events.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "distance/registry.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "pellets"

static const pf_distance_ops *g_ops;
static void *g_inst;
static pf_env g_env;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_pct = -1;
static double g_cm = -1, g_updated;
static double g_last_check, g_last_warn, g_last_auger_total, g_est_usage_g;
static atomic_bool g_check_req;
static int g_current_id;

static void load_state(void)
{
	char buf[256];
	if (pf_db_kv_get("pellets", "state", buf, sizeof buf) == 0) {
		cJSON *j = cJSON_Parse(buf);
		g_current_id = pf_json_int(j, "current_id", 0);
		g_est_usage_g = pf_json_num(j, "est_usage_g", 0);
		cJSON_Delete(j);
	}
}

static void save_state(void)
{
	char buf[128];
	snprintf(buf, sizeof buf, "{\"current_id\":%d,\"est_usage_g\":%.1f}", g_current_id, g_est_usage_g);
	pf_db_kv_put("pellets", "state", buf);
}

static void log_entry(int pellet_id, const char *text)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "INSERT INTO pellet_log(ts,pellet_id,text,hopper_pct) VALUES(?,?,?,?)", -1, &st, NULL) != SQLITE_OK) return;
	sqlite3_bind_double(st, 1, pf_wall());
	sqlite3_bind_int(st, 2, pellet_id);
	sqlite3_bind_text(st, 3, text, -1, SQLITE_STATIC);
	sqlite3_bind_int(st, 4, atomic_load(&g_pct));
	sqlite3_step(st);
	sqlite3_finalize(st);
}

int pf_pellets_init(bool sim)
{
	pf_db_exec("CREATE TABLE IF NOT EXISTS pellets(id INTEGER PRIMARY KEY, brand TEXT, wood TEXT, rating INTEGER, comments TEXT, added_ts REAL);"
	           "CREATE TABLE IF NOT EXISTS pellet_log(id INTEGER PRIMARY KEY, ts REAL, pellet_id INTEGER, text TEXT, hopper_pct INTEGER);");
	load_state();
	char mod[32];
	pf_set_str("modules.dist", mod, sizeof mod, "none");
	if (sim && !strcmp(mod, "none")) pf_strlcpy(mod, "sim", sizeof mod);
	g_ops = pf_distance_find(mod);
	pf_env_init(&g_env, "distance");
	cJSON *cfg = pf_set_dup("platform.devices.distance");
	if (!cfg) cfg = cJSON_CreateObject();
	cJSON *extra = pf_set_dup("distance");
	if (extra) { pf_json_merge(cfg, extra); cJSON_Delete(extra); }
	char chip[64];
	pf_set_str("platform.gpiochip", chip, sizeof chip, "/dev/gpiochip0");
	cJSON_AddStringToObject(cfg, "gpiochip", chip);
	char *js = cJSON_PrintUnformatted(cfg);
	cJSON_Delete(cfg);
	g_inst = g_ops->create(js, &g_env);
	free(js);
	if (!g_inst) { LOGW(TAG, "distance sensor '%s' unavailable; hopper level disabled", mod); g_ops = pf_distance_none(); g_inst = g_ops->create("{}", &g_env); }
	else if (strcmp(g_ops->id, "none")) LOGI(TAG, "hopper sensor: %s", g_ops->name);
	if (g_current_id == 0) {
		/* seed a default profile so the UI has something to show */
		sqlite3_stmt *st;
		if (sqlite3_prepare_v2(pf_db_handle(), "SELECT id FROM pellets LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
			if (sqlite3_step(st) == SQLITE_ROW) g_current_id = sqlite3_column_int(st, 0);
			sqlite3_finalize(st);
		}
		if (g_current_id == 0) {
			pf_pellets_profile_save("{\"brand\":\"Generic\",\"wood\":\"Hickory\",\"rating\":4,\"comments\":\"\"}", NULL, 0);
			pf_db_exec("SELECT 1");
			sqlite3_stmt *s2;
			if (sqlite3_prepare_v2(pf_db_handle(), "SELECT id FROM pellets ORDER BY id DESC LIMIT 1", -1, &s2, NULL) == SQLITE_OK) {
				if (sqlite3_step(s2) == SQLITE_ROW) g_current_id = sqlite3_column_int(s2, 0);
				sqlite3_finalize(s2);
			}
			save_state();
		}
	}
	atomic_store(&g_check_req, true);
	return 0;
}

void pf_pellets_shutdown(void)
{
	if (g_ops && g_inst) g_ops->destroy(g_inst);
	g_inst = NULL;
}

static void read_hopper(void)
{
	if (!g_ops || !strcmp(g_ops->id, "none")) { atomic_store(&g_pct, -1); return; }
	/* median of three readings */
	double r[3];
	int k = 0;
	for (int i = 0; i < 3; i++) { double x = g_ops->read_cm(g_inst); if (x > 0) r[k++] = x; if (i < 2) pf_sleep_ms(50); }
	double cm = -1;
	if (k == 3) cm = (r[0] > r[1]) == (r[0] < r[2]) ? r[0] : (r[1] > r[0]) == (r[1] < r[2]) ? r[1] : r[2];
	else if (k == 2) cm = (r[0] + r[1]) / 2;
	else if (k == 1) cm = r[0];
	double empty = pf_set_num("pelletlevel.empty", 22), full = pf_set_num("pelletlevel.full", 4);
	pthread_mutex_lock(&g_mu);
	g_updated = pf_wall();
	if (cm < 0 || empty <= full) { g_cm = -1; atomic_store(&g_pct, -1); }
	else {
		g_cm = cm;
		double pct = (empty - cm) / (empty - full) * 100.0;
		atomic_store(&g_pct, (int)pf_clamp(pct, 0, 100));
	}
	pthread_mutex_unlock(&g_mu);
}

void pf_pellets_tick(double now, double auger_on_total_s, bool cooking)
{
	if (atomic_exchange(&g_check_req, false) || now - g_last_check > 60) {
		g_last_check = now;
		read_hopper();
	}
	if (auger_on_total_s < g_last_auger_total) g_last_auger_total = auger_on_total_s; /* new cook reset */
	double delta = auger_on_total_s - g_last_auger_total;
	if (delta > 0) {
		g_last_auger_total = auger_on_total_s;
		g_est_usage_g += delta * pf_set_num("globals.augerrate", 0.3);
		static double last_save;
		if (now - last_save > 60) { last_save = now; save_state(); }
	}
	int pct = atomic_load(&g_pct);
	if (cooking && pct >= 0 && pf_set_bool("pelletlevel.warning_enabled", true) && pct <= pf_set_int("pelletlevel.warning_level", 25) &&
	    now - g_last_warn > pf_set_num("pelletlevel.warning_time", 20) * 60) {
		g_last_warn = now;
		pf_events_emit("Pellet_Level_Low", "Pellets running low", "Hopper is at %d%%.", pct);
	}
}

void pf_pellets_request_check(void) { atomic_store(&g_check_req, true); }
int pf_pellets_hopper_pct(void) { return atomic_load(&g_pct); }

cJSON *pf_pellets_json(void)
{
	cJSON *o = cJSON_CreateObject();
	sqlite3 *db = pf_db_handle();
	cJSON *cur = cJSON_AddObjectToObject(o, "current");
	cJSON_AddNumberToObject(cur, "id", g_current_id);
	cJSON_AddNumberToObject(cur, "est_usage_g", g_est_usage_g);
	sqlite3_stmt *st;
	cJSON *profiles = cJSON_AddArrayToObject(o, "profiles");
	if (sqlite3_prepare_v2(db, "SELECT id,brand,wood,rating,comments,added_ts FROM pellets ORDER BY brand,wood", -1, &st, NULL) == SQLITE_OK) {
		while (sqlite3_step(st) == SQLITE_ROW) {
			cJSON *p = cJSON_CreateObject();
			int id = sqlite3_column_int(st, 0);
			cJSON_AddNumberToObject(p, "id", id);
			cJSON_AddStringToObject(p, "brand", (const char *)sqlite3_column_text(st, 1));
			cJSON_AddStringToObject(p, "wood", (const char *)sqlite3_column_text(st, 2));
			cJSON_AddNumberToObject(p, "rating", sqlite3_column_int(st, 3));
			cJSON_AddStringToObject(p, "comments", sqlite3_column_text(st, 4) ? (const char *)sqlite3_column_text(st, 4) : "");
			cJSON_AddItemToArray(profiles, p);
			if (id == g_current_id) { cJSON_AddStringToObject(cur, "brand", (const char *)sqlite3_column_text(st, 1)); cJSON_AddStringToObject(cur, "wood", (const char *)sqlite3_column_text(st, 2)); }
		}
		sqlite3_finalize(st);
	}
	cJSON *h = cJSON_AddObjectToObject(o, "hopper");
	pthread_mutex_lock(&g_mu);
	cJSON_AddNumberToObject(h, "pct", atomic_load(&g_pct));
	cJSON_AddNumberToObject(h, "cm", g_cm);
	cJSON_AddNumberToObject(h, "updated", g_updated);
	pthread_mutex_unlock(&g_mu);
	cJSON_AddBoolToObject(h, "enabled", g_ops && strcmp(g_ops->id, "none") != 0);
	cJSON *log = cJSON_AddArrayToObject(o, "log");
	if (sqlite3_prepare_v2(db, "SELECT l.ts,l.text,l.hopper_pct,p.brand,p.wood FROM pellet_log l LEFT JOIN pellets p ON p.id=l.pellet_id ORDER BY l.id DESC LIMIT 50", -1, &st, NULL) == SQLITE_OK) {
		while (sqlite3_step(st) == SQLITE_ROW) {
			cJSON *e = cJSON_CreateObject();
			cJSON_AddNumberToObject(e, "ts", sqlite3_column_double(st, 0));
			cJSON_AddStringToObject(e, "text", (const char *)sqlite3_column_text(st, 1));
			cJSON_AddNumberToObject(e, "hopper_pct", sqlite3_column_int(st, 2));
			cJSON_AddStringToObject(e, "brand", sqlite3_column_text(st, 3) ? (const char *)sqlite3_column_text(st, 3) : "");
			cJSON_AddStringToObject(e, "wood", sqlite3_column_text(st, 4) ? (const char *)sqlite3_column_text(st, 4) : "");
			cJSON_AddItemToArray(log, e);
		}
		sqlite3_finalize(st);
	}
	return o;
}

int pf_pellets_profile_save(const char *json, char *err, size_t errn)
{
	cJSON *j = cJSON_Parse(json);
	if (!j) { if (err) snprintf(err, errn, "invalid JSON"); return -1; }
	int id = pf_json_int(j, "id", 0);
	const char *brand = pf_json_str(j, "brand", ""), *wood = pf_json_str(j, "wood", ""), *comments = pf_json_str(j, "comments", "");
	int rating = pf_json_int(j, "rating", 3);
	if (!*brand) { cJSON_Delete(j); if (err) snprintf(err, errn, "brand required"); return -1; }
	sqlite3_stmt *st;
	int rc;
	if (id > 0) rc = sqlite3_prepare_v2(pf_db_handle(), "UPDATE pellets SET brand=?,wood=?,rating=?,comments=? WHERE id=?", -1, &st, NULL);
	else rc = sqlite3_prepare_v2(pf_db_handle(), "INSERT INTO pellets(brand,wood,rating,comments,added_ts) VALUES(?,?,?,?,?)", -1, &st, NULL);
	if (rc != SQLITE_OK) { cJSON_Delete(j); return -1; }
	sqlite3_bind_text(st, 1, brand, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, wood, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 3, rating);
	sqlite3_bind_text(st, 4, comments, -1, SQLITE_TRANSIENT);
	if (id > 0) sqlite3_bind_int(st, 5, id); else sqlite3_bind_double(st, 5, pf_wall());
	rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	cJSON_Delete(j);
	return rc;
}

int pf_pellets_profile_delete(int id)
{
	if (id == g_current_id) return -1;
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "DELETE FROM pellets WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_int(st, 1, id);
	int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	return rc;
}

int pf_pellets_load(int id)
{
	g_current_id = id;
	g_est_usage_g = 0;
	save_state();
	log_entry(id, "Loaded pellets");
	pf_pellets_request_check();
	return 0;
}
