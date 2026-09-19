#include "features/cookfile.h"
#include "core/db.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "pifire/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TAG "cookfile"

static char g_dir[512];

void pf_cookfile_init(const char *data_dir)
{
	snprintf(g_dir, sizeof g_dir, "%s/cookfiles", data_dir);
	pf_mkdir_p(g_dir);
	pf_db_exec("CREATE TABLE IF NOT EXISTS cooks(id INTEGER PRIMARY KEY, start_ts REAL, end_ts REAL, name TEXT, metrics TEXT);");
}

static void path_for(int id, char *out, size_t n) { snprintf(out, n, "%s/cook-%d.json", g_dir, id); }

int pf_cookfile_finish(double start_wall, double end_wall, double auger_on_s, double max_pit_c)
{
	if (end_wall - start_wall < 120) return -1; /* nothing worth keeping */
	char name[64];
	time_t t = (time_t)start_wall;
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(name, sizeof name, "Cook %Y-%m-%d %H:%M", &tm);

	cJSON *file = cJSON_CreateObject();
	cJSON_AddStringToObject(file, "name", name);
	cJSON_AddNumberToObject(file, "start", start_wall);
	cJSON_AddNumberToObject(file, "end", end_wall);
	cJSON_AddStringToObject(file, "units", pf_settings_units() == PF_UNITS_C ? "C" : "F");
	cJSON *metrics = cJSON_AddObjectToObject(file, "metrics");
	cJSON_AddNumberToObject(metrics, "duration_s", end_wall - start_wall);
	cJSON_AddNumberToObject(metrics, "auger_on_s", auger_on_s);
	cJSON_AddNumberToObject(metrics, "pellets_g", auger_on_s * pf_set_num("globals.augerrate", 0.3));
	cJSON_AddNumberToObject(metrics, "max_pit", pf_from_c(max_pit_c, pf_settings_units()));
	cJSON *hist = pf_db_history_query(start_wall, end_wall, 0);
	cJSON_AddItemToObject(file, "history", hist);
	cJSON *evs = cJSON_CreateArray();
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "SELECT ts,level,code,message FROM events WHERE ts>=? AND ts<=? ORDER BY id", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_double(st, 1, start_wall);
		sqlite3_bind_double(st, 2, end_wall);
		while (sqlite3_step(st) == SQLITE_ROW) {
			cJSON *e = cJSON_CreateObject();
			cJSON_AddNumberToObject(e, "ts", sqlite3_column_double(st, 0));
			cJSON_AddNumberToObject(e, "level", sqlite3_column_int(st, 1));
			cJSON_AddStringToObject(e, "code", (const char *)sqlite3_column_text(st, 2));
			cJSON_AddStringToObject(e, "message", (const char *)sqlite3_column_text(st, 3));
			cJSON_AddItemToArray(evs, e);
		}
		sqlite3_finalize(st);
	}
	cJSON_AddItemToObject(file, "events", evs);

	char *mtxt = cJSON_PrintUnformatted(metrics);
	if (sqlite3_prepare_v2(pf_db_handle(), "INSERT INTO cooks(start_ts,end_ts,name,metrics) VALUES(?,?,?,?)", -1, &st, NULL) != SQLITE_OK) { free(mtxt); cJSON_Delete(file); return -1; }
	sqlite3_bind_double(st, 1, start_wall);
	sqlite3_bind_double(st, 2, end_wall);
	sqlite3_bind_text(st, 3, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 4, mtxt, -1, SQLITE_STATIC);
	int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	free(mtxt);
	if (rc) { cJSON_Delete(file); return -1; }
	int id = (int)sqlite3_last_insert_rowid(pf_db_handle());
	cJSON_AddNumberToObject(file, "id", id);
	char *txt = cJSON_PrintUnformatted(file);
	cJSON_Delete(file);
	char path[600];
	path_for(id, path, sizeof path);
	rc = pf_write_file_atomic(path, txt, strlen(txt));
	free(txt);
	if (rc) { LOGE(TAG, "write %s failed", path); return -1; }
	LOGI(TAG, "saved %s (%s)", path, name);
	return id;
}

cJSON *pf_cookfile_list(void)
{
	cJSON *arr = cJSON_CreateArray();
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "SELECT id,start_ts,end_ts,name,metrics FROM cooks ORDER BY start_ts DESC", -1, &st, NULL) != SQLITE_OK) return arr;
	while (sqlite3_step(st) == SQLITE_ROW) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "id", sqlite3_column_int(st, 0));
		cJSON_AddNumberToObject(o, "start", sqlite3_column_double(st, 1));
		cJSON_AddNumberToObject(o, "end", sqlite3_column_double(st, 2));
		cJSON_AddStringToObject(o, "name", (const char *)sqlite3_column_text(st, 3));
		cJSON *m = sqlite3_column_text(st, 4) ? cJSON_Parse((const char *)sqlite3_column_text(st, 4)) : NULL;
		cJSON_AddItemToObject(o, "metrics", m ? m : cJSON_CreateObject());
		cJSON_AddItemToArray(arr, o);
	}
	sqlite3_finalize(st);
	return arr;
}

char *pf_cookfile_read(int id)
{
	char path[600];
	path_for(id, path, sizeof path);
	return pf_read_file(path, NULL);
}

int pf_cookfile_delete(int id)
{
	char path[600];
	path_for(id, path, sizeof path);
	unlink(path);
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "DELETE FROM cooks WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_int(st, 1, id);
	int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	return rc;
}

int pf_cookfile_rename(int id, const char *name)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "UPDATE cooks SET name=? WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(st, 2, id);
	int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	/* keep the file's name in sync */
	char *txt = pf_cookfile_read(id);
	if (txt) {
		cJSON *j = cJSON_Parse(txt);
		free(txt);
		if (j) {
			cJSON *n = cJSON_GetObjectItem(j, "name");
			if (n) cJSON_SetValuestring(n, name); else cJSON_AddStringToObject(j, "name", name);
			char *out = cJSON_PrintUnformatted(j);
			cJSON_Delete(j);
			char path[600];
			path_for(id, path, sizeof path);
			pf_write_file_atomic(path, out, strlen(out));
			free(out);
		}
	}
	return rc;
}
