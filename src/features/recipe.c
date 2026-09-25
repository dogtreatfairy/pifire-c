#include "features/recipe.h"
#include "core/db.h"
#include "core/settings.h"
#include "core/util.h"
#include "core/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pf_recipes_init(void)
{
	pf_db_exec("CREATE TABLE IF NOT EXISTS recipes(id INTEGER PRIMARY KEY, name TEXT, description TEXT, steps TEXT, updated REAL);");
	/* The temperatures in a recipe are written in the unit the cook wrote them in, and the unit is
	 * stored with them. Without it, switching the grill to Celsius read 225 as 225 C -- the numbers
	 * had no unit of their own and silently took whichever one the grill happened to be in. Rows
	 * written before this column existed are Fahrenheit or Celsius according to the grill's own
	 * setting, which is what they were saved as. */
	pf_db_exec("ALTER TABLE recipes ADD COLUMN units TEXT");   /* fails harmlessly once it is there */
}

cJSON *pf_recipes_list(void)
{
	cJSON *arr = cJSON_CreateArray();
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "SELECT id,name,description,steps,units FROM recipes ORDER BY name", -1, &st, NULL) != SQLITE_OK) return arr;
	while (sqlite3_step(st) == SQLITE_ROW) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "id", sqlite3_column_int(st, 0));
		cJSON_AddStringToObject(o, "name", (const char *)sqlite3_column_text(st, 1));
		cJSON_AddStringToObject(o, "description", sqlite3_column_text(st, 2) ? (const char *)sqlite3_column_text(st, 2) : "");
		cJSON *steps = sqlite3_column_text(st, 3) ? cJSON_Parse((const char *)sqlite3_column_text(st, 3)) : NULL;
		cJSON_AddItemToObject(o, "steps", steps ? steps : cJSON_CreateArray());
		const char *un = (const char *)sqlite3_column_text(st, 4);
		cJSON_AddStringToObject(o, "units", un && *un ? un : (pf_settings_units() == PF_UNITS_C ? "C" : "F"));
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
	if (id > 0) rc = sqlite3_prepare_v2(pf_db_handle(), "UPDATE recipes SET name=?,description=?,steps=?,updated=?,units=? WHERE id=?", -1, &st, NULL);
	else rc = sqlite3_prepare_v2(pf_db_handle(), "INSERT INTO recipes(name,description,steps,updated,units) VALUES(?,?,?,?,?)", -1, &st, NULL);
	if (rc != SQLITE_OK) { free(steps_txt); cJSON_Delete(j); return -1; }
	sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, pf_json_str(j, "description", ""), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 3, steps_txt, -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(st, 4, pf_wall());
	/* Stamped with the unit the numbers are in, which is the one the editor was showing. */
	const char *un = pf_json_str(j, "units", pf_settings_units() == PF_UNITS_C ? "C" : "F");
	sqlite3_bind_text(st, 5, (*un == 'C' || *un == 'c') ? "C" : "F", -1, SQLITE_TRANSIENT);
	if (id > 0) sqlite3_bind_int(st, 6, id);
	rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
	sqlite3_finalize(st);
	free(steps_txt);
	cJSON_Delete(j);
	if (rc) { snprintf(err, errn, "database error"); return -1; }
	return id > 0 ? id : (int)sqlite3_last_insert_rowid(pf_db_handle());
}

/* The shape a recipe is meant to have: it lights the grill before it cooks anything, and it puts
 * the grill out when it is done.
 *
 * Advisory, not enforced. A recipe of nothing but Shutdown is a cool-down, and quietly prepending
 * a Startup to that would light a grill somebody had asked to put out -- so this says what is
 * missing and leaves the decision where it belongs. Ending without a Shutdown is allowed outright:
 * a recipe that hands back a hot grill on purpose is a real thing, and the runner asks what to do
 * about it when it gets there. The editor mirrors these two rules so it can say so while the
 * recipe is being written rather than after it has been saved. */
cJSON *pf_recipe_shape_warnings(const cJSON *steps)
{
	cJSON *out = cJSON_CreateArray();
	int n = cJSON_GetArraySize((cJSON *)steps);
	if (n <= 0) return out;
	const char *first = pf_json_str(cJSON_GetArrayItem((cJSON *)steps, 0), "mode", "");
	const char *last = pf_json_str(cJSON_GetArrayItem((cJSON *)steps, n - 1), "mode", "");
	bool cooks = false;
	cJSON *st;
	cJSON_ArrayForEach(st, (cJSON *)steps) {
		const char *m = pf_json_str(st, "mode", "");
		if (!strcmp(m, "Startup") || !strcmp(m, "Smoke") || !strcmp(m, "Hold")) { cooks = true; break; }
	}
	if (cooks && strcmp(first, "Startup")) {
		cJSON *w = cJSON_CreateObject();
		cJSON_AddStringToObject(w, "code", "no_startup");
		cJSON_AddStringToObject(w, "message", "This recipe cooks but does not start with a Startup step, so it will not light a cold grill.");
		cJSON_AddItemToArray(out, w);
	}
	if (strcmp(last, "Shutdown")) {
		cJSON *w = cJSON_CreateObject();
		cJSON_AddStringToObject(w, "code", "no_shutdown");
		cJSON_AddStringToObject(w, "message", "This recipe does not end with a Shutdown step, so it will leave the grill running when it finishes.");
		cJSON_AddItemToArray(out, w);
	}
	return out;
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
	if (sqlite3_prepare_v2(pf_db_handle(), "SELECT name,steps,units FROM recipes WHERE id=?", -1, &st, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_int(st, 1, id);
	if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
	out->id = id;
	pf_strlcpy(out->name, (const char *)sqlite3_column_text(st, 0), sizeof out->name);
	cJSON *steps = cJSON_Parse((const char *)sqlite3_column_text(st, 1));
	const char *un = (const char *)sqlite3_column_text(st, 2);
	/* Read in the unit the recipe was written in, not the one the grill is in now. */
	pf_units u = un && (*un == 'C' || *un == 'c') ? PF_UNITS_C : un && *un ? PF_UNITS_F : pf_settings_units();
	sqlite3_finalize(st);
	if (!steps) return -1;
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
		rs->probe_all = !strcmp(pf_json_str(s, "probe_match", "any"), "all");
		rs->carryover = pf_json_bool(s, "carryover", false);
		const char *w = pf_json_str(s, "wait", "none");
		rs->wait = !strcmp(w, "lid") ? PF_RSTEP_WAIT_LID : !strcmp(w, "confirm") ? PF_RSTEP_WAIT_CONFIRM : PF_RSTEP_WAIT_NONE;
		/* "wait" says how the step ends and subsumes the older "pause" flag, which said only that
		 * it ended by asking. A recipe written before wait existed still reads correctly. */
		rs->pause = pf_json_bool(s, "pause", false) || rs->wait != PF_RSTEP_WAIT_NONE;
		pf_strlcpy(rs->message, pf_json_str(s, "message", ""), sizeof rs->message);
		rs->lead_s = pf_json_num(s, "lead_min", 0) * 60;
		pf_strlcpy(rs->lead_message, pf_json_str(s, "lead_message", ""), sizeof rs->lead_message);
	}
	cJSON_Delete(steps);
	return out->nsteps ? 0 : -1;
}

/* ------------------------------------------------------------------ built-in recipes */

/* 3-2-1 ribs, written out as the grill has to run it.
 *
 * The method is three hours of smoke, two wrapped in foil, one back open with sauce -- but the
 * hours are only how long it usually takes, and the parts of it that are not time are what make it
 * work: the smoke ends at 160 F if the ribs get there first, the last hour ends at 205 F, and the
 * two points where the cook has to handle the meat are points where the grill waits rather than
 * counts. Every handover is warned about before it arrives, because being told to wrap the ribs at
 * the moment they need wrapping means fetching foil with the lid open.
 *
 * The 205 is where the ribs finish, not where they come off: carryover pulls them early by as much
 * as they will still climb while they rest.
 *
 * Written in Fahrenheit and stamped as such, so it reads the same on a grill set to Celsius. */
static const char *RIBS_321 =
"{\"name\":\"3-2-1 Ribs\",\"units\":\"F\","
"\"description\":\"Three hours of smoke, two wrapped, one sauced. Waits for you at each handover.\","
"\"steps\":["
 "{\"mode\":\"Startup\",\"setpoint\":180,\"message\":\"Lighting, then holding at 180 F.\"},"
 "{\"mode\":\"Hold\",\"setpoint\":180,\"wait\":\"confirm\","
  "\"message\":\"Put the ribs on and tap Next.\"},"
 "{\"mode\":\"Hold\",\"setpoint\":180,\"timer_min\":180,\"probe\":\"@food\",\"probe_temp\":160,"
  "\"lead_min\":10,\"lead_message\":\"Ribs come off to wrap in about 10 minutes. Get the foil out.\","
  "\"wait\":\"lid\",\"message\":\"Take the ribs off and wrap them in foil.\"},"
 "{\"mode\":\"Hold\",\"setpoint\":225,\"wait\":\"confirm\","
  "\"message\":\"Grill is going to 225 F. Put the wrapped ribs back on and tap Next.\"},"
 "{\"mode\":\"Hold\",\"setpoint\":225,\"timer_min\":120,"
  "\"lead_min\":10,\"lead_message\":\"Foil comes off in about 10 minutes. Get the sauce out.\","
  "\"wait\":\"confirm\",\"message\":\"Take the foil off, baste with sauce, and tap Next.\"},"
 "{\"mode\":\"Hold\",\"setpoint\":225,\"timer_min\":60,\"probe\":\"@food\",\"probe_temp\":205,\"carryover\":true,"
  "\"lead_min\":2,\"lead_message\":\"Ribs come off in about 2 minutes.\","
  "\"wait\":\"confirm\",\"message\":\"Ribs are done. Take them off and rest them.\"},"
 "{\"mode\":\"Shutdown\",\"message\":\"Shutting down.\"}"
"]}";

/* Seeded once, and only once: a built-in recipe the cook deleted stays deleted, and one they
 * edited keeps their edit. */
void pf_recipes_seed(void)
{
	char buf[16];
	if (pf_db_kv_get("recipes", "seeded_v1", buf, sizeof buf) == 0) return;
	char err[128];
	if (pf_recipe_save(RIBS_321, err, sizeof err) < 0) { LOGW("recipe", "built-in 3-2-1 ribs: %s", err); return; }
	pf_db_kv_put("recipes", "seeded_v1", "true");
	LOGI("recipe", "added the built-in 3-2-1 Ribs recipe");
}
