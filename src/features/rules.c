#define _GNU_SOURCE
/* The conditional-notification engine.
 *
 * A rule selects some entities, describes a condition over their traits, and carries the message to
 * send when that condition becomes true. Everything is read straight out of the status JSON, so the
 * set of testable traits is whatever the status publishes.
 *
 * Firing is edge triggered on the whole condition set, after any hold time: a rule sends once when
 * what it describes becomes true and stays quiet until it becomes false again. A deadband, a
 * cooldown and an optional repeat sit on top, and by default a rule only runs while the grill is
 * cooking so a probe charging on the bench cannot page anyone. */
#include "features/rules.h"
#include "core/events.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "rules"
#define MAX_RULES 32
#define MAX_INST 12          /* instances one rule may track at once */
#define MAX_STATE 96

/* ------------------------------------------------------------------ values */

typedef enum { VT_NONE = 0, VT_NUM, VT_BOOL, VT_STR } vtype;
typedef struct { vtype t; double num; bool b; const char *str; } val;

static val v_none(void) { val v = { VT_NONE, 0, false, NULL }; return v; }
static val v_num(double n) { val v = { VT_NUM, n, false, NULL }; return v; }
static val v_bool(bool b) { val v = { VT_BOOL, b ? 1 : 0, b, NULL }; return v; }
static val v_str(const char *s) { val v = { VT_STR, 0, false, s }; return v; }

/* ------------------------------------------------------------------ entity lookup
 * An instance is one row of the status: a probe object, an output name, or the grill itself. */

typedef struct {
	const char *domain;      /* "probe", "output", "grill", ... */
	const char *name;        /* what a message calls it */
	const char *label;       /* stable key for per-instance state */
	const cJSON *obj;        /* the probe object, when the domain has one */
} inst;

static const cJSON *jget(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive((cJSON *)o, k); }

/* A trait of one instance, or of the grill when the condition names another entity. */
static val trait_of(const cJSON *status, const inst *in, const char *entity, const char *trait)
{
	const cJSON *o = in ? in->obj : NULL;
	const char *domain = in ? in->domain : "grill";
	if (entity && *entity && strcmp(entity, "this")) { domain = entity; o = NULL; }

	if (!strcmp(domain, "probe") && o) {
		const cJSON *t = jget(o, trait);
		if (!strcmp(trait, "temp") || !strcmp(trait, "target") || !strcmp(trait, "battery") ||
		    !strcmp(trait, "signal") || !strcmp(trait, "rssi") || !strcmp(trait, "eta") ||
		    !strcmp(trait, "limit_high") || !strcmp(trait, "limit_low") || !strcmp(trait, "ambient")) {
			if (!strcmp(trait, "eta")) t = jget(o, "eta_s");
			return cJSON_IsNumber(t) ? v_num(t->valuedouble) : v_none();
		}
		if (!strcmp(trait, "over")) {   /* how far past its target it is */
			const cJSON *tp = jget(o, "temp"), *tg = jget(o, "target");
			if (!cJSON_IsNumber(tp) || !cJSON_IsNumber(tg) || tg->valuedouble <= 0) return v_none();
			return v_num(tp->valuedouble - tg->valuedouble);
		}
		if (!strcmp(trait, "valid") || !strcmp(trait, "connected")) {
			/* a wireless probe that has gone quiet reads invalid, which is what "connected" means here */
			return v_bool(cJSON_IsTrue(jget(o, "valid")));
		}
		if (!strcmp(trait, "wireless")) return v_bool(cJSON_IsTrue(jget(o, "wireless")));
		if (!strcmp(trait, "enabled")) return v_bool(cJSON_IsTrue(jget(o, "enabled")));
		if (!strcmp(trait, "name")) return v_str(pf_json_str((cJSON *)o, "name", ""));
		if (!strcmp(trait, "role")) return v_str(pf_json_str((cJSON *)o, "role", ""));
		return v_none();
	}

	if (!strcmp(domain, "output")) {
		char key[48];
		snprintf(key, sizeof key, "outputs.%.24s", in && in->label ? in->label : "auger");
		if (!strcmp(trait, "state")) return v_bool(pf_json_bool((cJSON *)status, key, false));
		if (!strcmp(trait, "percent")) return v_num(pf_json_num((cJSON *)status, "outputs.fan_pct", 0));
		return v_none();
	}

	/* everything else lives at a fixed path in the status */
	static const struct { const char *domain, *trait, *path; } MAP[] = {
		{ "grill", "mode", "mode" }, { "grill", "temp", NULL }, { "grill", "over", NULL }, { "grill", "setpoint", "setpoint" },
		{ "grill", "error", "safety.error_code" }, { "grill", "cook_elapsed", "cook_elapsed" },
		{ "grill", "mode_remaining", "timers.mode_remaining" }, { "grill", "lid_open", "lid_open" },
		{ "grill", "hopper", "hopper_pct" },
		{ "hopper", "level", "hopper_pct" },
		{ "controller", "duty", "cycle.u_applied" }, { "controller", "feedforward", "cycle.u_ff" },
		{ "controller", "error", "controller.error" },
		{ "weather", "temp", "weather.temp" }, { "weather", "wind", "weather.wind_kmh" },
		{ "weather", "humidity", "weather.humidity" },
		{ "system", "wifi_signal", "net.signal" }, { "system", "tailscale_online", "net.tailscale.online" },
		{ "timer", "remaining", "timer.remaining" }, { "timer", "running", "timer.running" },
	};
	if (!strcmp(domain, "grill") && (!strcmp(trait, "temp") || !strcmp(trait, "over"))) {
		/* "over" is how far the pit sits from its set point: positive is hot, negative is cold.
		 * It is what "stabilised", "running hot" and "running cold" are all written against. */
		double sp = pf_json_num((cJSON *)status, "setpoint", 0);
		if (!strcmp(trait, "over") && sp <= 0) return v_none();
		const cJSON *p;
		cJSON_ArrayForEach(p, jget(status, "probes"))
			if (!strcmp(pf_json_str((cJSON *)p, "role", ""), "Primary")) {
				const cJSON *t = jget(p, "temp");
				if (!cJSON_IsNumber(t)) return v_none();
				return v_num(strcmp(trait, "over") ? t->valuedouble : t->valuedouble - sp);
			}
		return v_none();
	}
	for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++) {
		if (strcmp(MAP[i].domain, domain) || strcmp(MAP[i].trait, trait) || !MAP[i].path) continue;
		const cJSON *t = pf_json_path((cJSON *)status, MAP[i].path);
		if (cJSON_IsNumber(t)) return v_num(t->valuedouble);
		if (cJSON_IsBool(t)) return v_bool(cJSON_IsTrue(t));
		if (cJSON_IsString(t)) return v_str(t->valuestring);
		return v_none();
	}
	return v_none();
}

/* ------------------------------------------------------------------ selection
 * Which entities a rule watches: a domain, optional filters, and explicit include/exclude lists. */

static bool str_in_array(const cJSON *arr, const char *s)
{
	const cJSON *it;
	cJSON_ArrayForEach(it, arr) if (cJSON_IsString(it) && !strcasecmp(it->valuestring, s)) return true;
	return false;
}

static int select_instances(const cJSON *status, const cJSON *rule, inst *out, int max)
{
	const cJSON *sel = jget(rule, "select");
	const char *domain = pf_json_str((cJSON *)sel, "domain", "grill");
	int n = 0;

	if (!strcmp(domain, "probe")) {
		const char *role = pf_json_str((cJSON *)sel, "role", "any");
		const char *link = pf_json_str((cJSON *)sel, "link", "any");
		const cJSON *inc = jget(sel, "include"), *exc = jget(sel, "exclude");
		const cJSON *p;
		cJSON_ArrayForEach(p, jget(status, "probes")) {
			if (n >= max) break;
			if (!cJSON_IsTrue(jget(p, "enabled"))) continue;
			if (cJSON_IsTrue(jget(p, "companion"))) continue;   /* shown inside its sibling's card */
			const char *prole = pf_json_str((cJSON *)p, "role", "");
			if (strcmp(role, "any") && strcasecmp(role, prole)) continue;
			bool wireless = cJSON_IsTrue(jget(p, "wireless"));
			if (!strcmp(link, "bluetooth") && !wireless) continue;
			if (!strcmp(link, "wired") && wireless) continue;
			const char *label = pf_json_str((cJSON *)p, "label", "");
			const char *name = pf_json_str((cJSON *)p, "name", label);
			if (cJSON_IsArray(inc) && cJSON_GetArraySize((cJSON *)inc) > 0 &&
			    !str_in_array(inc, label) && !str_in_array(inc, name)) continue;
			if (str_in_array(exc, label) || str_in_array(exc, name)) continue;
			out[n].domain = "probe";
			out[n].name = name;
			out[n].label = label;
			out[n].obj = p;
			n++;
		}
		return n;
	}

	if (!strcmp(domain, "output")) {
		static const char *const OUTS[] = { "auger", "fan", "igniter", "power" };
		const cJSON *inc = jget(sel, "include");
		for (size_t i = 0; i < sizeof OUTS / sizeof OUTS[0] && n < max; i++) {
			if (cJSON_IsArray(inc) && cJSON_GetArraySize((cJSON *)inc) > 0 && !str_in_array(inc, OUTS[i])) continue;
			out[n].domain = "output";
			out[n].name = OUTS[i];
			out[n].label = OUTS[i];
			out[n].obj = NULL;
			n++;
		}
		return n;
	}

	if (n < max) {   /* single-instance domains */
		out[n].domain = domain;
		out[n].name = domain;
		out[n].label = domain;
		out[n].obj = NULL;
		n++;
	}
	return n;
}

/* ------------------------------------------------------------------ conditions */

/* the other side of a comparison: a literal, or another trait of the same instance */
static val resolve_operand(const cJSON *status, const inst *in, const cJSON *v)
{
	if (cJSON_IsNumber(v)) return v_num(v->valuedouble);
	if (cJSON_IsBool(v)) return v_bool(cJSON_IsTrue(v));
	if (cJSON_IsString(v)) return v_str(v->valuestring);
	if (cJSON_IsObject(v)) {
		const cJSON *t = jget(v, "trait");
		if (cJSON_IsString(t)) return trait_of(status, in, pf_json_str((cJSON *)v, "entity", "this"), t->valuestring);
	}
	return v_none();
}

static bool compare(const val *a, const char *op, const val *b, const val *b2)
{
	if (!strcmp(op, "unavailable")) return a->t == VT_NONE;
	if (!strcmp(op, "available")) return a->t != VT_NONE;
	if (a->t == VT_NONE) return false;

	if (a->t == VT_STR || b->t == VT_STR) {
		const char *as = a->t == VT_STR ? a->str : "", *bs = b->t == VT_STR ? b->str : "";
		if (!strcmp(op, "is") || !strcmp(op, "==")) return !strcasecmp(as, bs);
		if (!strcmp(op, "is_not") || !strcmp(op, "!=")) return strcasecmp(as, bs) != 0;
		if (!strcmp(op, "empty")) return as[0] == 0;
		if (!strcmp(op, "not_empty")) return as[0] != 0;
		if (!strcmp(op, "contains")) return strcasestr(as, bs) != NULL;
		return false;
	}
	if (a->t == VT_BOOL || b->t == VT_BOOL) {
		bool av = a->b, bv = b->t == VT_NONE ? true : b->b;
		if (!strcmp(op, "is_on") || !strcmp(op, "on")) return av;
		if (!strcmp(op, "is_off") || !strcmp(op, "off")) return !av;
		if (!strcmp(op, "is") || !strcmp(op, "==")) return av == bv;
		if (!strcmp(op, "is_not") || !strcmp(op, "!=")) return av != bv;
		return false;
	}
	double x = a->num, y = b->num;
	if (!strcmp(op, ">")) return x > y;
	if (!strcmp(op, ">=")) return x >= y;
	if (!strcmp(op, "<")) return x < y;
	if (!strcmp(op, "<=")) return x <= y;
	if (!strcmp(op, "==") || !strcmp(op, "is")) return fabs(x - y) < 1e-9;
	if (!strcmp(op, "!=") || !strcmp(op, "is_not")) return fabs(x - y) >= 1e-9;
	if (!strcmp(op, "between")) return b2->t != VT_NONE && x >= y && x <= b2->num;
	if (!strcmp(op, "within")) return b2->t != VT_NONE && fabs(x - y) <= b2->num;
	return false;
}

/* A condition node is either a comparison or a group of them joined by all/any. */
static bool eval_node(const cJSON *status, const inst *in, const cJSON *node, val *matched)
{
	const cJSON *kids = jget(node, "conditions");
	if (cJSON_IsArray(kids)) {
		bool any = !strcasecmp(pf_json_str((cJSON *)node, "op", "all"), "any");
		bool result = !any;   /* all: start true; any: start false */
		const cJSON *k;
		cJSON_ArrayForEach(k, kids) {
			bool r = eval_node(status, in, k, matched);
			if (any) result = result || r; else result = result && r;
		}
		if (cJSON_GetArraySize((cJSON *)kids) == 0) return true;
		return result;
	}
	const cJSON *tr = jget(node, "trait");
	if (!cJSON_IsString(tr)) return true;
	val a = trait_of(status, in, pf_json_str((cJSON *)node, "entity", "this"), tr->valuestring);
	val b = resolve_operand(status, in, jget(node, "value"));
	val b2 = resolve_operand(status, in, jget(node, "value2"));
	bool r = compare(&a, pf_json_str((cJSON *)node, "op", "=="), &b, &b2);
	if (r && matched && matched->t == VT_NONE) *matched = a;
	return r;
}

/* ------------------------------------------------------------------ message templates */

static void fmt_temp_token(char *out, size_t n, double v, const char *unit)
{
	snprintf(out, n, "%.0f\xC2\xB0%s", v, unit);
}

static void fmt_dur_token(char *out, size_t n, double secs)
{
	int m = (int)(secs / 60 + 0.5);
	if (m >= 60) snprintf(out, n, "%dh %dm", m / 60, m % 60);
	else snprintf(out, n, "%d min", m < 1 ? 1 : m);
}

/* Substitute {tokens}. Unknown tokens render as a dash so a half-written template still sends. */
static void render(char *out, size_t cap, const char *tpl, const cJSON *status, const inst *in, const val *matched)
{
	const char *units = pf_json_str((cJSON *)status, "units", "F");
	size_t o = 0;
	for (const char *p = tpl; *p && o + 1 < cap;) {
		if (*p == '{' && p[1] == '{') { out[o++] = '{'; p += 2; continue; }
		if (*p != '{') { out[o++] = *p++; continue; }
		const char *end = strchr(p, '}');
		if (!end) { out[o++] = *p++; continue; }
		char key[32];
		size_t klen = (size_t)(end - p - 1);
		if (klen >= sizeof key) klen = sizeof key - 1;
		memcpy(key, p + 1, klen);
		key[klen] = 0;
		p = end + 1;

		char buf[64] = "\xE2\x80\x94";   /* an em dash when we cannot resolve it */
		val v = v_none();
		if (!strcmp(key, "probe") || !strcmp(key, "probe_name") || !strcmp(key, "name"))
			snprintf(buf, sizeof buf, "%s", in && in->name ? in->name : "");
		else if (!strcmp(key, "grill")) { char g[48]; pf_set_str("globals.grill_name", g, sizeof g, "PiFire"); snprintf(buf, sizeof buf, "%s", g); }
		else if (!strcmp(key, "mode")) snprintf(buf, sizeof buf, "%s", pf_json_str((cJSON *)status, "mode", ""));
		else if (!strcmp(key, "time")) { char t[16]; time_t now = (time_t)pf_wall(); struct tm tmv; localtime_r(&now, &tmv); strftime(t, sizeof t, "%H:%M", &tmv); snprintf(buf, sizeof buf, "%s", t); }
		else if (!strcmp(key, "value") && matched && matched->t == VT_NUM) snprintf(buf, sizeof buf, "%.0f", matched->num);
		else {
			/* anything else is a trait: of the matched instance first, then the grill */
			if (!strcmp(key, "eta") || !strcmp(key, "eta_min")) v = trait_of(status, in, "this", "eta");
			else if (!strcmp(key, "cook_time")) v = trait_of(status, in, "grill", "cook_elapsed");
			else if (!strcmp(key, "grill_temp")) v = trait_of(status, in, "grill", "temp");
			else if (!strcmp(key, "setpoint")) v = trait_of(status, in, "grill", "setpoint");
			else if (!strcmp(key, "hopper")) v = trait_of(status, in, "hopper", "level");
			else if (!strcmp(key, "outdoor_temp")) v = trait_of(status, in, "weather", "temp");
			else v = trait_of(status, in, "this", key);
			if (v.t == VT_NUM) {
				if (!strcmp(key, "eta") || !strcmp(key, "cook_time")) fmt_dur_token(buf, sizeof buf, v.num);
				else if (!strcmp(key, "eta_min")) snprintf(buf, sizeof buf, "%.0f", v.num / 60);
				else if (!strcmp(key, "temp") || !strcmp(key, "target") || !strcmp(key, "over") ||
				         !strcmp(key, "grill_temp") || !strcmp(key, "setpoint") || !strcmp(key, "outdoor_temp") || !strcmp(key, "ambient"))
					fmt_temp_token(buf, sizeof buf, v.num, units);
				else if (!strcmp(key, "battery") || !strcmp(key, "hopper")) snprintf(buf, sizeof buf, "%.0f%%", v.num);
				else snprintf(buf, sizeof buf, "%.0f", v.num);
			} else if (v.t == VT_BOOL) snprintf(buf, sizeof buf, "%s", v.b ? "on" : "off");
			else if (v.t == VT_STR) snprintf(buf, sizeof buf, "%s", v.str);
		}
		for (const char *q = buf; *q && o + 1 < cap; q++) out[o++] = *q;
	}
	out[o < cap ? o : cap - 1] = 0;
}

/* ------------------------------------------------------------------ per-rule state */

typedef struct {
	bool used;
	char rule[40], inst[40];
	double held_since;    /* when the condition first became true, 0 = not true */
	double last_fired;
	bool armed;           /* false once fired, until the condition goes false again */
} rstate;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static rstate g_state[MAX_STATE];
static unsigned g_fired_total;

static rstate *state_for(const char *rule, const char *instance)
{
	rstate *free_slot = NULL;
	for (int i = 0; i < MAX_STATE; i++) {
		if (g_state[i].used && !strcmp(g_state[i].rule, rule) && !strcmp(g_state[i].inst, instance)) return &g_state[i];
		if (!g_state[i].used && !free_slot) free_slot = &g_state[i];
	}
	if (!free_slot) return NULL;
	memset(free_slot, 0, sizeof *free_slot);
	free_slot->used = true;
	free_slot->armed = true;
	pf_strlcpy(free_slot->rule, rule, sizeof free_slot->rule);
	pf_strlcpy(free_slot->inst, instance, sizeof free_slot->inst);
	return free_slot;
}

static unsigned sink_mask(const cJSON *rule)
{
	const cJSON *arr = jget(rule, "sinks");
	if (!cJSON_IsArray(arr) || cJSON_GetArraySize((cJSON *)arr) == 0) return PF_SINK_ALL;
	unsigned m = 0;
	const cJSON *it;
	cJSON_ArrayForEach(it, arr) {
		if (!cJSON_IsString(it)) continue;
		if (!strcmp(it->valuestring, "app")) m |= PF_SINK_APP;
		else if (!strcmp(it->valuestring, "pushover")) m |= PF_SINK_PUSHOVER;
		else if (!strcmp(it->valuestring, "ntfy")) m |= PF_SINK_NTFY;
		else if (!strcmp(it->valuestring, "mqtt")) m |= PF_SINK_MQTT;
		else if (!strcmp(it->valuestring, "webhook")) m |= PF_SINK_WEBHOOK;
	}
	return m ? m : PF_SINK_ALL;
}

static int crit_of(const cJSON *rule)
{
	const char *s = pf_json_str((cJSON *)rule, "level", "normal");
	if (!strcasecmp(s, "info")) return PF_CRIT_INFO;
	if (!strcasecmp(s, "high")) return PF_CRIT_HIGH;
	if (!strcasecmp(s, "critical")) return PF_CRIT_CRITICAL;
	return PF_CRIT_NORMAL;
}

static void fire(const cJSON *rule, const cJSON *status, const inst *in, const val *matched)
{
	char title[160], body[320];
	render(title, sizeof title, pf_json_str((cJSON *)rule, "title", "{grill}"), status, in, matched);
	render(body, sizeof body, pf_json_str((cJSON *)rule, "body", ""), status, in, matched);
	char code[40];
	snprintf(code, sizeof code, "RULE_%.32s", pf_json_str((cJSON *)rule, "id", "custom"));
	pf_events_emit_ex(code, crit_of(rule), sink_mask(rule), title, "%s", body);
	g_fired_total++;
}

/* ------------------------------------------------------------------ tick */

void pf_rules_tick(const cJSON *status, double now)
{
	if (!status) return;
	cJSON *rules = pf_set_dup("notify.rules");
	if (!cJSON_IsArray(rules)) { cJSON_Delete(rules); return; }

	const char *mode = pf_json_str((cJSON *)status, "mode", "Stop");
	bool cooking = !strcmp(mode, "Startup") || !strcmp(mode, "Reignite") || !strcmp(mode, "Smoke") ||
	               !strcmp(mode, "Hold") || !strcmp(mode, "Shutdown");

	pthread_mutex_lock(&g_mu);
	const cJSON *rule;
	cJSON_ArrayForEach(rule, rules) {
		if (!cJSON_IsTrue(jget(rule, "enabled"))) continue;
		const char *id = pf_json_str((cJSON *)rule, "id", "");
		if (!id[0]) continue;
		if (pf_json_bool((cJSON *)rule, "only_while_cooking", true) && !cooking) continue;

		inst instances[MAX_INST];
		int ni = select_instances(status, rule, instances, MAX_INST);
		bool every = !strcasecmp(pf_json_str((cJSON *)rule, "select.match", "any"), "every");
		double hold = pf_json_num((cJSON *)rule, "for_s", 0);
		double cooldown = pf_json_num((cJSON *)rule, "cooldown_s", 300);
		double repeat = pf_json_num((cJSON *)rule, "repeat_s", 0);
		const cJSON *when = jget(rule, "when");

		int matches = 0;
		for (int i = 0; i < ni; i++) {
			val matched = v_none();
			bool ok = when ? eval_node(status, &instances[i], when, &matched) : false;
			if (every) { matches += ok ? 1 : 0; continue; }

			rstate *st = state_for(id, instances[i].label);
			if (!st) continue;
			if (!ok) { st->held_since = 0; st->armed = true; continue; }
			if (st->held_since == 0) st->held_since = now;
			if (now - st->held_since < hold) continue;
			bool due = st->armed || (repeat > 0 && now - st->last_fired >= repeat);
			if (!due) continue;
			if (st->last_fired > 0 && now - st->last_fired < cooldown) continue;
			st->armed = false;
			st->last_fired = now;
			pthread_mutex_unlock(&g_mu);
			fire(rule, status, &instances[i], &matched);
			pthread_mutex_lock(&g_mu);
		}

		if (every && ni > 0) {
			rstate *st = state_for(id, "*");
			bool ok = matches == ni;
			if (st) {
				if (!ok) { st->held_since = 0; st->armed = true; }
				else {
					if (st->held_since == 0) st->held_since = now;
					bool due = st->armed || (repeat > 0 && now - st->last_fired >= repeat);
					if (now - st->held_since >= hold && due && !(st->last_fired > 0 && now - st->last_fired < cooldown)) {
						st->armed = false;
						st->last_fired = now;
						val none = v_none();
						pthread_mutex_unlock(&g_mu);
						fire(rule, status, ni > 0 ? &instances[0] : NULL, &none);
						pthread_mutex_lock(&g_mu);
					}
				}
			}
		}
	}
	pthread_mutex_unlock(&g_mu);
	cJSON_Delete(rules);
}

void pf_rules_preview(const cJSON *rule, const cJSON *status, char *title, size_t tn, char *body, size_t bn,
                      int *selected, int *matching)
{
	if (title && tn) title[0] = 0;
	if (body && bn) body[0] = 0;
	if (selected) *selected = 0;
	if (matching) *matching = 0;
	if (!rule || !status) return;
	inst instances[MAX_INST];
	int ni = select_instances(status, rule, instances, MAX_INST);
	if (selected) *selected = ni;
	const cJSON *when = jget(rule, "when");

	/* prefer an instance that actually matches, so the preview shows what would really be sent */
	int show = ni > 0 ? 0 : -1;
	val matched = v_none();
	for (int i = 0; i < ni; i++) {
		val m = v_none();
		if (when && eval_node(status, &instances[i], when, &m)) {
			if (matching) (*matching)++;
			if (show <= 0 || matched.t == VT_NONE) { show = i; matched = m; }
		}
	}
	const inst *in = show >= 0 ? &instances[show] : NULL;
	if (title && tn) render(title, tn, pf_json_str((cJSON *)rule, "title", ""), status, in, &matched);
	if (body && bn) render(body, bn, pf_json_str((cJSON *)rule, "body", ""), status, in, &matched);
}

int pf_rules_test(const cJSON *rule, const cJSON *status, char *err, size_t n)
{
	if (!rule || !status) { snprintf(err, n, "no rule"); return -1; }
	inst instances[MAX_INST];
	int ni = select_instances(status, rule, instances, MAX_INST);
	val none = v_none();
	fire(rule, status, ni > 0 ? &instances[0] : NULL, &none);
	return 0;
}

/* ------------------------------------------------------------------ catalogue and state */

cJSON *pf_rules_catalogue_json(const cJSON *status)
{
	/* type drives the editor: which operators to offer and how to render the value box */
	static const struct { const char *domain, *trait, *type, *unit; } TRAITS[] = {
		{ "probe", "temp", "temperature", "deg" }, { "probe", "target", "temperature", "deg" },
		{ "probe", "over", "temperature", "deg" }, { "probe", "eta", "duration", "s" },
		{ "probe", "battery", "percent", "%" }, { "probe", "signal", "number", "bars" },
		{ "probe", "rssi", "number", "dBm" }, { "probe", "connected", "bool", "" },
		{ "probe", "wireless", "bool", "" }, { "probe", "name", "string", "" },
		{ "grill", "mode", "enum", "" }, { "grill", "temp", "temperature", "deg" },
		{ "grill", "over", "temperature", "deg" },
		{ "grill", "setpoint", "temperature", "deg" }, { "grill", "error", "string", "" },
		{ "grill", "cook_elapsed", "duration", "s" }, { "grill", "mode_remaining", "duration", "s" },
		{ "grill", "lid_open", "bool", "" },
		{ "output", "state", "bool", "" }, { "output", "percent", "percent", "%" },
		{ "hopper", "level", "percent", "%" },
		{ "controller", "duty", "number", "" }, { "controller", "feedforward", "number", "" },
		{ "weather", "temp", "temperature", "deg" }, { "weather", "wind", "number", "km/h" },
		{ "weather", "humidity", "percent", "%" },
		{ "system", "wifi_signal", "percent", "%" }, { "system", "tailscale_online", "bool", "" },
		{ "timer", "remaining", "duration", "s" }, { "timer", "running", "bool", "" },
	};
	static const char *const NUM_OPS[] = { ">", ">=", "<", "<=", "==", "!=", "between", "within", NULL };
	static const char *const BOOL_OPS[] = { "is_on", "is_off", NULL };
	static const char *const STR_OPS[] = { "is", "is_not", "contains", "empty", "not_empty", NULL };

	cJSON *o = cJSON_CreateObject();
	cJSON *domains = cJSON_AddArrayToObject(o, "domains");
	static const char *const DOMS[] = { "probe", "grill", "output", "hopper", "controller", "weather", "system", "timer" };
	for (size_t d = 0; d < sizeof DOMS / sizeof DOMS[0]; d++) {
		cJSON *dj = cJSON_CreateObject();
		cJSON_AddStringToObject(dj, "id", DOMS[d]);
		cJSON_AddBoolToObject(dj, "multi", !strcmp(DOMS[d], "probe") || !strcmp(DOMS[d], "output"));
		cJSON *tj = cJSON_AddArrayToObject(dj, "traits");
		for (size_t i = 0; i < sizeof TRAITS / sizeof TRAITS[0]; i++) {
			if (strcmp(TRAITS[i].domain, DOMS[d])) continue;
			cJSON *t = cJSON_CreateObject();
			cJSON_AddStringToObject(t, "id", TRAITS[i].trait);
			cJSON_AddStringToObject(t, "type", TRAITS[i].type);
			cJSON_AddStringToObject(t, "unit", TRAITS[i].unit);
			const char *const *ops = !strcmp(TRAITS[i].type, "bool") ? BOOL_OPS
			                       : (!strcmp(TRAITS[i].type, "string") || !strcmp(TRAITS[i].type, "enum")) ? STR_OPS : NUM_OPS;
			cJSON *oj = cJSON_AddArrayToObject(t, "operators");
			for (int k = 0; ops[k]; k++) cJSON_AddItemToArray(oj, cJSON_CreateString(ops[k]));
			cJSON_AddItemToArray(tj, t);
		}
		/* the instances this domain currently has, so the editor can offer them by name */
		cJSON *ij = cJSON_AddArrayToObject(dj, "instances");
		if (status && !strcmp(DOMS[d], "probe")) {
			const cJSON *p;
			cJSON_ArrayForEach(p, jget(status, "probes")) {
				if (cJSON_IsTrue(jget(p, "companion"))) continue;
				cJSON *e = cJSON_CreateObject();
				cJSON_AddStringToObject(e, "label", pf_json_str((cJSON *)p, "label", ""));
				cJSON_AddStringToObject(e, "name", pf_json_str((cJSON *)p, "name", ""));
				cJSON_AddStringToObject(e, "role", pf_json_str((cJSON *)p, "role", ""));
				cJSON_AddBoolToObject(e, "wireless", cJSON_IsTrue(jget(p, "wireless")));
				cJSON_AddItemToArray(ij, e);
			}
		} else if (!strcmp(DOMS[d], "output")) {
			static const char *const OUTS[] = { "auger", "fan", "igniter", "power" };
			for (size_t i = 0; i < sizeof OUTS / sizeof OUTS[0]; i++) {
				cJSON *e = cJSON_CreateObject();
				cJSON_AddStringToObject(e, "label", OUTS[i]);
				cJSON_AddStringToObject(e, "name", OUTS[i]);
				cJSON_AddItemToArray(ij, e);
			}
		}
		cJSON_AddItemToArray(domains, dj);
	}

	static const char *const TOKENS[] = { "probe", "temp", "target", "over", "eta", "eta_min", "battery",
		"signal", "grill", "grill_temp", "setpoint", "mode", "hopper", "outdoor_temp", "cook_time", "value", "time", NULL };
	cJSON *tk = cJSON_AddArrayToObject(o, "tokens");
	for (int i = 0; TOKENS[i]; i++) cJSON_AddItemToArray(tk, cJSON_CreateString(TOKENS[i]));
	cJSON *lv = cJSON_AddArrayToObject(o, "levels");
	for (const char *const *l = (const char *const[]){ "info", "normal", "high", "critical", NULL }; *l; l++)
		cJSON_AddItemToArray(lv, cJSON_CreateString(*l));
	return o;
}

cJSON *pf_rules_state_json(void)
{
	cJSON *arr = cJSON_CreateArray();
	pthread_mutex_lock(&g_mu);
	for (int i = 0; i < MAX_STATE; i++) {
		if (!g_state[i].used) continue;
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "rule", g_state[i].rule);
		cJSON_AddStringToObject(o, "instance", g_state[i].inst);
		cJSON_AddBoolToObject(o, "holding", g_state[i].held_since > 0);
		cJSON_AddBoolToObject(o, "armed", g_state[i].armed);
		if (g_state[i].last_fired > 0) cJSON_AddNumberToObject(o, "last_fired_ago", round(pf_now() - g_state[i].last_fired));
		cJSON_AddItemToArray(arr, o);
	}
	pthread_mutex_unlock(&g_mu);
	return arr;
}

void pf_rules_init(void)
{
	pthread_mutex_lock(&g_mu);
	memset(g_state, 0, sizeof g_state);
	pthread_mutex_unlock(&g_mu);
	cJSON *rules = pf_set_dup("notify.rules");
	LOGI(TAG, "%d notification rule(s)", cJSON_IsArray(rules) ? cJSON_GetArraySize(rules) : 0);
	cJSON_Delete(rules);
}

void pf_rules_shutdown(void) {}
