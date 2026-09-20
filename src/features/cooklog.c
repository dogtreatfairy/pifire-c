/* Cook log export: everything needed to analyse a cook offline and retune the controller.
 * Columnar samples (history + per-sample controller terms + probes), events, the settings that shaped
 * the run, and the learning state. Format documented in docs/cooklog.md. */
#include "features/cooklog.h"
#include "core/db.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/util.h"
#include "features/learning.h"
#include "features/update.h"
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static void add_setting(cJSON *dst, const char *key)
{
	cJSON *v = pf_set_dup(key);
	if (v) cJSON_AddItemToObject(dst, key, v);
}

static cJSON *samples_json(double from, double to)
{
	cJSON *o = cJSON_CreateObject();
	const char *cols[] = { "t", "mode", "setpoint", "u_raw", "u_applied", "u_ff", "p", "i", "d", "ff", "fan_pct", "outputs", "ambient", "flags", "pmode", "cycle_s" };
	cJSON *arr[16];
	for (int k = 0; k < 16; k++) arr[k] = cJSON_AddArrayToObject(o, cols[k]);
	cJSON *probes = cJSON_AddObjectToObject(o, "probes");
	sqlite3 *db = pf_db_handle();
	if (!db) return o;
	sqlite3_stmt *st;
	int n = 0;
	if (sqlite3_prepare_v2(db, "SELECT h.ts,h.mode,h.setpoint,h.u_raw,h.u_applied,c.u_ff,c.p,c.i,c.d,c.ff,h.fan_pct,h.outputs,c.ambient,c.flags,c.pmode,c.cycle_s "
	                           "FROM history h LEFT JOIN history_ctrl c ON c.ts=h.ts WHERE h.ts>=? AND h.ts<=? ORDER BY h.ts", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_double(st, 1, from);
		sqlite3_bind_double(st, 2, to);
		while (sqlite3_step(st) == SQLITE_ROW) {
			for (int k = 0; k < 16; k++) {
				if (sqlite3_column_type(st, k) == SQLITE_NULL) cJSON_AddItemToArray(arr[k], cJSON_CreateNull());
				else if (k == 1 || k == 10 || k == 11 || k == 13 || k == 14) cJSON_AddItemToArray(arr[k], cJSON_CreateNumber(sqlite3_column_int(st, k)));
				else cJSON_AddItemToArray(arr[k], cJSON_CreateNumber(round(sqlite3_column_double(st, k) * 1000) / 1000));
			}
			n++;
		}
		sqlite3_finalize(st);
	}
	/* probes: one column set per label, aligned to t by position (same ts order) */
	if (sqlite3_prepare_v2(db, "SELECT ts,label,temp,target FROM history_probe WHERE ts>=? AND ts<=? ORDER BY ts", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_double(st, 1, from);
		sqlite3_bind_double(st, 2, to);
		double last_ts = -1; int idx = -1;
		while (sqlite3_step(st) == SQLITE_ROW) {
			double ts = sqlite3_column_double(st, 0);
			if (ts != last_ts) { last_ts = ts; idx++; }
			const char *label = (const char *)sqlite3_column_text(st, 1);
			cJSON *pr = cJSON_GetObjectItem(probes, label);
			if (!pr) { pr = cJSON_AddObjectToObject(probes, label); cJSON_AddArrayToObject(pr, "temp"); cJSON_AddArrayToObject(pr, "target"); }
			cJSON *temp = cJSON_GetObjectItem(pr, "temp"), *target = cJSON_GetObjectItem(pr, "target");
			while (cJSON_GetArraySize(temp) < idx) { cJSON_AddItemToArray(temp, cJSON_CreateNull()); cJSON_AddItemToArray(target, cJSON_CreateNull()); }
			cJSON_AddItemToArray(temp, sqlite3_column_type(st, 2) == SQLITE_NULL ? cJSON_CreateNull() : cJSON_CreateNumber(round(sqlite3_column_double(st, 2) * 10) / 10));
			cJSON_AddItemToArray(target, cJSON_CreateNumber(sqlite3_column_double(st, 3)));
		}
		sqlite3_finalize(st);
	}
	cJSON_AddNumberToObject(o, "count", n);
	return o;
}

cJSON *pf_cooklog_json(double from, double to, const char *name)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "format", "pifire-cooklog/1");
	cJSON_AddNumberToObject(o, "generated", pf_wall());
	cJSON_AddStringToObject(o, "version", PF_VERSION);
	cJSON_AddStringToObject(o, "arch", pf_update_arch());
	cJSON_AddStringToObject(o, "units", "C");   /* every temperature in this file is Celsius, duties are 0..1 */
	cJSON *cook = cJSON_AddObjectToObject(o, "cook");
	cJSON_AddStringToObject(cook, "name", name ? name : "");
	cJSON_AddNumberToObject(cook, "start", from);
	cJSON_AddNumberToObject(cook, "end", to);
	cJSON_AddNumberToObject(cook, "duration_s", to - from);

	pf_status st;
	pf_status_get(&st);
	cJSON *live = cJSON_AddObjectToObject(o, "controller");
	cJSON_AddStringToObject(live, "id", st.controller_id);
	cJSON_AddStringToObject(live, "tuning_note", st.ctrl_dbg.note);

	cJSON *set = cJSON_AddObjectToObject(o, "settings");
	const char *keys[] = { "controller", "cycle_data", "safety", "startup", "shutdown", "smoke_plus", "pwm", "keep_warm", "learning", "globals.augerrate", "globals.units",
	                       "platform.current", "platform.dc_fan", "probe_settings.probe_map", "pelletlevel" };
	for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) add_setting(set, keys[k]);

	cJSON_AddItemToObject(o, "learning", pf_learning_json());
	cJSON_AddItemToObject(o, "samples", samples_json(from, to));

	cJSON *evs = cJSON_AddArrayToObject(o, "events");
	sqlite3_stmt *s2;
	if (pf_db_handle() && sqlite3_prepare_v2(pf_db_handle(), "SELECT ts,level,code,message FROM events WHERE ts>=? AND ts<=? ORDER BY id", -1, &s2, NULL) == SQLITE_OK) {
		sqlite3_bind_double(s2, 1, from - 60);
		sqlite3_bind_double(s2, 2, to + 60);
		while (sqlite3_step(s2) == SQLITE_ROW) {
			cJSON *e = cJSON_CreateObject();
			cJSON_AddNumberToObject(e, "ts", sqlite3_column_double(s2, 0));
			cJSON_AddNumberToObject(e, "level", sqlite3_column_int(s2, 1));
			cJSON_AddStringToObject(e, "code", (const char *)sqlite3_column_text(s2, 2));
			cJSON_AddStringToObject(e, "message", (const char *)sqlite3_column_text(s2, 3));
			cJSON_AddItemToArray(evs, e);
		}
		sqlite3_finalize(s2);
	}
	cJSON *legend = cJSON_AddObjectToObject(o, "legend");
	cJSON_AddStringToObject(legend, "mode", "0 Stop 1 Monitor 2 Prime 3 Startup 4 Reignite 5 Smoke 6 Hold 7 Shutdown 8 Manual 9 Error");
	cJSON_AddStringToObject(legend, "outputs", "bit0 power, bit1 fan, bit2 auger, bit3 igniter (pf_output order)");
	cJSON_AddStringToObject(legend, "flags", "1 lid_open, 2 smoke_plus, 4 pwm_control, 8 target_reached, 16 coldstart_active, 32 saturated_low, 64 saturated_high");
	cJSON_AddStringToObject(legend, "u", "auger duty fraction per cycle: u_raw = controller output, u_applied = after clamping to [u_min,u_max], u_ff = learned feed-forward; p/i/d/ff = controller terms");
	cJSON_AddStringToObject(legend, "temps", "Celsius; setpoint 0 outside Hold; probes.<label>.temp aligned to t by index");
	return o;
}

/* window of the running cook, else the most recent finished cook, else the last 6 hours */
int pf_cooklog_default_window(double *from, double *to, char *name, size_t n)
{
	pf_status st;
	pf_status_get(&st);
	if (st.cook_start_wall > 0) { *from = st.cook_start_wall; *to = pf_wall(); snprintf(name, n, "current cook"); return 0; }
	sqlite3_stmt *s;
	if (pf_db_handle() && sqlite3_prepare_v2(pf_db_handle(), "SELECT start_ts,end_ts,name FROM cooks ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK) {
		int rc = sqlite3_step(s);
		if (rc == SQLITE_ROW) {
			*from = sqlite3_column_double(s, 0); *to = sqlite3_column_double(s, 1);
			snprintf(name, n, "%s", (const char *)sqlite3_column_text(s, 2));
			sqlite3_finalize(s);
			return 0;
		}
		sqlite3_finalize(s);
	}
	*to = pf_wall(); *from = *to - 6 * 3600;
	snprintf(name, n, "last 6 hours");
	return 0;
}

int pf_cooklog_cook_window(int id, double *from, double *to, char *name, size_t n)
{
	sqlite3_stmt *s;
	if (!pf_db_handle() || sqlite3_prepare_v2(pf_db_handle(), "SELECT start_ts,end_ts,name FROM cooks WHERE id=?", -1, &s, NULL) != SQLITE_OK) return -1;
	sqlite3_bind_int(s, 1, id);
	int rc = -1;
	if (sqlite3_step(s) == SQLITE_ROW) { *from = sqlite3_column_double(s, 0); *to = sqlite3_column_double(s, 1); snprintf(name, n, "%s", (const char *)sqlite3_column_text(s, 2)); rc = 0; }
	sqlite3_finalize(s);
	return rc;
}
