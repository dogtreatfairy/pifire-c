#include "core/db.h"
#include "core/log.h"
#include "core/util.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TAG "db"
#define SCHEMA_VERSION 1

static sqlite3 *g_db;

static const char *schema_v1 =
	"CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
	"CREATE TABLE IF NOT EXISTS kv(ns TEXT NOT NULL, key TEXT NOT NULL, value TEXT, updated REAL, PRIMARY KEY(ns,key));"
	"CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY, ts REAL NOT NULL, level INTEGER, code TEXT, message TEXT);"
	"CREATE INDEX IF NOT EXISTS events_ts ON events(ts);"
	"CREATE TABLE IF NOT EXISTS history(ts REAL NOT NULL, mode INTEGER, setpoint REAL, u_raw REAL, u_applied REAL, fan_pct INTEGER, outputs INTEGER);"
	"CREATE INDEX IF NOT EXISTS history_ts ON history(ts);"
	/* `raw` and `ohms` are what the probe actually measured, before the filter. Keeping them is how
	   a real flare at ignition can be told from an electrical artifact after the fact, instead of
	   guessing from a smoothed curve. */
	"CREATE TABLE IF NOT EXISTS history_probe(ts REAL NOT NULL, label TEXT NOT NULL, temp REAL, target REAL, raw REAL, ohms REAL);"
	"CREATE TABLE IF NOT EXISTS history_ctrl(ts REAL NOT NULL, u_ff REAL, p REAL, i REAL, d REAL, ff REAL, ambient REAL, flags INTEGER, pmode INTEGER, cycle_s REAL);"
	"CREATE INDEX IF NOT EXISTS history_ctrl_ts ON history_ctrl(ts);"
	"CREATE INDEX IF NOT EXISTS history_probe_ts ON history_probe(ts, label);"
	"CREATE TABLE IF NOT EXISTS cooks(id INTEGER PRIMARY KEY, start_ts REAL, end_ts REAL, name TEXT, metrics TEXT);"
	"CREATE TABLE IF NOT EXISTS observations(id INTEGER PRIMARY KEY, ts REAL, controller TEXT, setpoint_c REAL, ambient_c REAL, u_mean REAL, pit_stdev REAL, pellet TEXT);"
	"CREATE TABLE IF NOT EXISTS episodes(id INTEGER PRIMARY KEY, start_ts REAL, end_ts REAL, kind TEXT, data TEXT);";

int pf_db_exec(const char *sql)
{
	char *err = NULL;
	int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
	if (rc != SQLITE_OK) {
		LOGE(TAG, "exec failed: %s (%s)", err ? err : "?", sql);
		sqlite3_free(err);
		return -1;
	}
	return 0;
}

static int meta_get_int(const char *key, int dflt)
{
	sqlite3_stmt *st;
	int v = dflt;
	if (sqlite3_prepare_v2(g_db, "SELECT value FROM meta WHERE key=?", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
		sqlite3_finalize(st);
	}
	return v;
}

static void meta_set_int(const char *key, int v)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
		sqlite3_bind_int(st, 2, v);
		sqlite3_step(st);
		sqlite3_finalize(st);
	}
}

int pf_db_open(const char *path)
{
	int rc = sqlite3_open_v2(path, &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
	if (rc != SQLITE_OK) {
		LOGE(TAG, "open %s: %s", path, sqlite3_errmsg(g_db));
		return -1;
	}
	sqlite3_busy_timeout(g_db, 5000);
	if (pf_db_exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA auto_vacuum=INCREMENTAL; PRAGMA temp_store=MEMORY;"))
		return -1;
	if (pf_db_exec(schema_v1)) return -1;
	int ver = meta_get_int("schema_version", 0);
	if (ver == 0) { meta_set_int("schema_version", SCHEMA_VERSION); ver = SCHEMA_VERSION; }
	/* A database written before the probe's unfiltered reading was kept: add the two columns. They
	 * already exist on a database created from the schema above, and the failure that produces is
	 * the intended answer, so it is ignored rather than prevented. */
	sqlite3_exec(g_db, "ALTER TABLE history_probe ADD COLUMN raw REAL", NULL, NULL, NULL);
	sqlite3_exec(g_db, "ALTER TABLE history_probe ADD COLUMN ohms REAL", NULL, NULL, NULL);
	LOGI(TAG, "opened %s (schema v%d)", path, ver);
	return 0;
}

void pf_db_close(void)
{
	if (g_db) { sqlite3_exec(g_db, "PRAGMA incremental_vacuum;", NULL, NULL, NULL); sqlite3_close(g_db); g_db = NULL; }
}

sqlite3 *pf_db_handle(void) { return g_db; }

/* ---------------- kv ---------------- */

int pf_db_kv_get(const char *ns, const char *key, char *out, size_t n)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(g_db, "SELECT value FROM kv WHERE ns=? AND key=?", -1, &st, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
	int rc = 1;
	if (sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *v = sqlite3_column_text(st, 0);
		pf_strlcpy(out, v ? (const char *)v : "", n);
		rc = 0;
	}
	sqlite3_finalize(st);
	return rc;
}

int pf_db_kv_put(const char *ns, const char *key, const char *json)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO kv(ns,key,value,updated) VALUES(?,?,?,?)", -1, &st, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 3, json, -1, SQLITE_STATIC);
	sqlite3_bind_double(st, 4, pf_wall());
	int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	return rc;
}

int pf_db_kv_delete_ns(const char *ns)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(g_db, "DELETE FROM kv WHERE ns=?", -1, &st, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC);
	int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	return rc;
}

/* ---------------- events ---------------- */

int pf_db_event(int level, const char *code, const char *message)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(g_db, "INSERT INTO events(ts,level,code,message) VALUES(?,?,?,?)", -1, &st, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_double(st, 1, pf_wall());
	sqlite3_bind_int(st, 2, level);
	sqlite3_bind_text(st, 3, code, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 4, message, -1, SQLITE_STATIC);
	int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	return rc;
}

cJSON *pf_db_events_recent(int limit)
{
	cJSON *arr = cJSON_CreateArray();
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(g_db, "SELECT ts,level,code,message FROM events ORDER BY id DESC LIMIT ?", -1, &st, NULL) != SQLITE_OK) return arr;
	sqlite3_bind_int(st, 1, limit > 0 ? limit : 100);
	while (sqlite3_step(st) == SQLITE_ROW) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "ts", sqlite3_column_double(st, 0));
		cJSON_AddNumberToObject(o, "level", sqlite3_column_int(st, 1));
		cJSON_AddStringToObject(o, "code", (const char *)sqlite3_column_text(st, 2));
		cJSON_AddStringToObject(o, "message", (const char *)sqlite3_column_text(st, 3));
		cJSON_AddItemToArray(arr, o);
	}
	sqlite3_finalize(st);
	return arr;
}

/* ---------------- history ---------------- */

int pf_db_history_write(const pf_hist_sample *samples, int n)
{
	if (n <= 0) return 0;
	sqlite3_stmt *s1 = NULL, *s2 = NULL, *s3 = NULL;
	if (sqlite3_prepare_v2(g_db, "INSERT INTO history(ts,mode,setpoint,u_raw,u_applied,fan_pct,outputs) VALUES(?,?,?,?,?,?,?)", -1, &s1, NULL) != SQLITE_OK ||
	    sqlite3_prepare_v2(g_db, "INSERT INTO history_probe(ts,label,temp,target,raw,ohms) VALUES(?,?,?,?,?,?)", -1, &s2, NULL) != SQLITE_OK ||
	    sqlite3_prepare_v2(g_db, "INSERT INTO history_ctrl(ts,u_ff,p,i,d,ff,ambient,flags,pmode,cycle_s) VALUES(?,?,?,?,?,?,?,?,?,?)", -1, &s3, NULL) != SQLITE_OK) {
		sqlite3_finalize(s1); sqlite3_finalize(s2); sqlite3_finalize(s3);
		return -1;
	}
	/* IMMEDIATE, so a second writer is told at once rather than part way through, and the whole
	 * batch is rolled back if a step fails instead of leaving a transaction open on the handle. */
	if (pf_db_exec("BEGIN IMMEDIATE")) {
		sqlite3_finalize(s1); sqlite3_finalize(s2); sqlite3_finalize(s3);
		return -1;
	}
	for (int i = 0; i < n; i++) {
		const pf_hist_sample *s = &samples[i];
		sqlite3_reset(s1);
		sqlite3_bind_double(s1, 1, s->ts);
		sqlite3_bind_int(s1, 2, s->mode);
		sqlite3_bind_double(s1, 3, s->setpoint);
		sqlite3_bind_double(s1, 4, s->u_raw);
		sqlite3_bind_double(s1, 5, s->u_applied);
		sqlite3_bind_int(s1, 6, s->fan_pct);
		sqlite3_bind_int(s1, 7, (int)s->outputs);
		sqlite3_step(s1);
		sqlite3_reset(s3);
		sqlite3_bind_double(s3, 1, s->ts);
		sqlite3_bind_double(s3, 2, s->u_ff); sqlite3_bind_double(s3, 3, s->p); sqlite3_bind_double(s3, 4, s->i); sqlite3_bind_double(s3, 5, s->d); sqlite3_bind_double(s3, 6, s->ff);
		if (isnan(s->ambient)) sqlite3_bind_null(s3, 7); else sqlite3_bind_double(s3, 7, s->ambient);
		sqlite3_bind_int(s3, 8, (int)s->flags); sqlite3_bind_int(s3, 9, s->pmode); sqlite3_bind_double(s3, 10, s->cycle_s);
		sqlite3_step(s3);
		for (int p = 0; p < s->nprobes; p++) {
			sqlite3_reset(s2);
			sqlite3_bind_double(s2, 1, s->ts);
			sqlite3_bind_text(s2, 2, s->probes[p].label, -1, SQLITE_STATIC);
			if (isnan(s->probes[p].temp)) sqlite3_bind_null(s2, 3);
			else sqlite3_bind_double(s2, 3, s->probes[p].temp);
			sqlite3_bind_double(s2, 4, s->probes[p].target);
			if (isnan(s->probes[p].raw)) sqlite3_bind_null(s2, 5);
			else sqlite3_bind_double(s2, 5, s->probes[p].raw);
			sqlite3_bind_double(s2, 6, s->probes[p].ohms);
			sqlite3_step(s2);
		}
	}
	int rc = pf_db_exec("COMMIT");
	if (rc) pf_db_exec("ROLLBACK");
	sqlite3_finalize(s1);
	sqlite3_finalize(s2);
	sqlite3_finalize(s3);
	return rc;
}

cJSON *pf_db_history_query(double from, double to, int res_s)
{
	cJSON *out = cJSON_CreateObject();
	cJSON *t = cJSON_AddArrayToObject(out, "t");
	cJSON *mode = cJSON_AddArrayToObject(out, "mode");
	cJSON *sp = cJSON_AddArrayToObject(out, "setpoint");
	cJSON *u = cJSON_AddArrayToObject(out, "u");
	cJSON *probes = cJSON_AddObjectToObject(out, "probes");
	if (res_s < 1) res_s = 1;

	sqlite3_stmt *st;
	const char *q1 =
		"SELECT CAST(ts/? AS INTEGER)*? AS b, MAX(mode), AVG(setpoint), AVG(u_applied) "
		"FROM history WHERE ts>=? AND ts<=? GROUP BY b ORDER BY b";
	if (sqlite3_prepare_v2(g_db, q1, -1, &st, NULL) != SQLITE_OK) return out;
	sqlite3_bind_int(st, 1, res_s); sqlite3_bind_int(st, 2, res_s);
	sqlite3_bind_double(st, 3, from); sqlite3_bind_double(st, 4, to);
	while (sqlite3_step(st) == SQLITE_ROW) {
		cJSON_AddItemToArray(t, cJSON_CreateNumber(sqlite3_column_double(st, 0)));
		cJSON_AddItemToArray(mode, cJSON_CreateNumber(sqlite3_column_int(st, 1)));
		cJSON_AddItemToArray(sp, cJSON_CreateNumber(sqlite3_column_double(st, 2)));
		cJSON_AddItemToArray(u, cJSON_CreateNumber(sqlite3_column_double(st, 3)));
	}
	sqlite3_finalize(st);

	int nt = cJSON_GetArraySize(t);
	const char *q2 =
		"SELECT label, CAST(ts/? AS INTEGER)*? AS b, AVG(temp), MAX(target) "
		"FROM history_probe WHERE ts>=? AND ts<=? GROUP BY label, b ORDER BY label, b";
	if (sqlite3_prepare_v2(g_db, q2, -1, &st, NULL) != SQLITE_OK) return out;
	sqlite3_bind_int(st, 1, res_s); sqlite3_bind_int(st, 2, res_s);
	sqlite3_bind_double(st, 3, from); sqlite3_bind_double(st, 4, to);
	cJSON *cur_temp = NULL, *cur_target = NULL;
	char cur_label[PF_LABEL_LEN] = "";
	int idx = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *label = (const char *)sqlite3_column_text(st, 0);
		double b = sqlite3_column_double(st, 1);
		if (strcmp(label, cur_label)) {
			/* pad previous series to full length */
			while (cur_temp && cJSON_GetArraySize(cur_temp) < nt) {
				cJSON_AddItemToArray(cur_temp, cJSON_CreateNull());
				cJSON_AddItemToArray(cur_target, cJSON_CreateNumber(0));
			}
			pf_strlcpy(cur_label, label, sizeof cur_label);
			cJSON *po = cJSON_AddObjectToObject(probes, label);
			cur_temp = cJSON_AddArrayToObject(po, "temp");
			cur_target = cJSON_AddArrayToObject(po, "target");
			idx = 0;
		}
		/* align to the time axis: fill nulls for buckets this label has no data in */
		while (idx < nt && cJSON_GetArrayItem(t, idx)->valuedouble < b - 0.5) {
			cJSON_AddItemToArray(cur_temp, cJSON_CreateNull());
			cJSON_AddItemToArray(cur_target, cJSON_CreateNumber(0));
			idx++;
		}
		if (sqlite3_column_type(st, 2) == SQLITE_NULL) cJSON_AddItemToArray(cur_temp, cJSON_CreateNull());
		else cJSON_AddItemToArray(cur_temp, cJSON_CreateNumber(sqlite3_column_double(st, 2)));
		cJSON_AddItemToArray(cur_target, cJSON_CreateNumber(sqlite3_column_double(st, 3)));
		idx++;
	}
	while (cur_temp && cJSON_GetArraySize(cur_temp) < nt) {
		cJSON_AddItemToArray(cur_temp, cJSON_CreateNull());
		cJSON_AddItemToArray(cur_target, cJSON_CreateNumber(0));
	}
	sqlite3_finalize(st);
	return out;
}

int pf_db_history_clear(void)
{
	return pf_db_exec("DELETE FROM history; DELETE FROM history_probe; DELETE FROM history_ctrl;");
}

int pf_db_history_prune(double older_than_ts)
{
	sqlite3_stmt *st;
	int rc = 0;
	const char *qs[] = { "DELETE FROM history WHERE ts<?", "DELETE FROM history_probe WHERE ts<?", "DELETE FROM history_ctrl WHERE ts<?" };
	for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) {
		if (sqlite3_prepare_v2(g_db, qs[i], -1, &st, NULL) != SQLITE_OK) return -1;
		sqlite3_bind_double(st, 1, older_than_ts);
		if (sqlite3_step(st) != SQLITE_DONE) rc = -1;
		sqlite3_finalize(st);
	}
	return rc;
}
