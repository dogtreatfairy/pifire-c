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
#include "features/cooklog.h"
#include "features/learning.h"
#include "features/pellets.h"
#include "features/recipe.h"
#include "features/update.h"
#include "net/netmgr.h"
#include "net/sysinfo.h"
#include "net/tailscale.h"
#include "features/push.h"
#include "features/rules.h"
#include "features/alarms.h"
#include "features/webpush.h"
#include "features/tuner.h"
#include "features/weather.h"
#include "net/wifi.h"
#include "probes/ble/bluez.h"
#include "probes/probes.h"
#include "probes/shh.h"
#include <cJSON.h>
#include <math.h>
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
	if (n == 0) return false;
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
		else { c.type = PF_CMD_MODE; c.mode = (pf_mode)m; c.num = pf_json_num(j, "setpoint", 0); c.flag = pf_json_bool(j, "force", false); /* force: leave Startup/Reignite now */ }
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
	} else if (!strcmp(cmd, "probes_in_use")) {
		/* which probes are in the food, as a list of labels; none is a valid answer */
		c.type = PF_CMD_PROBES_IN_USE;
		c.str[0] = 0;
		cJSON *it;
		cJSON_ArrayForEach(it, cJSON_GetObjectItem(j, "labels")) {
			if (!cJSON_IsString(it)) continue;
			size_t len = strlen(c.str);
			snprintf(c.str + len, sizeof c.str - len, "%s%s", len ? "," : "", it->valuestring);
		}
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
	/* The alarm table: one shared answer to what is wrong and what has not been looked at, so
	 * clearing something on a phone clears it on the laptop too. */
	if (get && !strcmp(p, "/alarms")) { reply(resp, 200, pf_alarms_json()); return; }
	/* Web push: the browser needs this grill's public key to subscribe, and hands the subscription
	 * back so the daemon can reach it with the app closed. */
	if (get && !strcmp(p, "/push")) { reply(resp, 200, pf_webpush_json()); return; }
	if (post && !strcmp(p, "/push/subscribe")) {
		cJSON *b = req->body ? cJSON_Parse(req->body) : NULL;
		if (!b) { reply_err(resp, 400, "expected a push subscription"); return; }
		char err[160];
		int rc = pf_webpush_subscribe(b, err, sizeof err);
		cJSON_Delete(b);
		if (rc) { reply_err(resp, 400, err); return; }
		reply(resp, 200, pf_webpush_json());
		return;
	}
	if (post && !strcmp(p, "/push/unsubscribe")) {
		cJSON *b = req->body ? cJSON_Parse(req->body) : NULL;
		char ep[512];
		pf_strlcpy(ep, pf_json_str(b, "endpoint", ""), sizeof ep);
		cJSON_Delete(b);
		if (!ep[0]) { reply_err(resp, 400, "which subscription?"); return; }
		pf_webpush_unsubscribe(ep);
		reply(resp, 200, pf_webpush_json());
		return;
	}
	/* A backup of what the grill has learned about itself: hours of its own time and a hopper of
	 * pellets, living on an SD card. */
	if (get && !strcmp(p, "/tune/export")) { reply(resp, 200, pf_learning_export()); return; }
	if (post && !strcmp(p, "/tune/import")) {
		cJSON *body = req->body ? cJSON_Parse(req->body) : NULL;
		if (!body) { reply_err(resp, 400, "expected a tuning backup"); return; }
		char err[160];
		int k = pf_learning_import(body, err, sizeof err);
		cJSON_Delete(body);
		if (k < 0) { reply_err(resp, 400, err); return; }
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "restored", k);
		reply(resp, 200, o);
		return;
	}
	if (post && !strcmp(p, "/alarms/ack")) {
		cJSON *b = req->body ? cJSON_Parse(req->body) : NULL;
		const char *key = pf_json_str(b, "key", "");
		int n = (!key[0] || pf_json_bool(b, "all", false)) ? pf_alarms_ack_all() : (pf_alarms_ack(key) == 0 ? 1 : 0);
		cJSON_Delete(b);
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "acknowledged", n);
		reply(resp, 200, o);
		return;
	}
	if (post && !strcmp(p, "/alarms/shelve")) {
		cJSON *b = req->body ? cJSON_Parse(req->body) : NULL;
		char key[96];
		pf_strlcpy(key, pf_json_str(b, "key", ""), sizeof key);
		double secs = pf_json_num(b, "seconds", 1800);
		cJSON_Delete(b);
		if (!key[0]) { reply_err(resp, 400, "which alarm?"); return; }
		if (pf_alarms_shelve(key, secs)) { reply_err(resp, 404, "no such alarm"); return; }
		reply(resp, 200, cJSON_CreateObject());
		return;
	}
	if (get && !strcmp(p, "/logs")) {
		size_t n = 200000;
		char *buf = malloc(n);
		if (!buf) { reply(resp, 503, cJSON_CreateString("out of memory")); return; }
		pf_log_recent_json(buf, n, (int)query_num(req->query, "limit", 300));
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
	if (post && !strcmp(p, "/probes/tune")) {
		/* body: {"points":[{"temp":T,"ohms":R} x3]} in user units -> Steinhart-Hart A/B/C for a new profile */
		cJSON *b = cJSON_Parse(req->body ? req->body : "{}");
		cJSON *pts = cJSON_GetObjectItem(b, "points");
		double t[3], r[3];
		int n = 0;
		cJSON *it;
		pf_units u = pf_settings_units();
		cJSON_ArrayForEach(it, pts) { if (n < 3) { t[n] = pf_to_c(pf_json_num(it, "temp", NAN), u); r[n] = pf_json_num(it, "ohms", NAN); n++; } }
		cJSON_Delete(b);
		pf_shh shh;
		if (n != 3 || isnan(t[0]) || isnan(t[1]) || isnan(t[2]) || pf_shh_solve(t[0], r[0], t[1], r[1], t[2], r[2], &shh)) { reply_err(resp, 400, "need three distinct (temp, ohms) points"); return; }
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "A", shh.A);
		cJSON_AddNumberToObject(o, "B", shh.B);
		cJSON_AddNumberToObject(o, "C", shh.C);
		/* sanity: the fit must reproduce its own points */
		cJSON *chk = cJSON_AddArrayToObject(o, "check");
		for (int i = 0; i < 3; i++) cJSON_AddItemToArray(chk, cJSON_CreateNumber(round(pf_from_c(pf_shh_ohms_to_c(r[i], &shh), u) * 10) / 10));
		reply(resp, 200, o);
		return;
	}
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
	/* Calibrate one end of the hopper scale from what the sensor can see right now. */
	if (post && !strcmp(p, "/pellets/calibrate")) {
		cJSON *j = cJSON_Parse(req->body);
		const char *as = pf_json_str(j, "as", "");
		bool full = !strcmp(as, "full");
		bool ok = full || !strcmp(as, "empty");
		cJSON_Delete(j);
		if (!ok) { reply_err(resp, 400, "as must be \"full\" or \"empty\""); return; }
		pf_pellets_calibrate(full);
		reply_ok(resp);
		return;
	}
	if (get && !strcmp(p, "/learning")) { reply(resp, 200, pf_learning_json()); return; }
	/* Both go through the control thread: it owns the controller instance, which holds its own copy
	 * of what it has learned and been told, and clearing the stored copy alone would leave that
	 * running. Two different things can be wrong, so they are two different requests: forget what
	 * the grill worked out for itself, or throw away what was measured and go back to the numbers
	 * that were typed. */
	if (post && (!strcmp(p, "/learning/forget") || !strcmp(p, "/tune/clear"))) {
		pf_cmd c = { .type = PF_CMD_FORGET_LEARNING,
		             .aux = p[1] == 'l' ? PF_CLEAR_LEARNING : PF_CLEAR_TUNING };
		pf_strlcpy(c.str, "asked for", sizeof c.str);
		pf_cmdq_push(&c);
		reply_ok(resp);
		return;
	}
	if (get && !strcmp(p, "/recipes")) { reply(resp, 200, pf_recipes_list()); return; }
	if (post && !strcmp(p, "/recipes")) {
		char err[128] = "";
		int id = pf_recipe_save(req->body, err, sizeof err);
		if (id < 0) { reply_err(resp, 400, err); return; }
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "result", "OK");
		cJSON_AddNumberToObject(o, "id", id);
		/* Saved, and here is what is odd about it. The recipe is stored either way -- these are
		 * things to know, not reasons to refuse -- so a caller that is not the editor still hears
		 * that its recipe will not light the grill or will not put it out. */
		cJSON *body = cJSON_Parse(req->body);
		cJSON *warn = pf_recipe_shape_warnings(body ? cJSON_GetObjectItem(body, "steps") : NULL);
		if (cJSON_GetArraySize(warn)) cJSON_AddItemToObject(o, "warnings", warn); else cJSON_Delete(warn);
		cJSON_Delete(body);
		reply(resp, 200, o);
		return;
	}
	if (post && !strncmp(p, "/recipes/", 9) && strstr(p, "/delete")) {
		if (pf_recipe_delete(atoi(p + 9))) reply_err(resp, 400, "delete failed"); else reply_ok(resp);
		return;
	}
	if (get && !strcmp(p, "/cooklog")) {
		double from = 0, to = 0; char name[64];
		const char *qf = strstr(req->query ? req->query : "", "from="), *qt = strstr(req->query ? req->query : "", "to=");
		if (qf && qt) { from = atof(qf + 5); to = atof(qt + 3); snprintf(name, sizeof name, "range"); }
		else pf_cooklog_default_window(&from, &to, name, sizeof name);
		reply(resp, 200, pf_cooklog_json(from, to, name));
		return;
	}
	if (get && !strcmp(p, "/cookfiles")) { reply(resp, 200, pf_cookfile_list()); return; }
	if (!strncmp(p, "/cookfiles/", 11)) {
		int id = atoi(p + 11);
		const char *sub = strchr(p + 11, '/');
		if (get && sub && !strcmp(sub, "/log")) {
			double from, to; char name[64];
			if (pf_cooklog_cook_window(id, &from, &to, name, sizeof name)) { reply_err(resp, 404, "no such cook"); return; }
			reply(resp, 200, pf_cooklog_json(from, to, name));
			return;
		}
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
	if (get && !strcmp(p, "/network/tailscale")) { reply(resp, 200, pf_tailscale_status_json()); return; }
	if (post && !strncmp(p, "/network/tailscale/", 19)) {
		char err[160];
		if (pf_tailscale_action(p + 19, err, sizeof err)) { reply_err(resp, 409, err); return; }
		reply_ok(resp);
		return;
	}
	if (post && !strncmp(p, "/notify/test/", 13)) {
		char err[160];
		if (pf_push_test(p + 13, err, sizeof err)) { reply_err(resp, 502, err); return; }
		reply_ok(resp);
		return;
	}
	if (get && !strcmp(p, "/tune")) { reply(resp, 200, pf_tuner_json()); return; }
	if (post && !strcmp(p, "/tune/start")) {
		pf_status st;
		pf_status_get(&st);
		if (st.mode != PF_MODE_STOP && st.mode != PF_MODE_MONITOR) { reply_err(resp, 409, "stop the grill first"); return; }
		cJSON *body = req->body ? cJSON_Parse(req->body) : NULL;
		char err[160];
		/* A full profile is the default: it is what "tune my grill" means. A caller that passes
		 * its own set points is tuning one temperature and adding it to the library. */
		const cJSON *pts = body ? cJSON_GetObjectItem(body, "setpoints") : NULL;
		bool full = body && cJSON_IsBool(cJSON_GetObjectItem(body, "full_profile"))
		            ? cJSON_IsTrue(cJSON_GetObjectItem(body, "full_profile")) : !cJSON_IsArray(pts);
		int rc = pf_tuner_start(pts, full, pf_json_bool(body, "from_scratch", false), err, sizeof err);
		cJSON_Delete(body);
		if (rc) { reply_err(resp, 409, err); return; }
		reply_ok(resp);
		return;
	}
	if (post && !strcmp(p, "/tune/stop")) { pf_tuner_stop("Stopped from the app."); reply_ok(resp); return; }
	if (get && !strcmp(p, "/rules/entities")) {
		pf_status st;
		pf_status_get(&st);
		cJSON *j = pf_status_to_json(&st, pf_settings_units());
		cJSON *cat = pf_rules_catalogue_json(j);
		cJSON_Delete(j);
		reply(resp, 200, cat);
		return;
	}
	if (get && !strcmp(p, "/rules")) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddItemToObject(o, "rules", pf_set_dup("notify.rules"));
		cJSON_AddItemToObject(o, "state", pf_rules_state_json());
		reply(resp, 200, o);
		return;
	}
	if (post && !strcmp(p, "/rules/preview")) {
		cJSON *body = req->body ? cJSON_Parse(req->body) : NULL;
		if (!body) { reply_err(resp, 400, "expected a rule"); return; }
		pf_status st;
		pf_status_get(&st);
		cJSON *j = pf_status_to_json(&st, pf_settings_units());
		char title[200], text[400];
		int sel = 0, match = 0;
		pf_rules_preview(body, j, title, sizeof title, text, sizeof text, &sel, &match);
		cJSON_Delete(j);
		cJSON_Delete(body);
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "title", title);
		cJSON_AddStringToObject(o, "body", text);
		cJSON_AddNumberToObject(o, "selected", sel);
		cJSON_AddNumberToObject(o, "matching", match);
		reply(resp, 200, o);
		return;
	}
	if (post && !strcmp(p, "/rules/test")) {
		cJSON *body = req->body ? cJSON_Parse(req->body) : NULL;
		if (!body) { reply_err(resp, 400, "expected a rule"); return; }
		pf_status st;
		pf_status_get(&st);
		cJSON *j = pf_status_to_json(&st, pf_settings_units());
		char err[160];
		int rc = pf_rules_test(body, j, err, sizeof err);
		cJSON_Delete(j);
		cJSON_Delete(body);
		if (rc) { reply_err(resp, 400, err); return; }
		reply_ok(resp);
		return;
	}
	if (get && !strcmp(p, "/weather")) { reply(resp, 200, pf_weather_json()); return; }
	if (post && !strcmp(p, "/weather/refresh")) { pf_weather_refresh(); reply_ok(resp); return; }
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
		pf_system_power(strstr(p, "reboot") != NULL);
		reply_ok(resp);
		return;
	}
	if (get && !strcmp(p, "/update")) { reply(resp, 200, pf_update_status_json()); return; }
	if (post && !strcmp(p, "/update/check")) { if (pf_update_check()) { reply_err(resp, 409, "an update operation is already running"); return; } reply_ok(resp); return; }
	if (post && !strcmp(p, "/update/install")) {
		char err[160];
		if (pf_update_install(err, sizeof err)) { reply_err(resp, 409, err); return; }
		LOGW(TAG, "update install requested via API");
		reply_ok(resp);
		return;
	}
	if (post && !strcmp(p, "/admin/boardcfg")) {
		/* Apply the selected board's boot configuration: relay pulls that keep outputs OFF without the daemon,
		 * the hardware-PWM overlay for a DC fan, 1-Wire, I2C and SPI. Needs the sudoers rule from install.sh. */
		pf_status st;
		pf_status_get(&st);
		if (st.sim) { reply_err(resp, 400, "not available in simulator"); return; }
		char pins[64] = "", pwm[16] = "", w1[16] = "", level[8];
		int n = 0;
		const char *keys[] = { "platform.outputs.auger", "platform.outputs.igniter", "platform.outputs.power", "platform.outputs.fan", "platform.outputs.dc_fan" };
		for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
			int pin = pf_set_int(keys[i], -1);
			if (pin < 0 || pin > 63) continue;   /* a GPIO number, not whatever the settings file says */
			/* snprintf returns what it *would* have written, so accumulating it blindly walks the
			 * offset past the end of the buffer and turns the remaining size into a huge unsigned
			 * value. Stop at the point where it no longer fits. */
			int k = snprintf(pins + n, sizeof pins - (size_t)n, "%s%d", n ? "," : "", pin);
			if (k < 0 || (size_t)k >= sizeof pins - (size_t)n) break;
			n += k;
		}
		pf_set_str("platform.triggerlevel", level, sizeof level, "LOW");
		int pwm_pin = pf_set_bool("platform.dc_fan", false) ? pf_set_int("platform.outputs.pwm", -1) : -1;
		if (pwm_pin >= 0) snprintf(pwm, sizeof pwm, "%d", pwm_pin % 1000);
		int w1_pin = pf_set_int("platform.system.1WIRE", -1);
		if (w1_pin >= 0) snprintf(w1, sizeof w1, "%d", w1_pin % 1000);
		const char *argv[16];
		int a = 0;
		argv[a++] = "sudo"; argv[a++] = "-n"; argv[a++] = "/usr/local/bin/pifire-boardcfg"; argv[a++] = "--i2c"; argv[a++] = "--spi"; argv[a++] = "--watchdog";
		if (n) { argv[a++] = "--pulls"; argv[a++] = pins; argv[a++] = level; }
		if (pwm[0]) { argv[a++] = "--pwm"; argv[a++] = pwm; }
		if (w1[0]) { argv[a++] = "--onewire"; argv[a++] = w1; } else argv[a++] = "--no-onewire";
		argv[a] = NULL;
		char out[512];
		int rc = pf_run_capture(argv, out, sizeof out, 20);
		LOGI(TAG, "boardcfg rc=%d: %.200s", rc, out);
		if (rc != 0) { reply_err(resp, 500, out[0] ? out : "pifire-boardcfg failed"); return; }
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "reboot", strstr(out, "REBOOT") != NULL);
		cJSON_AddStringToObject(o, "output", out);
		reply(resp, 200, o);
		return;
	}
	reply_err(resp, 404, "unknown endpoint");
}
