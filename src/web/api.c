#define _GNU_SOURCE
#include "web/api.h"
#include "core/cmdq.h"
#include "core/db.h"
#include "core/embedded.h"
#include "core/events.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/util.h"
#include "controllers/registry.h"
#include "features/cookfile.h"
#include "features/learning.h"
#include "features/pellets.h"
#include "features/recipe.h"
#include "net/netmgr.h"
#include "net/sysinfo.h"
#include "net/wifi.h"
#include "probes/ble/bluez.h"
#include "probes/probes.h"
#include <cJSON.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "api"

static atomic_bool g_hotspot;
bool pf_api_hotspot_active(void) { return atomic_load(&g_hotspot); }
void pf_api_set_hotspot_active(bool on) { atomic_store(&g_hotspot, on); }

/* ---------------- response helpers ---------------- */

static void reply(pf_api_resp *r, int status, cJSON *obj_owned)
{
	r->status = status;
	r->json = obj_owned ? cJSON_PrintUnformatted(obj_owned) : NULL;
	cJSON_Delete(obj_owned);
}

static void reply_ok(pf_api_resp *r)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "result", "OK");
	reply(r, 200, o);
}

static void reply_err(pf_api_resp *r, int status, const char *msg)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "result", "ERROR");
	cJSON_AddStringToObject(o, "message", msg);
	reply(r, status, o);
}

static bool query_get(const char *query, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	const char *p = query;
	while (p && *p) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			const char *v = p + kl + 1, *e = strchr(v, '&');
			size_t vl = e ? (size_t)(e - v) : strlen(v);
			if (vl >= n) vl = n - 1;
			memcpy(out, v, vl); out[vl] = 0;
			return true;
		}
		p = strchr(p, '&');
		if (p) p++;
	}
	return false;
}

static double query_num(const char *query, const char *key, double dflt)
{
	char v[32];
	return query_get(query, key, v, sizeof v) ? atof(v) : dflt;
}

/* ---------------- commands ---------------- */

int pf_api_command_json(const char *json, char *err, size_t errn)
{
	cJSON *j = cJSON_Parse(json);
	if (!j) { snprintf(err, errn, "invalid JSON"); return -1; }
	const char *cmd = pf_json_str(j, "cmd", "");
	int rc = 0;
	pf_cmd c;
	memset(&c, 0, sizeof c);
	if (!strcmp(cmd, "mode")) {
		int m = pf_mode_from_name(pf_json_str(j, "mode", ""));
		if (m < 0) { snprintf(err, errn, "unknown mode"); rc = -1; }
		else if (m == PF_MODE_ERROR) { snprintf(err, errn, "cannot request Error mode"); rc = -1; }
		else { c.type = PF_CMD_MODE; c.mode = (pf_mode)m; c.num = pf_json_num(j, "setpoint", 0); }
	} else if (!strcmp(cmd, "setpoint")) {
		c.type = PF_CMD_SETPOINT; c.num = pf_json_num(j, "setpoint", 0);
		if (c.num <= 0) { snprintf(err, errn, "setpoint required"); rc = -1; }
	} else if (!strcmp(cmd, "stop")) {
		c.type = PF_CMD_STOP;
	} else if (!strcmp(cmd, "smoke_plus")) {
		c.type = PF_CMD_SMOKE_PLUS; c.flag = pf_json_bool(j, "enabled", false);
	} else if (!strcmp(cmd, "pwm_control")) {
		c.type = PF_CMD_PWM_CONTROL; c.flag = pf_json_bool(j, "enabled", false);
	} else if (!strcmp(cmd, "duty_cycle")) {
		c.type = PF_CMD_DUTY_CYCLE; c.num = pf_json_num(j, "duty_cycle", 100);
	} else if (!strcmp(cmd, "manual")) {
		c.type = PF_CMD_MANUAL_OUTPUT;
		pf_strlcpy(c.str, pf_json_str(j, "output", ""), sizeof c.str);
		c.flag = pf_json_bool(j, "on", false);
		c.num = pf_json_num(j, "pct", 100);
	} else if (!strcmp(cmd, "lid")) {
		c.type = PF_CMD_LID_TOGGLE;
	} else if (!strcmp(cmd, "prime")) {
		c.type = PF_CMD_PRIME; c.num = pf_json_num(j, "amount", 10);
		pf_strlcpy(c.str, pf_json_str(j, "next", ""), sizeof c.str);
	} else if (!strcmp(cmd, "clear_error")) {
		c.type = PF_CMD_CLEAR_ERROR;
	} else if (!strcmp(cmd, "target")) {
		c.type = PF_CMD_NOTIFY_TARGET;
		pf_strlcpy(c.str, pf_json_str(j, "label", ""), sizeof c.str);
		c.num = pf_json_num(j, "target", 0);
		c.aux = pf_json_int(j, "after", 0);
		if (!c.str[0]) { snprintf(err, errn, "label required"); rc = -1; }
	} else if (!strcmp(cmd, "limits")) {
		c.type = PF_CMD_NOTIFY_LIMITS;
		pf_strlcpy(c.str, pf_json_str(j, "label", ""), sizeof c.str);
		c.num = pf_json_num(j, "high", 0);
		c.num2 = pf_json_num(j, "low", 0);
	} else if (!strcmp(cmd, "timer")) {
		const char *op = pf_json_str(j, "op", "start");
		if (!strcmp(op, "start")) { c.type = PF_CMD_TIMER_START; c.num = pf_json_num(j, "seconds", 0); c.aux = pf_json_int(j, "after", 0); if (c.num <= 0) { snprintf(err, errn, "seconds required"); rc = -1; } }
		else if (!strcmp(op, "pause")) c.type = PF_CMD_TIMER_PAUSE;
		else if (!strcmp(op, "resume")) c.type = PF_CMD_TIMER_RESUME;
		else if (!strcmp(op, "cancel")) c.type = PF_CMD_TIMER_CANCEL;
		else { snprintf(err, errn, "unknown timer op"); rc = -1; }
	} else if (!strcmp(cmd, "test_notify")) {
		c.type = PF_CMD_NOTIFY_TEST;
	} else if (!strcmp(cmd, "autotune")) {
		c.type = pf_json_bool(j, "start", true) ? PF_CMD_AUTOTUNE_START : PF_CMD_AUTOTUNE_STOP;
	} else if (!strcmp(cmd, "apply_tuning")) {
		c.type = PF_CMD_TUNING_APPLY;
	} else if (!strcmp(cmd, "recipe")) {
		const char *op = pf_json_str(j, "op", "start");
		if (!strcmp(op, "start")) { c.type = PF_CMD_RECIPE_START; c.num = pf_json_num(j, "id", 0); if (c.num <= 0) { snprintf(err, errn, "id required"); rc = -1; } }
		else if (!strcmp(op, "next")) c.type = PF_CMD_RECIPE_NEXT;
		else if (!strcmp(op, "stop")) c.type = PF_CMD_RECIPE_STOP;
		else { snprintf(err, errn, "unknown recipe op"); rc = -1; }
	} else {
		snprintf(err, errn, "unknown command '%s'", cmd);
		rc = -1;
	}
	cJSON_Delete(j);
	if (rc == 0 && pf_cmdq_push(&c)) { snprintf(err, errn, "command queue full"); rc = -1; }
	return rc;
}

/* ---------------- settings ---------------- */

static void settings_get(const char *sub, pf_api_resp *r)
{
	cJSON *d = pf_set_dup(sub && *sub ? sub : NULL);
	if (!d) { reply_err(r, 404, "no such setting"); return; }
	reply(r, 200, d);
}

static void settings_patch(const char *sub, const char *body, pf_api_resp *r)
{
	char err[160] = "";
	if (pf_settings_patch(sub && *sub ? sub : NULL, body, err, sizeof err)) { reply_err(r, 400, err); return; }
	/* tell the control thread which subsystems to rebuild */
	if (!sub || !*sub || !strncmp(sub, "controller", 10)) pf_cmd_simple(PF_CMD_CONTROLLER_CHANGED);
	if (!sub || !*sub || !strncmp(sub, "probe_settings", 14)) pf_cmd_simple(PF_CMD_PROBES_CHANGED);
	pf_cmd_simple(PF_CMD_SETTINGS_CHANGED);
	reply_ok(r);
}

/* ---------------- dispatch ---------------- */

void pf_api_dispatch(const pf_api_req *req, pf_api_resp *resp)
{
	const char *p = req->path;
	const char *m = req->method;
	bool get = !strcmp(m, "GET"), post = !strcmp(m, "POST"), put = !strcmp(m, "PUT") || !strcmp(m, "PATCH");

	if (get && !strcmp(p, "/status")) {
		pf_status st;
		pf_status_get(&st);
		reply(resp, 200, pf_status_to_json(&st, pf_settings_units()));
		return;
	}
	if (!strncmp(p, "/settings", 9)) {
		const char *sub = p[9] == '/' ? p + 10 : "";
		char dotted[128];
		pf_strlcpy(dotted, sub, sizeof dotted);
		for (char *s = dotted; *s; s++) if (*s == '/') *s = '.';
		if (get) { settings_get(dotted, resp); return; }
		if (put || post) { settings_patch(dotted, req->body, resp); return; }
	}
	if (post && (!strcmp(p, "/cmd"))) {
		char err[128];
		if (pf_api_command_json(req->body, err, sizeof err)) reply_err(resp, 400, err); else reply_ok(resp);
		return;
	}
	/* convenience REST forms of the common commands */
	if (post && (!strcmp(p, "/mode") || !strcmp(p, "/setpoint") || !strcmp(p, "/smoke_plus") || !strcmp(p, "/pwm_control") ||
	             !strcmp(p, "/manual") || !strcmp(p, "/lid") || !strcmp(p, "/prime") || !strcmp(p, "/stop") || !strcmp(p, "/clear_error"))) {
		cJSON *j = req->body_len ? cJSON_Parse(req->body) : cJSON_CreateObject();
		if (!j) { reply_err(resp, 400, "invalid JSON"); return; }
		cJSON_AddStringToObject(j, "cmd", p + 1);
		char *txt = cJSON_PrintUnformatted(j);
		cJSON_Delete(j);
		char err[128];
		int rc = pf_api_command_json(txt, err, sizeof err);
		free(txt);
		if (rc) reply_err(resp, 400, err); else reply_ok(resp);
		return;
	}
	if (get && !strcmp(p, "/history")) {
		double now = pf_wall();
		double minutes = query_num(req->query, "minutes", 0);
		double from = query_num(req->query, "from", minutes > 0 ? now - minutes * 60 : now - 15 * 60);
		double to = query_num(req->query, "to", now);
		int res = (int)query_num(req->query, "res", 0);
		if (res <= 0) { double span = to - from; res = span > 6 * 3600 ? 60 : span > 3600 ? 15 : 3; }
		cJSON *h = pf_db_history_query(from, to, res);
		cJSON_AddStringToObject(h, "units", pf_settings_units() == PF_UNITS_C ? "C" : "F");
		/* convert temperatures to user units */
		pf_units u = pf_settings_units();
		if (u == PF_UNITS_F) {
			cJSON *it;
			cJSON_ArrayForEach(it, cJSON_GetObjectItem(h, "setpoint")) if (cJSON_IsNumber(it) && it->valuedouble > 0) cJSON_SetNumberValue(it, pf_c_to_f(it->valuedouble));
			cJSON *probes = cJSON_GetObjectItem(h, "probes"), *pr;
			cJSON_ArrayForEach(pr, probes) {
				cJSON_ArrayForEach(it, cJSON_GetObjectItem(pr, "temp")) if (cJSON_IsNumber(it)) cJSON_SetNumberValue(it, pf_c_to_f(it->valuedouble));
				cJSON_ArrayForEach(it, cJSON_GetObjectItem(pr, "target")) if (cJSON_IsNumber(it) && it->valuedouble > 0) cJSON_SetNumberValue(it, pf_c_to_f(it->valuedouble));
			}
		}
		reply(resp, 200, h);
		return;
	}
	if (get && !strcmp(p, "/events")) { reply(resp, 200, pf_db_events_recent((int)query_num(req->query, "limit", 100))); return; }
	if (get && !strcmp(p, "/alerts")) { reply(resp, 200, pf_events_recent_json((int)query_num(req->query, "limit", 20))); return; }
	if (get && !strcmp(p, "/logs")) {
		char *buf = malloc(200000);
		pf_log_recent_json(buf, 200000, (int)query_num(req->query, "limit", 300));
		resp->status = 200;
		resp->json = buf;
		return;
	}
	if (get && !strcmp(p, "/controllers")) {
		cJSON *arr = cJSON_CreateArray();
		for (int i = 0; i < pf_controller_count(); i++) {
			const pf_controller_ops *o = pf_controller_at(i);
			cJSON *c = cJSON_CreateObject();
			cJSON_AddStringToObject(c, "id", o->id);
			cJSON_AddStringToObject(c, "name", o->name ? o->name : o->id);
			cJSON_AddStringToObject(c, "description", o->description ? o->description : "");
			cJSON_AddStringToObject(c, "author", o->author ? o->author : "");
			cJSON *rec = cJSON_AddObjectToObject(c, "recommend");
			cJSON_AddNumberToObject(rec, "cycle_time", o->recommend.cycle_time);
			cJSON_AddNumberToObject(rec, "u_min", o->recommend.u_min);
			cJSON_AddNumberToObject(rec, "u_max", o->recommend.u_max);
			cJSON *schema = o->config_schema_json ? cJSON_Parse(o->config_schema_json) : NULL;
			cJSON_AddItemToObject(c, "config", schema ? schema : cJSON_CreateArray());
			cJSON_AddItemToArray(arr, c);
		}
		reply(resp, 200, arr);
		return;
	}
	if (get && !strcmp(p, "/manifest")) {
		const pf_embedded_file *f = pf_embedded_share("manifest.json");
		resp->status = 200;
		resp->json = f ? strndup((const char *)f->data, f->len) : strdup("{}");
		return;
	}
	if (get && !strcmp(p, "/probes/devices")) { reply(resp, 200, pf_probes_device_status()); return; }
	if (post && !strcmp(p, "/probes/ble/scan")) {
		if (!pf_ble_available()) { pf_ble_start(); pf_sleep_ms(1500); }
		if (!pf_ble_available()) { reply_err(resp, 503, "Bluetooth adapter not available"); return; }
		reply(resp, 200, pf_ble_scan_json((int)query_num(req->query, "seconds", 8)));
		return;
	}
	if (get && !strcmp(p, "/system")) { reply(resp, 200, pf_sysinfo_json()); return; }
	if (get && !strcmp(p, "/pellets")) { reply(resp, 200, pf_pellets_json()); return; }
	if (post && !strcmp(p, "/pellets/profile")) {
		char err[128] = "";
		if (pf_pellets_profile_save(req->body, err, sizeof err)) reply_err(resp, 400, err[0] ? err : "save failed"); else reply_ok(resp);
		return;
	}
	if (post && !strcmp(p, "/pellets/delete")) {
		cJSON *j = cJSON_Parse(req->body);
		int rc = pf_pellets_profile_delete(pf_json_int(j, "id", 0));
		cJSON_Delete(j);
		if (rc) reply_err(resp, 400, "cannot delete (in use?)"); else reply_ok(resp);
		return;
	}
	if (post && !strcmp(p, "/pellets/load")) {
		cJSON *j = cJSON_Parse(req->body);
		int id = pf_json_int(j, "id", 0);
		cJSON_Delete(j);
		if (id <= 0) { reply_err(resp, 400, "id required"); return; }
		pf_pellets_load(id);
		reply_ok(resp);
		return;
	}
	if (post && !strcmp(p, "/pellets/check")) { pf_pellets_request_check(); reply_ok(resp); return; }
	if (get && !strcmp(p, "/learning")) { reply(resp, 200, pf_learning_json()); return; }
	if (post && !strcmp(p, "/learning/reset")) { pf_learning_reset(); reply_ok(resp); return; }
	if (get && !strcmp(p, "/recipes")) { reply(resp, 200, pf_recipes_list()); return; }
	if (post && !strcmp(p, "/recipes")) {
		char err[128] = "";
		int id = pf_recipe_save(req->body, err, sizeof err);
		if (id < 0) { reply_err(resp, 400, err); return; }
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "result", "OK");
		cJSON_AddNumberToObject(o, "id", id);
		reply(resp, 200, o);
		return;
	}
	if (post && !strncmp(p, "/recipes/", 9) && strstr(p, "/delete")) {
		if (pf_recipe_delete(atoi(p + 9))) reply_err(resp, 400, "delete failed"); else reply_ok(resp);
		return;
	}
	if (get && !strcmp(p, "/cookfiles")) { reply(resp, 200, pf_cookfile_list()); return; }
	if (!strncmp(p, "/cookfiles/", 11)) {
		int id = atoi(p + 11);
		const char *sub = strchr(p + 11, '/');
		if (get) {
			char *txt = pf_cookfile_read(id);
			if (!txt) { reply_err(resp, 404, "no such cook file"); return; }
			resp->status = 200;
			resp->json = txt;
			return;
		}
		if (post && sub && !strcmp(sub, "/delete")) { if (pf_cookfile_delete(id)) reply_err(resp, 400, "delete failed"); else reply_ok(resp); return; }
		if (post && sub && !strcmp(sub, "/rename")) {
			cJSON *j = cJSON_Parse(req->body);
			int rc = pf_cookfile_rename(id, pf_json_str(j, "name", "Cook"));
			cJSON_Delete(j);
			if (rc) reply_err(resp, 400, "rename failed"); else reply_ok(resp);
			return;
		}
	}
	if (get && !strcmp(p, "/network/status")) { reply(resp, 200, pf_netmgr_status()); return; }
	if (get && !strcmp(p, "/network/scan")) { reply(resp, 200, pf_wifi_scan(query_num(req->query, "rescan", 1) != 0)); return; }
	if (get && !strcmp(p, "/network/saved")) { reply(resp, 200, pf_wifi_saved()); return; }
	if (post && !strcmp(p, "/network/connect")) {
		cJSON *j = cJSON_Parse(req->body);
		const char *ssid = pf_json_str(j, "ssid", "");
		if (!*ssid) { cJSON_Delete(j); reply_err(resp, 400, "ssid required"); return; }
		int rc = pf_netmgr_connect(ssid, pf_json_str(j, "psk", ""));
		cJSON_Delete(j);
		if (rc) reply_err(resp, 409, "a connection attempt is already in progress"); else reply_ok(resp);
		return;
	}
	if (post && !strcmp(p, "/network/forget")) {
		cJSON *j = cJSON_Parse(req->body);
		const char *ssid = pf_json_str(j, "ssid", "");
		int rc = *ssid ? pf_wifi_forget(ssid) : -1;
		cJSON_Delete(j);
		if (rc) reply_err(resp, 400, "could not remove network"); else reply_ok(resp);
		return;
	}
	if (post && !strcmp(p, "/network/hotspot")) {
		cJSON *j = cJSON_Parse(req->body);
		pf_netmgr_hotspot(pf_json_bool(j, "on", true));
		cJSON_Delete(j);
		reply_ok(resp);
		return;
	}
	if (post && !strcmp(p, "/history/clear")) { pf_db_history_clear(); reply_ok(resp); return; }
	if (post && (!strcmp(p, "/admin/reboot") || !strcmp(p, "/admin/poweroff"))) {
		pf_status st;
		pf_status_get(&st);
		if (st.mode != PF_MODE_STOP && st.mode != PF_MODE_MONITOR && st.mode != PF_MODE_ERROR) { reply_err(resp, 409, "stop the grill first"); return; }
		if (st.sim) { reply_err(resp, 400, "not available in simulator"); return; }
		pf_cmd_simple(PF_CMD_STOP);
		LOGW(TAG, "%s requested via API", p + 7);
		pf_db_event(PF_LVL_WARN, "ADMIN", p + 7);
		pf_sleep_ms(500);
		if (fork() == 0) { execlp("systemctl", "systemctl", strstr(p, "reboot") ? "reboot" : "poweroff", (char *)NULL); _exit(1); }
		reply_ok(resp);
		return;
	}
	reply_err(resp, 404, "unknown endpoint");
}
