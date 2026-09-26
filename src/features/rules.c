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
#include "features/alarms.h"
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
		/* A target of zero is not a target of zero degrees, it is no target: the pit probe never
		 * has one, and food probes only get one when somebody sets it. Reporting it as a number
		 * makes "reached its target" true for every probe nobody is watching. "over" has always
		 * treated it this way; now the trait itself agrees. */
		if (!strcmp(trait, "target")) {
			const cJSON *tg = jget(o, "target");
			return (cJSON_IsNumber(tg) && tg->valuedouble > 0) ? v_num(tg->valuedouble) : v_none();
		}
		if (!strcmp(trait, "next_step")) {
			const cJSON *ns = jget(o, "next_step");
			return cJSON_IsString(ns) ? v_str(ns->valuestring) : v_none();
		}
		if (!strcmp(trait, "eta_step")) {
			const cJSON *e = jget(o, "eta_step_s");
			return cJSON_IsNumber(e) && e->valuedouble >= 0 ? v_num(e->valuedouble) : v_none();
		}
		if (!strcmp(trait, "temp") || !strcmp(trait, "battery") ||
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
		/* Is this probe part of what is being cooked? A grill can have nine configured and two in
		 * the meat; the rest are switched on in a drawer and there is nothing to say about them. */
		if (!strcmp(trait, "in_use")) return v_bool(cJSON_IsTrue(jget(o, "in_use")));
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
		/* seconds since the grill was last given a new mode or set point: a pit short of its
		 * target is ordinary while it climbs, and only worth reporting once it has had time */
		{ "grill", "aiming_s", "aiming_s" },
		{ "hopper", "level", "hopper_pct" },
		{ "controller", "duty", "cycle.u_applied" }, { "controller", "feedforward", "cycle.u_ff" },
		{ "controller", "error", "controller.error" },
		{ "weather", "temp", "weather.temp" }, { "weather", "wind", "weather.wind_kmh" },
		{ "weather", "humidity", "weather.humidity" },
		{ "system", "wifi_signal", "net.signal" }, { "system", "tailscale_online", "net.tailscale.online" },
		{ "timer", "remaining", "timer.remaining" }, { "timer", "running", "timer.running" },
		/* A recipe step, asked about while it is running. These only exist inside the facts a step
		 * is evaluated against, which is why they are not in the status the UI receives. */
		{ "step", "elapsed", "step.elapsed" },
		{ "step", "food_max", "step.food_max" }, { "step", "food_min", "step.food_min" },
		{ "step", "food_rested", "step.food_rested" },
		{ "step", "prompt", "step.prompt" }, { "step", "lid", "step.lid" },
	};
	if (!strcmp(domain, "grill") && (!strcmp(trait, "temp") || !strcmp(trait, "over"))) {
		/* "over" is how far the pit sits from its set point: positive is hot, negative is cold.
		 * It is what "stabilised", "running hot" and "running cold" are all written against.
		 *
		 * It has no meaning while a tuning measurement is running. The relay is deliberately
		 * driving the pit either side of the target to see how the grill answers, so a rule about
		 * the grill running hot would be reporting the tuner's own doing, several times per set
		 * point. The pit temperature itself is still a fact and still testable; only the distance
		 * from a target the grill is not currently trying to hold goes away. */
		double sp = pf_json_num((cJSON *)status, "setpoint", 0);
		if (!strcmp(trait, "over") && (sp <= 0 || pf_json_bool((cJSON *)status, "autotune.active", false))) return v_none();
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
		if (cJSON_IsString(t)) {
			val a = trait_of(status, in, pf_json_str((cJSON *)v, "entity", "this"), t->valuestring);
			/* A reading can be compared against with an offset on it: "below the set point plus 15",
			 * "above its target minus 5". Without one, a band around a moving number has to be
			 * written as a fixed pair that stops meaning anything the moment the set point changes,
			 * which is the whole reason for comparing against a reading in the first place. */
			const cJSON *off = jget(v, "offset");
			if (a.t == VT_NUM && cJSON_IsNumber(off)) a.num += off->valuedouble;
			return a;
		}
	}
	return v_none();
}

/* `db` is the deadband: how far the reading must come back before the condition is allowed to go
 * false again, applied only while the rule is already reporting. Without it a reading that sits on
 * its threshold re-arms and re-fires every time it wobbles across -- a hopper sensor reading 18,
 * then 21, then 13, then 21 announces itself four times while the hopper simply gets emptier. */
static bool compare(const val *a, const char *op, const val *b, const val *b2, double db)
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
	/* Nothing is not zero. A reading the grill cannot give -- a trait this entity does not have,
	 * or one that is not set -- used to arrive here as a plain 0 and make "temperature is at or
	 * above its target" true the moment the probe read anything at all. There is no number to
	 * compare against, so the comparison is simply not satisfied. */
	if (b->t == VT_NONE) return false;
	double x = a->num, y = b->num;
	/* The threshold is widened in whichever direction keeps the condition true, so an alarm clears
	 * only once the reading has genuinely recovered rather than the moment it grazes back. */
	if (!strcmp(op, ">")) return x > y - db;
	if (!strcmp(op, ">=")) return x >= y - db;
	if (!strcmp(op, "<")) return x < y + db;
	if (!strcmp(op, "<=")) return x <= y + db;
	if (!strcmp(op, "==") || !strcmp(op, "is")) return fabs(x - y) < (db > 0 ? db : 1e-9);
	if (!strcmp(op, "!=") || !strcmp(op, "is_not")) return fabs(x - y) >= 1e-9;
	if (!strcmp(op, "between")) return b2->t != VT_NONE && x >= y - db && x <= b2->num + db;
	if (!strcmp(op, "within")) return b2->t != VT_NONE && fabs(x - y) <= b2->num + db;
	return false;
}

/* A condition can carry a time of its own -- "above 200 for five minutes", "in Hold for half an
 * hour" -- so each node that asks for one needs somewhere to remember when it first became true.
 * The node is identified by its position in the tree, hashed as we descend, which costs nothing in
 * the stored rule and needs no migration; editing the tree resets the timers, which is right,
 * because a condition that has been rewritten has not been true for any length of time. */
#define MAX_NODE_TIMERS PF_RULES_NODE_TIMERS
typedef pf_rules_node_timer node_timer;

typedef struct rstate_s {
	bool used;
	char rule[40], inst[40];
	node_timer timers[MAX_NODE_TIMERS];
	double held_since;    /* when the condition first became true, 0 = not true */
	double false_since;   /* when it went false, for the delay before it is declared over */
	double last_fired;
	bool armed;           /* false once fired, until the condition goes false again */
	bool raised;          /* there is an entry in the alarm table waiting to be cleared */
} rstate;

/* What a whole evaluation of one rule against one instance needs to carry with it. */
typedef struct {
	const cJSON *status;
	const inst *in;
	double db;
	double now;
	node_timer *timers;    /* where the per-condition clocks live; NULL while previewing */
} evalctx;

/* Has this node been true long enough? A node with no time of its own is answered at once.
 *
 * While previewing there is no state to keep a clock in, and none is wanted: the editor is asking
 * "is this true now", so it can show the condition, not "has it been true for five minutes". */
static bool held_long_enough(evalctx *cx, uint32_t path, const cJSON *node, bool now_true)
{
	double need = pf_json_num((cJSON *)node, "for_s", 0);
	if (need <= 0 || !cx->timers) return now_true;
	node_timer *t = NULL, *spare = NULL;
	for (int i = 0; i < MAX_NODE_TIMERS; i++) {
		if (cx->timers[i].path == path) { t = &cx->timers[i]; break; }
		if (!spare && cx->timers[i].path == 0) spare = &cx->timers[i];
	}
	if (!t) t = spare;
	/* More timed conditions than there is room for. Refusing is the safe direction: a rule that
	 * cannot time itself must not fire early. */
	if (!t) return false;
	t->path = path;
	if (!now_true) { t->since = 0; return false; }
	if (t->since == 0) t->since = cx->now;
	return cx->now - t->since >= need;
}

static bool eval_path(evalctx *cx, const cJSON *node, val *matched, uint32_t path);

/* A condition node is either a comparison or a group of them joined by all, any or not. */
static bool eval_node_inner(evalctx *cx, const cJSON *node, val *matched, uint32_t path)
{
	const cJSON *status = cx->status;
	const inst *in = cx->in;
	double db = cx->db;
	const cJSON *kids = jget(node, "conditions");
	if (cJSON_IsArray(kids)) {
		/* A group with nothing in it describes nothing, so it cannot be true. It used to return
		 * true, which meant a rule still being written matched every instance it watched and
		 * started sending the moment it was saved. NOT of nothing is nothing either: inverting an
		 * empty group would make a half-written rule fire, the same fault in a new hat. */
		if (cJSON_GetArraySize((cJSON *)kids) == 0) return false;
		const char *gop = pf_json_str((cJSON *)node, "op", "all");
		bool invert = !strcasecmp(gop, "not");
		bool any = !strcasecmp(gop, "any");
		bool result = !any;   /* all: start true; any: start false */
		const cJSON *k;
		int i = 0;
		cJSON_ArrayForEach(k, kids) {
			bool r = eval_path(cx, k, matched, path * 31u + (uint32_t)(++i));
			if (any) result = result || r; else result = result && r;
		}
		/* Not holds everything inside it and denies the lot: "not (this and that)". With one
		 * condition in it, which is how it is nearly always used, that is plain negation. */
		return invert ? !result : result;
	}
	const cJSON *tr = jget(node, "trait");
	if (!cJSON_IsString(tr)) return true;
	val a = trait_of(status, in, pf_json_str((cJSON *)node, "entity", "this"), tr->valuestring);
	const char *op = pf_json_str((cJSON *)node, "op", "==");

	/* Membership. "The mode is Hold or Smoke" is one thought and reads badly as a nested group of
	 * two comparisons, so it is one row with a list on the right. */
	if (!strcmp(op, "is_one_of") || !strcmp(op, "is_none_of")) {
		if (a.t == VT_NONE) return false;
		val none = v_none();
		bool found = false;
		const cJSON *it;
		cJSON_ArrayForEach(it, jget(node, "value")) {
			val b = resolve_operand(status, in, it);
			if (compare(&a, "is", &b, &none, 0)) { found = true; break; }
		}
		bool r = !strcmp(op, "is_one_of") ? found : !found;
		if (r && matched && matched->t == VT_NONE) *matched = a;
		return r;
	}

	val b = resolve_operand(status, in, jget(node, "value"));
	val b2 = resolve_operand(status, in, jget(node, "value2"));
	bool r = compare(&a, op, &b, &b2, db);
	if (r && matched && matched->t == VT_NONE) *matched = a;
	return r;
}

/* One node: what it says right now, then how long it has been saying it. */
static bool eval_path(evalctx *cx, const cJSON *node, val *matched, uint32_t path)
{
	return held_long_enough(cx, path, node, eval_node_inner(cx, node, matched, path));
}

/* `st` and `now` are what the per-condition clocks run on. Preview and Test pass neither: there is
 * no cook in progress to have been true for any length of time, and the editor is asking what the
 * conditions say right now. */
static bool eval_node_at(const cJSON *status, const inst *in, const cJSON *node, val *matched,
                         double db, rstate *st, double now)
{
	evalctx cx = { .status = status, .in = in, .db = db, .now = now, .timers = st ? st->timers : NULL };
	return eval_path(&cx, node, matched, 1u);
}

static bool eval_node(const cJSON *status, const inst *in, const cJSON *node, val *matched, double db)
{
	return eval_node_at(status, in, node, matched, db, NULL, 0);
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
		/* The grill ships without a name, and the setting is an empty string rather than absent,
		 * so the default only applied to a settings file that had never heard of it. Every fresh
		 * install therefore read the built-in rule as " is up to temperature". A grill nobody has
		 * named is "The grill". */
		else if (!strcmp(key, "grill")) { char g[48]; pf_set_str("globals.grill_name", g, sizeof g, ""); snprintf(buf, sizeof buf, "%s", g[0] ? g : "The grill"); }
		else if (!strcmp(key, "mode")) snprintf(buf, sizeof buf, "%s", pf_json_str((cJSON *)status, "mode", ""));
		else if (!strcmp(key, "time")) { char t[16]; time_t now = (time_t)pf_wall(); struct tm tmv; localtime_r(&now, &tmv); strftime(t, sizeof t, "%H:%M", &tmv); snprintf(buf, sizeof buf, "%s", t); }
		else if (!strcmp(key, "value") && matched && matched->t == VT_NUM) snprintf(buf, sizeof buf, "%.0f", matched->num);
		else {
			/* anything else is a trait: of the matched instance first, then the grill */
			if (!strcmp(key, "step")) v = trait_of(status, in, "this", "next_step");
			else if (!strcmp(key, "eta_step")) v = trait_of(status, in, "this", "eta_step");
			else if (!strcmp(key, "eta") || !strcmp(key, "eta_min")) v = trait_of(status, in, "this", "eta");
			else if (!strcmp(key, "cook_time")) v = trait_of(status, in, "grill", "cook_elapsed");
			else if (!strcmp(key, "grill_temp")) v = trait_of(status, in, "grill", "temp");
			else if (!strcmp(key, "setpoint")) v = trait_of(status, in, "grill", "setpoint");
			else if (!strcmp(key, "hopper")) v = trait_of(status, in, "hopper", "level");
			else if (!strcmp(key, "outdoor_temp")) v = trait_of(status, in, "weather", "temp");
			else v = trait_of(status, in, "this", key);
			if (v.t == VT_NUM) {
				if (!strcmp(key, "eta") || !strcmp(key, "eta_step") || !strcmp(key, "cook_time")) fmt_dur_token(buf, sizeof buf, v.num);
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

/* Everything this rule is still reporting is over, because the rule itself has stopped applying --
 * it was switched off, or the cook it only watches during has ended. A condition nobody is
 * evaluating any more cannot be true, and leaving its entry standing would be the system claiming
 * to know something it has stopped looking at. Caller holds the lock. */
static void retire_rule(const char *id)
{
	for (int i = 0; i < MAX_STATE; i++) {
		rstate *st = &g_state[i];
		if (!st->used || strcmp(st->rule, id) || !st->raised) continue;
		char key[96];
		snprintf(key, sizeof key, "RULE_%.32s:%.32s", id, st->inst);
		pf_alarms_retire(key);
		st->raised = false;
		st->held_since = 0;
		st->false_since = 0;
		st->armed = true;
	}
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
		else if (!strcmp(it->valuestring, "webpush")) m |= PF_SINK_WEBPUSH;
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

/* What this rule is about, for this instance: the identity the alarm table keys on. It must not
 * contain the message, which carries a temperature that changes every tick. */
static void rule_key(char *out, size_t n, const cJSON *rule, const inst *in)
{
	snprintf(out, n, "RULE_%.32s:%.32s", pf_json_str((cJSON *)rule, "id", "custom"),
	         in && in->label ? in->label : "-");
}

/* `renotify` is a deliberate re-announcement of a condition that is still true -- the rule's own
 * repeat interval, which exists so a critical standing alarm keeps asking to be dealt with. It is
 * the one reason to speak again about something already on the list. */
static void fire(const cJSON *rule, const cJSON *status, const inst *in, const val *matched, bool renotify)
{
	char title[160], body[320];
	render(title, sizeof title, pf_json_str((cJSON *)rule, "title", "{grill}"), status, in, matched);
	render(body, sizeof body, pf_json_str((cJSON *)rule, "body", ""), status, in, matched);
	char code[40], key[96];
	snprintf(code, sizeof code, "RULE_%.32s", pf_json_str((cJSON *)rule, "id", "custom"));
	rule_key(key, sizeof key, rule, in);

	/* A rule describes a condition, so it gets a standing entry rather than a fresh line every
	 * time it is still true. The table answers whether this is a new activation; only a new one is
	 * worth a phone buzzing, and only a new one counts as something to announce. */
	const char *name = pf_json_str((cJSON *)rule, "name", code);
	bool fresh = pf_alarms_raise(key, code, name, crit_of(rule), sink_mask(rule), title, body);
	if (!fresh && !renotify) return;
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
		const char *id = pf_json_str((cJSON *)rule, "id", "");
		if (!id[0]) continue;
		if (!cJSON_IsTrue(jget(rule, "enabled"))) { retire_rule(id); continue; }
		if (pf_json_bool((cJSON *)rule, "only_while_cooking", true) && !cooking) { retire_rule(id); continue; }

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
			/* The deadband only applies to a rule that is already reporting: it decides when the
			 * report ends, never when it starts. */
			rstate *pre = state_for(id, instances[i].label);
			double db = (pre && pre->raised) ? pf_json_num((cJSON *)rule, "deadband", 0) : 0;
			bool ok = when ? eval_node_at(status, &instances[i], when, &matched, db, pre, now) : false;
			if (every) { matches += ok ? 1 : 0; continue; }

			rstate *st = pre;
			if (!st) continue;
			if (!ok) {
				st->held_since = 0;
				st->armed = true;
				/* The condition is false again, so the thing it was reporting is over. Saying so
				 * is the whole difference between a notification system and a pile of receipts:
				 * a probe that has been plugged back in should not still be reported missing, and
				 * nobody should have to tell the grill that. */
				if (st->raised) {
					double off = pf_json_num((cJSON *)rule, "clear_after_s", 0);
					if (st->false_since == 0) st->false_since = now;
					if (now - st->false_since >= off) {
						char key[96];
						rule_key(key, sizeof key, rule, &instances[i]);
						pf_alarms_clear(key);
						st->raised = false;
						st->false_since = 0;
					}
				}
				continue;
			}
			st->false_since = 0;
			if (st->held_since == 0) st->held_since = now;
			if (now - st->held_since < hold) continue;
			bool due = st->armed || (repeat > 0 && now - st->last_fired >= repeat);
			if (!due) continue;
			if (st->last_fired > 0 && now - st->last_fired < cooldown) continue;
			bool renotify = !st->armed;   /* already reported; this is the repeat interval */
			st->armed = false;
			st->last_fired = now;
			st->raised = true;
			pthread_mutex_unlock(&g_mu);
			fire(rule, status, &instances[i], &matched, renotify);
			pthread_mutex_lock(&g_mu);
		}

		if (every && ni > 0) {
			rstate *st = state_for(id, "*");
			bool ok = matches == ni;
			if (st) {
				if (!ok) {
					st->held_since = 0;
					st->armed = true;
					if (st->raised) {
						char key[96];
						rule_key(key, sizeof key, rule, ni > 0 ? &instances[0] : NULL);
						pf_alarms_clear(key);
						st->raised = false;
					}
				} else {
					if (st->held_since == 0) st->held_since = now;
					bool due = st->armed || (repeat > 0 && now - st->last_fired >= repeat);
					if (now - st->held_since >= hold && due && !(st->last_fired > 0 && now - st->last_fired < cooldown)) {
						bool renotify = !st->armed;
						st->armed = false;
						st->last_fired = now;
						st->raised = true;
						val none = v_none();
						pthread_mutex_unlock(&g_mu);
						fire(rule, status, ni > 0 ? &instances[0] : NULL, &none, renotify);
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
		if (when && eval_node(status, &instances[i], when, &m, 0)) {
			if (matching) (*matching)++;
			if (show <= 0 || matched.t == VT_NONE) { show = i; matched = m; }
		}
	}
	const inst *in = show >= 0 ? &instances[show] : NULL;
	if (title && tn) render(title, tn, pf_json_str((cJSON *)rule, "title", ""), status, in, &matched);
	if (body && bn) render(body, bn, pf_json_str((cJSON *)rule, "body", ""), status, in, &matched);
}

bool pf_rules_eval_tree(const cJSON *node, const cJSON *facts, const char *domain,
                        pf_rules_clocks *clocks, double now)
{
	if (!node || !facts) return false;
	inst in = { .domain = domain && *domain ? domain : "grill", .name = "", .label = "", .obj = NULL };
	val matched = v_none();
	evalctx cx = { .status = facts, .in = &in, .db = 0, .now = now, .timers = clocks ? clocks->t : NULL };
	return eval_path(&cx, node, &matched, 1u);
}

int pf_rules_test(const cJSON *rule, const cJSON *status, char *err, size_t n)
{
	if (!rule || !status) { snprintf(err, n, "no rule"); return -1; }
	inst instances[MAX_INST];
	int ni = select_instances(status, rule, instances, MAX_INST);
	val none = v_none();
	fire(rule, status, ni > 0 ? &instances[0] : NULL, &none, true);   /* a test always speaks */
	return 0;
}

/* Everything the grill can be asked about.
 *
 * `delta` marks a temperature that is a DIFFERENCE rather than a reading: "degrees past target" and
 * "degrees from set point" are gaps, not places. It matters when the units change, because a gap
 * converts by the ratio alone while a reading also moves by the freezing point -- treating -50 F of
 * shortfall as an absolute would file it as -45 C instead of -27.8, and the rule would fire
 * somewhere else entirely. */
struct trait_def { const char *domain, *trait, *type, *unit, *label; bool delta; };
static const struct trait_def TRAIT_TABLE[] = {
		{ "probe", "temp", "temperature", "deg", "Temperature", false }, { "probe", "target", "temperature", "deg", "Target", false },
		{ "probe", "over", "temperature", "deg", "Degrees Past Target", true }, { "probe", "eta", "duration", "s", "Time To Target", false },
		{ "probe", "battery", "percent", "%", "Battery", false }, { "probe", "signal", "number", "bars", "Signal Bars", false },
		{ "probe", "rssi", "number", "dBm", "Signal Strength", false }, { "probe", "connected", "bool", "", "Connected", false },
		{ "probe", "wireless", "bool", "", "Is Bluetooth", false }, { "probe", "name", "string", "", "Name", false },
		{ "probe", "in_use", "bool", "", "In This Cook", false },
		{ "grill", "mode", "enum", "", "Mode", false }, { "grill", "temp", "temperature", "deg", "Pit Temperature", false },
		{ "grill", "over", "temperature", "deg", "Degrees From Set Point", true },
		{ "grill", "setpoint", "temperature", "deg", "Set Point", false }, { "grill", "error", "string", "", "Error Code", false },
		{ "grill", "cook_elapsed", "duration", "s", "Cook Time", false }, { "grill", "mode_remaining", "duration", "s", "Time Left In Mode", false },
		/* How long the grill has been aiming at the target it has now. A pit short of its set point
		 * is ordinary while it climbs; this is what separates climbing from not getting there. */
		{ "grill", "aiming_s", "duration", "s", "Time Since Mode Or Target Changed", false },
		{ "grill", "lid_open", "bool", "", "Lid Open", false },
		{ "output", "state", "bool", "", "State", false }, { "output", "percent", "percent", "%", "Fan Percent", false },
		{ "hopper", "level", "percent", "%", "Hopper Level", false },
		{ "controller", "duty", "number", "", "Auger Duty", false }, { "controller", "feedforward", "number", "", "Feed Forward", false },
		{ "weather", "temp", "temperature", "deg", "Outdoor Temperature", false }, { "weather", "wind", "number", "km/h", "Wind", false },
		{ "weather", "humidity", "percent", "%", "Humidity", false },
		{ "system", "wifi_signal", "percent", "%", "Wi-Fi Signal", false }, { "system", "tailscale_online", "bool", "", "Tailscale Online", false },
		{ "timer", "remaining", "duration", "s", "Time Remaining", false }, { "timer", "running", "bool", "", "Timer Running", false },
		/* What a recipe step can be asked about. The food probes are given as the hottest and the
		 * coolest of the ones in this cook, so "any of them has got there" and "all of them have"
		 * are each one row rather than a group that has to know how many probes are in the meat.
		 * "Rested" is the hottest one plus the climb it will still do off the heat, so a step that
		 * ends at 205 rested ends where the meat finishes rather than where it came off. */
		{ "step", "elapsed", "duration", "s", "Time In This Step", false },
		{ "step", "food_max", "temperature", "deg", "Hottest Food Probe", false },
		{ "step", "food_min", "temperature", "deg", "Coolest Food Probe", false },
		{ "step", "food_rested", "temperature", "deg", "Hottest Food Probe, Rested", false },
		{ "step", "prompt", "bool", "", "You Confirmed", false },
		{ "step", "lid", "bool", "", "Lid Opened", false },
	
};
#define N_TRAITS (sizeof TRAIT_TABLE / sizeof TRAIT_TABLE[0])

static const struct trait_def *trait_def_of(const char *domain, const char *trait)
{
	for (size_t i = 0; i < N_TRAITS; i++)
		if (!strcmp(TRAIT_TABLE[i].domain, domain) && !strcmp(TRAIT_TABLE[i].trait, trait)) return &TRAIT_TABLE[i];
	return NULL;
}

/* ------------------------------------------------------------------ catalogue and state */

cJSON *pf_rules_catalogue_json(const cJSON *status)
{
	/* type drives the editor: which operators to offer and how to render the value box */
	const struct trait_def *TRAITS = TRAIT_TABLE;
	static const char *const NUM_OPS[] = { ">", ">=", "<", "<=", "==", "!=", "between", "within", NULL };
	static const char *const BOOL_OPS[] = { "is_on", "is_off", NULL };
	static const char *const STR_OPS[] = { "is", "is_not", "is_one_of", "is_none_of", "contains", "empty", "not_empty", NULL };

	cJSON *o = cJSON_CreateObject();
	cJSON *domains = cJSON_AddArrayToObject(o, "domains");
	static const char *const DOMS[] = { "step", "probe", "grill", "output", "hopper", "controller", "weather", "system", "timer" };
	for (size_t d = 0; d < sizeof DOMS / sizeof DOMS[0]; d++) {
		cJSON *dj = cJSON_CreateObject();
		cJSON_AddStringToObject(dj, "id", DOMS[d]);
		cJSON_AddBoolToObject(dj, "multi", !strcmp(DOMS[d], "probe") || !strcmp(DOMS[d], "output"));
		cJSON *tj = cJSON_AddArrayToObject(dj, "traits");
		for (size_t i = 0; i < N_TRAITS; i++) {
			if (strcmp(TRAITS[i].domain, DOMS[d])) continue;
			cJSON *t = cJSON_CreateObject();
			cJSON_AddStringToObject(t, "id", TRAITS[i].trait);
			cJSON_AddStringToObject(t, "type", TRAITS[i].type);
			cJSON_AddStringToObject(t, "unit", TRAITS[i].unit);
			cJSON_AddStringToObject(t, "label", TRAITS[i].label);
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

	static const char *const TOKENS[] = { "probe", "temp", "target", "over", "eta", "eta_min", "step", "eta_step", "battery",
		"signal", "grill", "grill_temp", "setpoint", "mode", "hopper", "outdoor_temp", "cook_time", "value", "time", NULL };
	cJSON *tk = cJSON_AddArrayToObject(o, "tokens");
	for (int i = 0; TOKENS[i]; i++) cJSON_AddItemToArray(tk, cJSON_CreateString(TOKENS[i]));
	cJSON *md = cJSON_AddArrayToObject(o, "modes");
	for (const char *const *m = (const char *const[]){ "Stop", "Monitor", "Startup", "Reignite", "Smoke",
	     "Hold", "Shutdown", "Manual", "Error", NULL }; *m; m++)
		cJSON_AddItemToArray(md, cJSON_CreateString(*m));
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

/* Every temperature written into one condition, converted between units.
 *
 * A rule says "above 250" and means 250 of whatever the grill was showing when it was written. Left
 * alone, switching the grill to Celsius turns that into 250 C -- a rule that can never be true --
 * and a -50 F flame-out into -50 C, which fires on a grill that is merely a little cool. So the
 * rules carry the units they were written in, and the first load after a change rewrites them. A
 * difference converts by the ratio alone; a reading also moves by the freezing point. */
static void convert_node_units(cJSON *node, const char *domain, bool to_c)
{
	cJSON *kids = (cJSON *)jget(node, "conditions");
	if (cJSON_IsArray(kids)) {
		cJSON *k;
		cJSON_ArrayForEach(k, kids) convert_node_units(k, domain, to_c);
		return;
	}
	const cJSON *tr = jget(node, "trait");
	if (!cJSON_IsString(tr)) return;
	const char *dom = pf_json_str((cJSON *)node, "entity", "this");
	const struct trait_def *d = trait_def_of(!strcmp(dom, "this") ? domain : dom, tr->valuestring);
	if (!d || strcmp(d->type, "temperature")) return;

	/* absolute unless the trait is a gap; an offset and the width of a "within" are always gaps */
	const double R = 9.0 / 5.0;
	#define CONV(x, delta) (to_c ? ((delta) ? (x) / R : ((x) - 32.0) / R) : ((delta) ? (x) * R : (x) * R + 32.0))
	cJSON *v = (cJSON *)jget(node, "value");
	if (cJSON_IsNumber(v)) cJSON_SetNumberValue(v, CONV(v->valuedouble, d->delta));
	else if (cJSON_IsObject(v)) {
		cJSON *off = (cJSON *)jget(v, "offset");
		if (cJSON_IsNumber(off)) cJSON_SetNumberValue(off, CONV(off->valuedouble, true));
	}
	cJSON *v2 = (cJSON *)jget(node, "value2");
	if (cJSON_IsNumber(v2)) {
		bool width = !strcmp(pf_json_str(node, "op", ""), "within");
		cJSON_SetNumberValue(v2, CONV(v2->valuedouble, d->delta || width));
	}
	cJSON *dbv = (cJSON *)jget(node, "deadband");
	if (cJSON_IsNumber(dbv)) cJSON_SetNumberValue(dbv, CONV(dbv->valuedouble, true));
	#undef CONV
}

void pf_rules_convert_tree(cJSON *node, const char *domain, pf_units from, pf_units to)
{
	if (!node || from == to) return;
	convert_node_units(node, domain && *domain ? domain : "grill", to == PF_UNITS_C);
}

static void convert_rules_units(void)
{
	char u[8];
	pf_set_str("globals.units", u, sizeof u, "F");
	char was[8];
	pf_set_str("notify.rules_units", was, sizeof was, "");
	if (was[0] && was[0] == u[0]) return;

	cJSON *rules = pf_set_dup("notify.rules");
	if (cJSON_IsArray(rules) && was[0] && was[0] != u[0]) {
		bool to_c = u[0] == 'C';
		cJSON *r;
		cJSON_ArrayForEach(r, rules) {
			const char *dom = pf_json_str(r, "select.domain", "grill");
			cJSON *when = (cJSON *)jget(r, "when");
			if (when) convert_node_units(when, dom, to_c);
			cJSON *db = (cJSON *)jget(r, "deadband");
			if (cJSON_IsNumber(db)) cJSON_SetNumberValue(db, to_c ? db->valuedouble / (9.0 / 5.0) : db->valuedouble * (9.0 / 5.0));
		}
		char *txt = cJSON_PrintUnformatted(rules);
		if (txt) {
			cJSON *patch = cJSON_CreateObject();
			cJSON_AddItemToObject(patch, "rules", cJSON_Parse(txt));
			cJSON_AddStringToObject(patch, "rules_units", u[0] == 'C' ? "C" : "F");
			char *ptxt = cJSON_PrintUnformatted(patch);
			if (ptxt) { pf_settings_patch("notify", ptxt, NULL, 0); free(ptxt); }
			cJSON_Delete(patch);
			free(txt);
		}
		LOGI(TAG, "notification rules converted to %s", u[0] == 'C' ? "C" : "F");
	} else if (!was[0]) {
		cJSON *patch = cJSON_CreateObject();
		cJSON_AddStringToObject(patch, "rules_units", u[0] == 'C' ? "C" : "F");
		char *ptxt = cJSON_PrintUnformatted(patch);
		if (ptxt) { pf_settings_patch("notify", ptxt, NULL, 0); free(ptxt); }
		cJSON_Delete(patch);
	}
	cJSON_Delete(rules);
}

void pf_rules_init(void)
{
	pthread_mutex_lock(&g_mu);
	memset(g_state, 0, sizeof g_state);
	pthread_mutex_unlock(&g_mu);
	/* Rules are being reloaded, so nothing in the table is being tracked any more. Whatever is
	 * still true will raise itself again on the next tick. */
	pf_alarms_init();
	convert_rules_units();
	cJSON *rules = pf_set_dup("notify.rules");
	LOGI(TAG, "%d notification rule(s)", cJSON_IsArray(rules) ? cJSON_GetArraySize(rules) : 0);
	cJSON_Delete(rules);
}

void pf_rules_shutdown(void) {}
