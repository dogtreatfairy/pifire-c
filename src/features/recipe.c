#include "features/recipe.h"
#include "core/db.h"
#include "core/settings.h"
#include "core/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pf_recipes_init(void)
{
	pf_db_exec("CREATE TABLE IF NOT EXISTS recipes(id INTEGER PRIMARY KEY, name TEXT, description TEXT, steps TEXT, updated REAL);");
}

cJSON *pf_recipes_list(void)
{
	cJSON *arr = cJSON_CreateArray();
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "SELECT id,name,description,steps FROM recipes ORDER BY name", -1, &st, NULL) != SQLITE_OK) return arr;
	while (sqlite3_step(st) == SQLITE_ROW) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "id", sqlite3_column_int(st, 0));
		cJSON_AddStringToObject(o, "name", (const char *)sqlite3_column_text(st, 1));
		cJSON_AddStringToObject(o, "description", sqlite3_column_text(st, 2) ? (const char *)sqlite3_column_text(st, 2) : "");
		cJSON *steps = sqlite3_column_text(st, 3) ? cJSON_Parse((const char *)sqlite3_column_text(st, 3)) : NULL;
		cJSON_AddItemToObject(o, "steps", steps ? steps : cJSON_CreateArray());
		cJSON_AddItemToArray(arr, o);
	}
	sqlite3_finalize(st);
	return arr;
}

int pf_recipe_save(const char *json, char *err, size_t errn)
{
	cJSON *j = cJSON_Parse(json);
	if (!j) { snprintf(err, errn, "invalid JSON"); return -1; }
	const char *name = pf_json_str(j, "name", "");
	cJSON *steps = cJSON_GetObjectItem(j, "steps");
	if (!*name || !cJSON_IsArray(steps) || cJSON_GetArraySize(steps) == 0) { cJSON_Delete(j); snprintf(err, errn, "name and at least one step required"); return -1; }
	if (cJSON_GetArraySize(steps) > PF_RECIPE_MAX_STEPS) { cJSON_Delete(j); snprintf(err, errn, "too many steps"); return -1; }
	cJSON *s;
	cJSON_ArrayForEach(s, steps) {
		if (pf_mode_from_name(pf_json_str(s, "mode", "")) < 0) { cJSON_Delete(j); snprintf(err, errn, "step has an unknown mode"); return -1; }
	}
	int id = pf_json_int(j, "id", 0);
	char *steps_txt = cJSON_PrintUnformatted(steps);
	sqlite3_stmt *st;
	int rc;
	if (id > 0) rc = sqlite3_prepare_v2(pf_db_handle(), "UPDATE recipes SET name=?,description=?,steps=?,updated=? WHERE id=?", -1, &st, NULL);
	else rc = sqlite3_prepare_v2(pf_db_handle(), "INSERT INTO recipes(name,description,steps,updated) VALUES(?,?,?,?)", -1, &st, NULL);
	if (rc != SQLITE_OK) { free(steps_txt); cJSON_Delete(j); return -1; }
	sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, pf_json_str(j, "description", ""), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, steps_txt, -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(st, 4, pf_wall());
	if (id > 0) sqlite3_bind_int(st, 5, id);
	rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	free(steps_txt);
	cJSON_Delete(j);
	if (rc) { snprintf(err, errn, "database error"); return -1; }
	return id > 0 ? id : (int)sqlite3_last_insert_rowid(pf_db_handle());
}

int pf_recipe_delete(int id)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "DELETE FROM recipes WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_int(st, 1, id);
	int rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	return rc;
}

int pf_recipe_load(int id, pf_recipe *out)
{
	memset(out, 0, sizeof *out);
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "SELECT name,steps FROM recipes WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_int(st, 1, id);
	if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
	out->id = id;
	pf_strlcpy(out->name, (const char *)sqlite3_column_text(st, 0), sizeof out->name);
	cJSON *steps = cJSON_Parse((const char *)sqlite3_column_text(st, 1));
	sqlite3_finalize(st);
	if (!steps) return -1;
	pf_units u = pf_settings_units();
	cJSON *s;
	cJSON_ArrayForEach(s, steps) {
		if (out->nsteps >= PF_RECIPE_MAX_STEPS) break;
		pf_recipe_step *rs = &out->steps[out->nsteps++];
		int m = pf_mode_from_name(pf_json_str(s, "mode", "Hold"));
		rs->mode = m >= 0 ? (pf_mode)m : PF_MODE_HOLD;
		double sp = pf_json_num(s, "setpoint", 0);
		rs->setpoint_c = sp > 0 ? pf_to_c(sp, u) : 0;
		rs->s_plus = pf_json_bool(s, "s_plus", false);
		rs->timer_s = pf_json_num(s, "timer_min", 0) * 60;
		pf_strlcpy(rs->probe, pf_json_str(s, "probe", ""), sizeof rs->probe);
		double pt = pf_json_num(s, "probe_temp", 0);
		rs->probe_temp_c = pt > 0 ? pf_to_c(pt, u) : 0;
		rs->pause = pf_json_bool(s, "pause", false);
		pf_strlcpy(rs->message, pf_json_str(s, "message", ""), sizeof rs->message);
	}
	cJSON_Delete(steps);
	return out->nsteps ? 0 : -1;
}
