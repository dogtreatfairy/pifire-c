#include "core/control.h"
#include "core/cmdq.h"
#include "core/db.h"
#include "core/env.h"
#include "core/events.h"
#include "core/history.h"
#include "core/log.h"
#include "core/outputs.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/util.h"
#include "controllers/registry.h"
#include "features/alarms.h"
#include "features/cookfile.h"
#include "features/learning.h"
#include "features/tuner.h"
#include "features/pellets.h"
#include "features/weather.h"
#include "platform/sim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "control"

static void enter_mode(pf_control *c, pf_mode m, double now);
static void recipe_begin_step(pf_control *c, double now);
static void recipe_advance(pf_control *c, double now);
static void learn_reset_window(pf_control *c, double now);
static void learn_rise_begin(pf_control *c, double now);
static void autotune_start(pf_control *c, double now);
static void autotune_finish(pf_control *c, bool ok, const char *why);
static double autotune_output(const pf_control *c);
static void autotune_cycle(const pf_control *c, int i, double *period, double *amp);

/* ------------------------------------------------------------------ settings -> cfg */

static void load_cfg(pf_cfg *g)
{
	cJSON *r = pf_settings_lock();
	pf_units u = pf_json_str(r, "globals.units", "F")[0] == 'C' ? PF_UNITS_C : PF_UNITS_F;
	g->units = u;
#define T(path, dflt) pf_to_c(pf_json_num(r, path, dflt), u)
#define D(path, dflt) pf_delta_to_c(pf_json_num(r, path, dflt), u)
#define N(path, dflt) pf_json_num(r, path, dflt)
#define B(path, dflt) pf_json_bool(r, path, dflt)
	g->hold_cycle_s = N("cycle_data.HoldCycleTime", 25);
	g->smoke_on_s = N("cycle_data.SmokeOnCycleTime", 15);
	g->smoke_off_s = N("cycle_data.SmokeOffCycleTime", 45);
	g->pmode = (int)N("cycle_data.PMode", 2);
	g->u_min = N("cycle_data.u_min", 0.1);
	g->u_max = N("cycle_data.u_max", 0.9);
	g->lid_detect = B("cycle_data.LidOpenDetectEnabled", false);
	g->lid_threshold_pct = N("cycle_data.LidOpenThreshold", 15);
	g->lid_pause_s = N("cycle_data.LidOpenPauseTime", 60);
	g->fan_pid = B("cycle_data.FanPidEnabled", false);

	g->min_startup_c = T("safety.minstartuptemp", 75);
	g->max_startup_c = T("safety.maxstartuptemp", 100);
	g->max_temp_c = T("safety.maxtemp", 550);
	g->restart_hot_c = T("safety.restart_hot_temp", 150);
	g->reignite_retries = (int)N("safety.reigniteretries", 1);
	g->startup_check = B("safety.startup_check", true);
	g->allow_manual = B("safety.allow_manual_changes", false);
	g->manual_override_s = N("safety.manual_override_time", 30);
	g->igniter_max_on_s = N("safety.igniter_max_on_s", 1200);
	g->auger_max_on_s = N("safety.auger_max_on_s", 60);
	g->probe_fault_s = N("safety.probe_fault_s", 10);
	g->relight_enabled = B("safety.relight_enabled", true);
	g->relight_drop_c = D("safety.relight_drop", 20);
	g->relight_recover_c = D("safety.relight_recover", 10);
	g->relight_recover_step_c = D("safety.relight_recover_step", 3);
	g->relight_timeout_s = N("safety.relight_timeout_s", 300);
	g->use_library = B("learning.use_library", true);
	g->error_cooldown_fan_s = N("safety.error_cooldown_fan_s", 300);
	g->coldstart = B("safety.coldstart.enabled", false);
	g->coldstart_delta_c = D("safety.coldstart.delta_rise", 12);
	g->coldstart_timeout_s = N("safety.coldstart.timeout_s", 0);
	g->coldstart_window_s = N("safety.coldstart.baseline_window_s", 60);
	g->coldstart_exit_on_rise = B("safety.coldstart.exit_on_rise", false);

	g->startup_duration_s = N("startup.duration", 240);
	g->prime_on_startup_g = N("startup.prime_on_startup", 0);
	double exit_user = N("startup.startup_exit_temp", 0);
	g->startup_exit_c = exit_user > 0 ? pf_to_c(exit_user, u) : 0;
	g->startup_exit_rise_c = D("startup.exit_rise", 15);
	const char *am = pf_json_str(r, "startup.start_to_mode.after_startup_mode", "Smoke");
	g->after_startup_mode = !strcasecmp(am, "Hold") ? PF_MODE_HOLD : PF_MODE_SMOKE;
	g->after_startup_setpoint_c = T("startup.start_to_mode.primary_setpoint", 165);
	g->smartstart = B("startup.smartstart.enabled", false);
	g->ss_exit_c = T("startup.smartstart.exit_temp", 120);
	cJSON *ranges = pf_json_path(r, "startup.smartstart.temp_range_list");
	cJSON *profs = pf_json_path(r, "startup.smartstart.profiles");
	g->ss_n = 0;
	cJSON *it;
	cJSON_ArrayForEach(it, ranges) { if (g->ss_n < PF_SS_MAX && cJSON_IsNumber(it)) g->ss_ranges_c[g->ss_n++] = pf_to_c(it->valuedouble, u); }
	int k = 0;
	cJSON_ArrayForEach(it, profs) {
		if (k > PF_SS_MAX) break;
		g->ss_prof[k].startuptime = pf_json_num(it, "startuptime", 240);
		g->ss_prof[k].augerontime = pf_json_num(it, "augerontime", 15);
		g->ss_prof[k].p_mode = pf_json_int(it, "p_mode", 2);
		k++;
	}
	for (; k <= PF_SS_MAX; k++) g->ss_prof[k] = g->ss_prof[k ? k - 1 : 0];
	g->startup_pwm_duty = (int)N("startup.pwm_duty_cycle", 100);
	g->shutdown_s = N("shutdown.shutdown_duration", 240);
	g->auto_power_off = B("shutdown.auto_power_off", false);

	g->splus_default = B("smoke_plus.enabled", false);
	g->splus_min_c = T("smoke_plus.min_temp", 160);
	g->splus_max_c = T("smoke_plus.max_temp", 220);
	g->splus_on_s = N("smoke_plus.on_time", 5);
	g->splus_off_s = N("smoke_plus.off_time", 5);
	g->splus_duty = (int)N("smoke_plus.duty_cycle", 75);
	g->splus_ramp = B("smoke_plus.fan_ramp", false);

	g->dc_fan = B("platform.dc_fan", false);
	g->pwm_control_default = B("pwm.pwm_control", false);
	g->pwm_update_s = N("pwm.update_time", 10);
	g->pwm_hz = (int)N("pwm.frequency", 25000);
	g->pwm_min_duty = (int)N("pwm.min_duty_cycle", 20);
	g->pwm_max_duty = (int)N("pwm.max_duty_cycle", 100);
	g->pwm_n = 0;
	cJSON_ArrayForEach(it, pf_json_path(r, "pwm.temp_range_list")) { if (g->pwm_n < PF_SS_MAX && cJSON_IsNumber(it)) g->pwm_ranges_c[g->pwm_n++] = pf_delta_to_c(it->valuedouble, u); }
	k = 0;
	cJSON_ArrayForEach(it, pf_json_path(r, "pwm.profiles")) { if (k <= PF_SS_MAX) g->pwm_profiles[k++] = pf_json_int(it, "duty_cycle", 100); }
	for (; k <= PF_SS_MAX; k++) g->pwm_profiles[k] = g->pwm_max_duty;

	g->augerrate = N("globals.augerrate", 0.3);
	g->prime_ignition = B("globals.prime_ignition", false);
	g->keepwarm_c = T("keep_warm.temp", 165);
	g->keepwarm_splus = B("keep_warm.s_plus", false);
	g->history_sample_s = N("history.sample_s", 3);
	g->clear_history_on_startup = B("history.clear_on_startup", true);
	pf_strlcpy(g->controller_id, pf_json_str(r, "controller.selected", "pid"), sizeof g->controller_id);
#undef T
#undef D
#undef N
#undef B
	pf_settings_unlock();
}

/* ------------------------------------------------------------------ controller lifecycle */

static char *controller_config_json(const char *id, pf_units u)
{
	char path[96];
	snprintf(path, sizeof path, "controller.config.%s", id);
	cJSON *cfg = pf_set_dup(path);
	if (!cfg) cfg = cJSON_CreateObject();
	cJSON_AddStringToObject(cfg, "_units", u == PF_UNITS_C ? "C" : "F");
	/* Whether the grill learns is settled in one place. A controller that can learn is told the
	 * answer rather than asking for it a second time under its own name. */
	cJSON_DeleteItemFromObject(cfg, "auto_tune");
	cJSON_AddBoolToObject(cfg, "auto_tune", pf_learning_enabled());
	char *s = cJSON_PrintUnformatted(cfg);
	cJSON_Delete(cfg);
	return s;
}

static void controller_destroy(pf_control *c)
{
	if (c->cinst && c->cops) c->cops->destroy(c->cinst);
	c->cinst = NULL;
	c->cops = NULL;
}

/* The three numbers someone types on the controller page: where the grill starts before anything
 * is measured or learned. Read straight from settings so a change made anywhere is seen. */
static void typed_gains(const char *id, double v[3])
{
	char path[96];
	snprintf(path, sizeof path, "controller.config.%s.PB", id); v[0] = pf_set_num(path, 0);
	snprintf(path, sizeof path, "controller.config.%s.Ti", id); v[1] = pf_set_num(path, 0);
	snprintf(path, sizeof path, "controller.config.%s.Td", id); v[2] = pf_set_num(path, 0);
}

static int controller_load(pf_control *c, const char *id)
{
	controller_destroy(c);
	const pf_controller_ops *ops = pf_controller_find(id);
	if (!ops) {
		LOGE(TAG, "controller '%s' not found, falling back to 'pid'", id);
		ops = pf_controller_find("pid");
		if (!ops) return -1;
	}
	char ns[64];
	snprintf(ns, sizeof ns, "controller.%s", ops->id);
	pf_env_init(&c->cenv, ns);
	char *cfg = controller_config_json(ops->id, c->cfg.units);
	c->cinst = ops->create(cfg, &c->cenv);
	free(cfg);
	if (!c->cinst) { LOGE(TAG, "controller '%s' failed to create", ops->id); return -1; }
	c->cops = ops;
	typed_gains(ops->id, c->typed_gains);
	c->ctrl_reset_needed = true;
	c->safety.ctrl_fault_count = 0;
	LOGI(TAG, "controller '%s' loaded", ops->id);
	return 0;
}

/* What would hold this set point if the grill were asked to hold it right now. The library first,
 * because it is measured at the set point; then whatever the controller is carrying, which it
 * reports in its own state; then the numbers that were typed. This is computed for every status
 * publish so the app can say what is in force without waiting for the next cook to prove it. */
static pf_ctrl_tuning ctrl_tuning(pf_control *c)
{
	pf_ctrl_tuning t = { 0 };
	if (c->cfg.use_library && pf_learning_gains(c->setpoint_c, &t.PB_c, &t.Ti, &t.Td)) {
		pf_strlcpy(t.src, "tuned", sizeof t.src);
		t.valid = true;
		return t;
	}
	char buf[512];
	if (c->cinst && c->cops->state_json && c->cops->state_json(c->cinst, buf, sizeof buf) > 0) {
		cJSON *j = cJSON_Parse(buf);
		if (j) {
			cJSON *pb = cJSON_GetObjectItem(j, "PB_c"), *ti = cJSON_GetObjectItem(j, "Ti"), *td = cJSON_GetObjectItem(j, "Td");
			if (cJSON_IsNumber(pb) && pb->valuedouble > 0) {
				t.PB_c = pb->valuedouble;
				t.Ti = cJSON_IsNumber(ti) ? ti->valuedouble : 0;
				t.Td = cJSON_IsNumber(td) ? td->valuedouble : 0;
				pf_strlcpy(t.src, cJSON_IsTrue(cJSON_GetObjectItem(j, "learned")) ? "learned" : "typed", sizeof t.src);
				t.valid = true;
			}
			cJSON_Delete(j);
		}
	}
	if (!t.valid) {
		double v[3];
		typed_gains(c->cops ? c->cops->id : c->cfg.controller_id, v);
		if (v[0] > 0) {
			t.PB_c = pf_delta_to_c(v[0], c->cfg.units); t.Ti = v[1]; t.Td = v[2];
			pf_strlcpy(t.src, "typed", sizeof t.src);
			t.valid = true;
		}
	}
	return t;
}

/* Clearing, in the three shapes it actually comes in. Each one has to reach the controller as well
 * as the database, because the controller holds its own copy of both what it was given and what it
 * worked out; clearing only the stored copy leaves the old numbers running until the next restart. */
static void forget_learning(pf_control *c, int what, const char *why)
{
	unsigned ctrl = 0;
	switch (what) {
	case PF_CLEAR_LEARNING:
		pf_learning_forget();
		ctrl = PF_FORGET_REFINEMENT;
		break;
	case PF_CLEAR_TUNING:
		/* Going back to the typed values means the refinements built on the measurement go too:
		 * they were corrections to a number that is about to stop existing. */
		pf_learning_clear_tuning();
		pf_learning_forget();
		ctrl = PF_FORGET_REFINEMENT | PF_FORGET_TUNING;
		break;
	case PF_CLEAR_FOR_BASELINE:
		/* The library keeps what the run just measured -- that is the point of the run -- and
		 * everything the controller was carrying goes, so the baseline governs from here rather
		 * than competing with a model fitted to some earlier cook.
		 *
		 * The steady-state observations stay. They are not a refinement of the tuning: they measure
		 * how much fuel this grill burns to hold a temperature, which a new proportional band does
		 * not change, and the next tuning run needs them -- they are where the static gain comes
		 * from, and they put the grill on its proper operating point before the relay starts.
		 * Clearing them here cost a real measurement: the run that followed had no feed-forward at
		 * all, settled differently, and came back with a limit cycle two thirds slower than the
		 * run before it. */
		ctrl = PF_FORGET_REFINEMENT | PF_FORGET_TUNING;
		break;
	default: return;
	}
	if (c->cinst && c->cops->forget) c->cops->forget(c->cinst, ctrl);
	if (what == PF_CLEAR_TUNING)
		pf_events_emit("Tuning_Cleared", "Measured tuning cleared",
		               "The tuning library is gone and the grill is back to the Proportional Band, Integral Time and Derivative Time typed on the controller page (%s).", why);
	else
		pf_events_emit("Learning_Cleared", "Learning cleared",
		               "The grill starts learning again from the tuning it has (%s).%s", why,
		               what == PF_CLEAR_FOR_BASELINE ? " What it has measured about its fuel use is kept." : "");
}

static void controller_fill_defaults(void)
{
	/* make sure settings.controller.config.<id> has every option so the UI can render it */
	for (int i = 0; i < pf_controller_count(); i++) {
		const pf_controller_ops *ops = pf_controller_at(i);
		if (!ops->config_schema_json) continue;
		cJSON *schema = cJSON_Parse(ops->config_schema_json);
		cJSON *opt;
		cJSON_ArrayForEach(opt, schema) {
			const char *name = pf_json_str(opt, "option_name", NULL);
			cJSON *dflt = cJSON_GetObjectItemCaseSensitive(opt, "option_default");
			if (!name || !dflt) continue;
			char path[128];
			snprintf(path, sizeof path, "controller.config.%s.%s", ops->id, name);
			cJSON *r = pf_settings_lock();
			bool have = pf_json_path(r, path) != NULL;
			pf_settings_unlock();
			if (!have) pf_set_put(path, cJSON_Duplicate(dflt, 1));
		}
		cJSON_Delete(schema);
	}
}

/* ------------------------------------------------------------------ helpers */

static void cycle_cfg_for(pf_control *c)
{
	c->ccfg.cycle_s = c->cfg.hold_cycle_s;
	c->ccfg.u_min = c->cfg.u_min;
	c->ccfg.u_max = c->cfg.u_max;
	c->ccfg.max_on_s = c->cfg.auger_max_on_s;
}

static void fan_on(pf_control *c, int pct)
{
	pf_outputs_set(PF_OUT_FAN, true);
	if (c->cfg.dc_fan) pf_outputs_fan_pct(pct > 0 ? pct : 100);
	c->fan_ramping = false;
}

static void smoke_cycle(pf_control *c, double now)
{
	double on = c->cfg.smoke_on_s, off = c->cfg.smoke_off_s + c->cfg.pmode * 10;
	if (c->cfg.smartstart && (c->mode == PF_MODE_STARTUP || c->mode == PF_MODE_REIGNITE || c->mode == PF_MODE_SMOKE)) {
		on = c->cfg.ss_prof[c->ss_profile].augerontime;
		off = c->cfg.smoke_off_s + c->cfg.ss_prof[c->ss_profile].p_mode * 10;
	}
	pf_cycle_begin_fixed(&c->cycle, &c->ccfg, now, on, off);
	c->u_raw = c->u_applied = c->cycle.u_applied;
}

static void select_smartstart_profile(pf_control *c)
{
	c->ss_profile = c->cfg.ss_n;
	for (int i = 0; i < c->cfg.ss_n; i++)
		if (c->pit_c < c->cfg.ss_ranges_c[i]) { c->ss_profile = i; break; }
}

static void event(int level, const char *code, const char *msg)
{
	if (pf_db_handle()) pf_db_event(level, code, msg);
}

/* ------------------------------------------------------------------ ambient estimate
 * The feed-forward is u = a + b * (setpoint - ambient), so a wrong ambient starves or floods the fire:
 * a grill restarted while still hot must not take its own pit temperature (130 C) as "outdoor air".
 * Only readings that can plausibly be outdoor air count; otherwise the last plausible value seen
 * (persisted across restarts) is used, and 20 C before any has been seen. */
#define AMBIENT_MAX_C 50.0

static void ambient_provisional(pf_control *c)
{
	if (c->pit_valid && c->pit_c <= AMBIENT_MAX_C) { c->ambient_c = c->pit_c; return; }
	if (!isnan(c->ambient_c) && c->ambient_c <= AMBIENT_MAX_C) return;
	char buf[32];
	if (pf_db_handle() && pf_db_kv_get("control", "ambient_last", buf, sizeof buf) == 0) {
		double v = atof(buf);
		if (v > -40 && v <= AMBIENT_MAX_C) { c->ambient_c = v; return; }
	}
	c->ambient_c = 20;
}

static void ambient_remember(pf_control *c)
{
	static double last_saved = NAN;
	if (isnan(c->ambient_c) || c->ambient_c > AMBIENT_MAX_C || !pf_db_handle()) return;
	if (!isnan(last_saved) && fabs(last_saved - c->ambient_c) < 1.0) return;
	last_saved = c->ambient_c;
	char buf[32];
	snprintf(buf, sizeof buf, "%.1f", c->ambient_c);
	pf_db_kv_put("control", "ambient_last", buf);
}

/* ------------------------------------------------------------------ mode transitions */

static void enter_mode(pf_control *c, pf_mode m, double now)
{
	pf_mode prev = c->mode;
	c->mode = m;
	c->mode_start = now;
	c->aim_since = now;
	{
		bool cooking = m == PF_MODE_STARTUP || m == PF_MODE_REIGNITE || m == PF_MODE_SMOKE ||
		               m == PF_MODE_HOLD || m == PF_MODE_SHUTDOWN || m == PF_MODE_PRIME;
		pf_probes_set_cooking(cooking);
	}
	c->lid_open = false;
	c->fan_pid_active = false;
	c->fan_ramping = false;
	c->target_reached = false;
	memset(c->manual_until, 0, sizeof c->manual_until);
	pf_cycle_stop(&c->cycle);
	if (c->autotune.active) autotune_finish(c, false, "Mode changed.");
	learn_reset_window(c, now);
	c->safety.igniter_locked_out = false;
	c->safety.igniter_on_since = 0;
	c->safety.primary_invalid_since = 0;
	cycle_cfg_for(c);

	pf_outputs_set(PF_OUT_IGNITER, false);
	pf_outputs_set(PF_OUT_AUGER, false);
	c->auger_on_since = now;

	switch (m) {
	case PF_MODE_STOP:
		pf_outputs_all_off();
		c->s_plus = false;
		c->next_mode = PF_MODE_STOP;
		if (c->cook_start_wall > 0) {
			pf_cookfile_request(c->cook_start_wall, pf_wall(), c->auger_total_on_s, c->cook_max_pit_c);
			c->cook_start_wall = 0;
		}
		break;
	case PF_MODE_MONITOR:
	case PF_MODE_MANUAL:
		pf_outputs_set(PF_OUT_FAN, false);
		pf_outputs_set(PF_OUT_POWER, false);
		break;
	case PF_MODE_PRIME:
		pf_outputs_set(PF_OUT_FAN, false);
		pf_outputs_set(PF_OUT_POWER, true);
		c->prime_duration_s = c->cfg.augerrate > 0 ? c->prime_amount_g / c->cfg.augerrate : 0;
		if (c->cfg.prime_ignition && c->next_mode == PF_MODE_STARTUP) pf_outputs_set(PF_OUT_IGNITER, true);
		pf_cycle_begin_fixed(&c->cycle, &c->ccfg, now, c->prime_duration_s, 1);
		pf_outputs_set(PF_OUT_AUGER, true);
		break;
	case PF_MODE_STARTUP:
	case PF_MODE_REIGNITE:
		c->startup_base_c = c->pit_valid ? c->pit_c : NAN;   /* reference for startup.exit_rise */
		if (m == PF_MODE_STARTUP) {
			c->cook_start_wall = pf_wall();
			c->auger_total_on_s = 0;
			c->cook_max_pit_c = 0;
			pf_safety_reset(c);
			if (c->cfg.clear_history_on_startup && prev == PF_MODE_STOP) pf_history_clear();
			if (!c->ambient_from_probe) ambient_provisional(c);   /* refined by the cold-start baseline */
			learn_rise_begin(c, now);
		}
		pf_outputs_set(PF_OUT_POWER, true);
		fan_on(c, c->cfg.dc_fan ? c->cfg.startup_pwm_duty : 100);
		pf_outputs_set(PF_OUT_IGNITER, true);
		c->raw_startup_c = c->pit_c;
		c->startup_duration_s = c->cfg.startup_duration_s;
		c->startup_exit_c = c->cfg.startup_exit_c;
		if (c->cfg.smartstart) {
			select_smartstart_profile(c);
			c->startup_duration_s = c->cfg.ss_prof[c->ss_profile].startuptime;
			c->startup_exit_c = c->cfg.ss_exit_c;
		}
		if (c->startup_exit_c > 0 && c->raw_startup_c >= c->startup_exit_c) c->startup_exit_c = 0; /* starting hot: force full ignite */
		pf_safety_on_startup_enter(c, now);
		smoke_cycle(c, now);
		pf_outputs_set(PF_OUT_AUGER, true);
		break;
	case PF_MODE_SMOKE:
		pf_outputs_set(PF_OUT_POWER, true);
		fan_on(c, c->duty_cycle);
		smoke_cycle(c, now);
		pf_outputs_set(PF_OUT_AUGER, true);
		break;
	case PF_MODE_HOLD:
		pf_outputs_set(PF_OUT_POWER, true);
		fan_on(c, c->duty_cycle);
		c->ctrl_reset_needed = true;
		pf_cycle_begin(&c->cycle, &c->ccfg, now, c->cfg.u_min);
		c->u_raw = c->u_applied = c->cycle.u_applied;
		pf_outputs_set(PF_OUT_AUGER, true);
		break;
	case PF_MODE_SHUTDOWN:
		pf_outputs_set(PF_OUT_POWER, true);
		fan_on(c, c->duty_cycle);
		break;
	case PF_MODE_ERROR:
		c->safety.error_fan_until = (c->pit_valid && c->pit_c > c->cfg.restart_hot_c) ? now + c->cfg.error_cooldown_fan_s : 0;
		if (c->safety.error_fan_until > now) { pf_outputs_set(PF_OUT_POWER, true); fan_on(c, 100); }
		else pf_outputs_all_off();
		break;
	default: break;
	}
	if (m == PF_MODE_HOLD) LOGI(TAG, "mode %s -> %s (setpoint %.1f C)", pf_mode_name(prev), pf_mode_name(m), c->setpoint_c);
	else LOGI(TAG, "mode %s -> %s", pf_mode_name(prev), pf_mode_name(m));
	char msg[96];
	snprintf(msg, sizeof msg, "Entered %s mode", pf_mode_name(m));
	event(PF_LVL_INFO, "MODE", msg);
}

void pf_control_request(pf_control *c, pf_mode mode, double setpoint_c)
{
	c->req_pending = true;
	c->req_mode = mode;
	c->req_setpoint_c = setpoint_c;
}

static void apply_request(pf_control *c, double now)
{
	if (!c->req_pending) return;
	c->req_pending = false;
	pf_mode m = c->req_mode;
	if (c->req_setpoint_c > 0) c->setpoint_c = c->req_setpoint_c;

	if (c->mode == PF_MODE_ERROR && m != PF_MODE_STOP) { LOGW(TAG, "in ERROR: only Stop is accepted"); return; }
	if (m == PF_MODE_STOP) { c->safety.error_code[0] = 0; c->safety.error_msg[0] = 0; }

	if (m == PF_MODE_HOLD && c->setpoint_c <= 0) c->setpoint_c = c->cfg.after_startup_setpoint_c;

	if (m == PF_MODE_STARTUP) {
		c->next_mode = c->cfg.after_startup_mode;
		if (c->cfg.after_startup_mode == PF_MODE_HOLD && c->req_setpoint_c <= 0) c->setpoint_c = c->cfg.after_startup_setpoint_c;
		if (c->cfg.prime_on_startup_g > 0 && (c->mode == PF_MODE_STOP || c->mode == PF_MODE_MONITOR)) {
			c->prime_amount_g = c->cfg.prime_on_startup_g;
			c->next_mode = PF_MODE_STARTUP;
			enter_mode(c, PF_MODE_PRIME, now);
			return;
		}
	}
	bool idle = c->mode == PF_MODE_STOP || c->mode == PF_MODE_MONITOR;
	if (m == PF_MODE_HOLD && idle) {
		/* Hold from cold (Stop or Monitor): run startup first, then hold */
		c->next_mode = PF_MODE_HOLD;
		enter_mode(c, PF_MODE_STARTUP, now);
		return;
	}
	if (m == PF_MODE_SMOKE && idle) {
		c->next_mode = PF_MODE_SMOKE;
		enter_mode(c, PF_MODE_STARTUP, now);
		return;
	}
	if (m == PF_MODE_HOLD && c->mode == PF_MODE_HOLD) {
		/* setpoint change only */
		c->target_reached = false;
		if (c->cinst) { c->ctrl_reset_needed = true; }
		learn_reset_window(c, now);
		return;
	}
	enter_mode(c, m, now);
}

/* ------------------------------------------------------------------ commands */

static void handle_cmd(pf_control *c, const pf_cmd *cmd, double now)
{
	pf_units u = c->cfg.units;
	switch (cmd->type) {
	case PF_CMD_STOP:
		enter_mode(c, PF_MODE_STOP, now);
		c->recipe.active = false;
		c->req_pending = false;
		c->safety.error_code[0] = 0; c->safety.error_msg[0] = 0;
		break;
	case PF_CMD_MODE:
		if (cmd->flag && (c->mode == PF_MODE_STARTUP || c->mode == PF_MODE_REIGNITE) && (cmd->mode == PF_MODE_SMOKE || cmd->mode == PF_MODE_HOLD)) {
			/* user forces the end of startup: the fire is lit by their judgement, so the flame-out floor must
			 * not sit above the pit they are looking at (it would trip on the next tick) */
			double sp = cmd->num > 0 ? pf_to_c(cmd->num, u) : c->setpoint_c;
			if (cmd->mode == PF_MODE_HOLD && sp <= 0) sp = c->cfg.after_startup_setpoint_c;
			c->setpoint_c = sp;
			pf_safety_on_startup_exit(c, now);
			if (c->pit_valid && c->safety.floor_c > c->pit_c - 3.0) c->safety.floor_c = c->pit_c - 3.0;
			c->safety.floor_set = true;
			c->s_plus = c->s_plus || c->cfg.splus_default;
			LOGW(TAG, "startup ended by user at %.0f C; flame-out floor %.0f C", c->pit_c, c->safety.floor_c);
			event(PF_LVL_WARN, "W09_STARTUP_SKIPPED", "Startup ended early by the user");
			enter_mode(c, cmd->mode, now);
			break;
		}
		pf_control_request(c, cmd->mode, cmd->num > 0 ? pf_to_c(cmd->num, u) : 0);
		break;
	case PF_CMD_SETPOINT:
		if (cmd->num > 0) {
			c->setpoint_c = pf_to_c(cmd->num, u);
			c->target_reached = false;
			c->aim_since = now;
			learn_reset_window(c, now);
			if (c->mode == PF_MODE_HOLD) c->ctrl_reset_needed = true;
			else if (c->mode == PF_MODE_SMOKE) pf_control_request(c, PF_MODE_HOLD, c->setpoint_c);
		}
		break;
	case PF_CMD_SMOKE_PLUS: c->s_plus = cmd->flag; break;
	case PF_CMD_PWM_CONTROL: c->pwm_control = cmd->flag; break;
	case PF_CMD_DUTY_CYCLE:
		c->duty_cycle = (int)pf_clamp(cmd->num, c->cfg.pwm_min_duty, c->cfg.pwm_max_duty);
		if (pf_outputs_get(PF_OUT_FAN)) pf_outputs_fan_pct(c->duty_cycle);
		break;
	case PF_CMD_MANUAL_OUTPUT: {
		bool free_mode = c->mode == PF_MODE_MANUAL || c->mode == PF_MODE_MONITOR;   /* outputs are idle: direct control is safe */
		if (!free_mode && !c->cfg.allow_manual) { LOGW(TAG, "manual output change refused (not in Manual/Monitor mode)"); break; }
		double until = free_mode ? 1e18 : now + c->cfg.manual_override_s;
		if (!strcmp(cmd->str, "pwm")) { pf_outputs_fan_pct((int)cmd->num); c->manual_until[PF_OUT_FAN] = until; break; }
		for (int i = 0; i < PF_OUT_COUNT; i++)
			if (!strcmp(cmd->str, pf_output_name((pf_output)i))) {
				/* A variable-speed fan has a relay and a duty, and they are separate. Switching the
				 * relay on while the duty still reads zero from whatever ran last gives a fan that
				 * is on and not turning, which looks like a broken switch. Asking for a fan means
				 * asking for air, so give it full speed unless a duty has already been chosen. */
				if (i == PF_OUT_FAN && cmd->flag && c->cfg.dc_fan && pf_outputs_get_fan_pct() <= 0)
					pf_outputs_fan_pct(c->cfg.pwm_max_duty > 0 ? c->cfg.pwm_max_duty : 100);
				pf_outputs_set((pf_output)i, cmd->flag);
				c->manual_until[i] = until;
				if (i == PF_OUT_AUGER && cmd->flag) c->auger_on_since = now;
			}
		break;
	}
	case PF_CMD_LID_TOGGLE:
		if (c->mode == PF_MODE_HOLD) {
			if (c->lid_open) { c->lid_open = false; fan_on(c, c->duty_cycle); }
			else { c->lid_open = true; c->lid_open_until = now + c->cfg.lid_pause_s; pf_outputs_set(PF_OUT_AUGER, false); pf_outputs_set(PF_OUT_FAN, false); pf_cycle_stop(&c->cycle); c->target_reached = false; }
		}
		break;
	case PF_CMD_PRIME:
		c->prime_amount_g = cmd->num;
		c->next_mode = !strcasecmp(cmd->str, "Startup") ? PF_MODE_STARTUP : PF_MODE_STOP;
		if (c->mode == PF_MODE_STOP || c->mode == PF_MODE_MONITOR) enter_mode(c, PF_MODE_PRIME, now);
		break;
	case PF_CMD_SETTINGS_CHANGED:
		pf_control_reload_settings(c);
		break;
	case PF_CMD_CONTROLLER_CHANGED:
		load_cfg(&c->cfg);
		controller_load(c, c->cfg.controller_id);
		break;
	case PF_CMD_PROBES_CHANGED:
		pf_probes_init();
		break;
	case PF_CMD_CLEAR_ERROR:
		if (c->mode == PF_MODE_ERROR) enter_mode(c, PF_MODE_STOP, now);
		break;
	case PF_CMD_NOTIFY_TARGET:
		pf_notify_sync(&c->notify, &c->sensors);
		if (pf_notify_set_target(&c->notify, cmd->str, cmd->num > 0 ? pf_to_c(cmd->num, u) : 0, cmd->aux))
			LOGW(TAG, "notify target: unknown probe '%s'", cmd->str);
		break;
	case PF_CMD_NOTIFY_LIMITS:
		pf_notify_sync(&c->notify, &c->sensors);
		pf_notify_set_limits(&c->notify, cmd->str, cmd->num > 0 ? pf_to_c(cmd->num, u) : 0, cmd->num2 > 0 ? pf_to_c(cmd->num2, u) : 0);
		break;
	case PF_CMD_TIMER_START: pf_notify_timer_start(&c->notify, cmd->num, cmd->aux, now); break;
	case PF_CMD_TIMER_PAUSE: pf_notify_timer_pause(&c->notify, now); break;
	case PF_CMD_TIMER_RESUME: pf_notify_timer_resume(&c->notify, now); break;
	case PF_CMD_TIMER_CANCEL: pf_notify_timer_cancel(&c->notify); break;
	case PF_CMD_NOTIFY_TEST: pf_events_emit("Test_Notify", "Test notification", "This is a test from PiFire."); break;
	case PF_CMD_RECIPE_START:
		if (c->mode == PF_MODE_ERROR) break;
		if (pf_recipe_load((int)cmd->num, &c->recipe.r)) { LOGW(TAG, "recipe %d not found", (int)cmd->num); break; }
		c->recipe.active = true;
		c->recipe.step = 0;
		memset(c->recipe.flags, 0, sizeof c->recipe.flags);
		/* Something is managing the grill again, so the question left by the last recipe -- that a
		 * fire was burning with nothing behind it -- is no longer a question. */
		c->recipe.left_running = false;
		pf_alarms_clear("RECIPE:left_running");
		recipe_begin_step(c, now);
		break;
	case PF_CMD_PROBES_IN_USE:
		pf_probes_set_in_use(cmd->str);
		LOGI(TAG, "probes in the food: %s", cmd->str[0] ? cmd->str : "none");
		break;
	case PF_CMD_RECIPE_NEXT:
		/* Next records that the cook has answered. Whether that ends the step is the step's own
		 * condition to decide -- with "the lid AND you confirmed" it does not, until the lid has
		 * been opened too, because the point of asking for both is that the meat has come off. */
		if (c->recipe.active) {
			c->recipe.prompt_given = true; c->recipe.last_eval = 0;
			/* a pause the cook asked for is released by the same tap, and the step goes on to end */
			if (c->recipe.step < 16 && c->recipe.flags[c->recipe.step] == 1) c->recipe.flags[c->recipe.step] = 0;
		}
		break;
	/* Stepping by hand, forwards or back. The step is ended or begun again exactly as the recipe
	 * would have done it; the app asks before sending either. */
	case PF_CMD_RECIPE_SKIP:
		if (c->recipe.active) { LOGI(TAG, "recipe '%s' step %d skipped by user", c->recipe.r.name, c->recipe.step + 1); recipe_advance(c, now); }
		break;
	case PF_CMD_RECIPE_FLAG: {
		int i = (int)cmd->num;
		if (c->recipe.active && i >= 0 && i < c->recipe.r.nsteps && i < 16) {
			c->recipe.flags[i] = (unsigned char)cmd->num2;
			LOGI(TAG, "recipe '%s' step %d: %s", c->recipe.r.name, i + 1,
			     cmd->num2 == 1 ? "will pause when it ends" : cmd->num2 == 2 ? "will be skipped" : cmd->num2 == 3 ? "will continue on its own" : "no override");
		}
		break;
	}
	case PF_CMD_RECIPE_BACK:
		if (c->recipe.active && c->recipe.step > 0) { c->recipe.step--; LOGI(TAG, "recipe '%s' back to step %d by user", c->recipe.r.name, c->recipe.step + 1); recipe_begin_step(c, now); }
		break;
	case PF_CMD_RECIPE_STOP:
		if (c->recipe.active) {
			c->recipe.active = false;
			cJSON_Delete(c->recipe.ends);
			c->recipe.ends = NULL;
			LOGI(TAG, "recipe stopped by user");
		}
		break;
	case PF_CMD_AUTOTUNE_START: autotune_start(c, now); break;
	case PF_CMD_AUTOTUNE_STOP: if (c->autotune.active) autotune_finish(c, false, "Stopped by user."); break;
	case PF_CMD_TUNING_APPLY: {
		if (!c->cinst || !c->cops->apply_tuning) { LOGW(TAG, "controller '%s' does not accept tuning", c->cops ? c->cops->id : "?"); break; }
		pf_autotune_result a = pf_learning_autotune();
		pf_fopdt p = pf_learning_fopdt();
		c->cops->apply_tuning(c->cinst, a.valid ? a.Ku : 0, a.valid ? a.Pu : 0, p.valid ? p.K : 0, p.valid ? p.tau : 0, p.valid ? p.theta : 0);
		/* persist PB/Ti/Td into settings for controllers that use the standard names */
		if (a.valid) {
			char path[96];
			snprintf(path, sizeof path, "controller.config.%s.PB", c->cops->id);
			pf_set_put_num(path, round(pf_delta_from_c(a.PB_c, c->cfg.units) * 10) / 10);
			snprintf(path, sizeof path, "controller.config.%s.Ti", c->cops->id);
			pf_set_put_num(path, round(a.Ti));
			snprintf(path, sizeof path, "controller.config.%s.Td", c->cops->id);
			pf_set_put_num(path, round(a.Td));
		}
		pf_settings_save();
		pf_events_emit("Tuning_Applied", "Tuning applied", "Controller '%s' now uses the learned tuning.", c->cops->id);
		/* Those numbers were just written into the starting values by the daemon, not typed by
		 * anyone, so they must not read as a change of mind the next time settings are reloaded. */
		typed_gains(c->cops->id, c->typed_gains);
		break;
	}
	case PF_CMD_FORGET_LEARNING:
		forget_learning(c, cmd->aux, cmd->str[0] ? cmd->str : "asked for");
		break;
	default: break;
	}
}

/* ------------------------------------------------------------------ recipe runner */

/* The food probes in this cook as the three numbers a step condition can ask about: the hottest,
 * the coolest, and the hottest plus the climb it will still do once it is off the heat. Any one of
 * them getting there is the hottest; all of them is the coolest; where the meat finishes rather
 * than where it came off is the rested one. NAN when nothing is in the meat. */
typedef struct { double hi, lo, avg, rested, battery, eta; } food_stats;

static food_stats food_numbers(pf_control *c)
{
	food_stats f = { NAN, NAN, NAN, NAN, NAN, -1 };
	double sum = 0, rate_at_hi = 0;
	int n = 0;
	for (int i = 0; i < c->sensors.n; i++) {
		const pf_probe_reading *p = &c->sensors.p[i];
		if (p->role != PF_PROBE_FOOD || !p->enabled || !p->in_use) continue;
		/* battery and time-to-target are about the probe, not its reading, and count even while
		 * a reading is briefly missing */
		if (p->wireless && p->battery >= 0 && (isnan(f.battery) || p->battery < f.battery)) f.battery = p->battery;
		const pf_notify_probe *np = pf_notify_find(&c->notify, p->label);
		if (np && np->eta_s >= 0 && (f.eta < 0 || np->eta_s < f.eta)) f.eta = np->eta_s;
		if (!p->valid) continue;
		if (isnan(f.hi) || p->temp_c > f.hi) { f.hi = p->temp_c; rate_at_hi = pf_notify_probe_rate(&c->notify, p->label); }
		if (isnan(f.lo) || p->temp_c < f.lo) f.lo = p->temp_c;
		sum += p->temp_c; n++;
	}
	if (n) { f.avg = sum / n; f.rested = f.hi + pf_carryover_c(rate_at_hi); }
	return f;
}

/* What a step's ending is evaluated against.
 *
 * Small on purpose: a step asks about itself, the grill and the food, and building the whole status
 * once a second to answer that would be waste. The shape matches the status the rules engine reads,
 * so the same trait paths work and a condition means the same thing in both places.
 *
 * `as_if_answered` forces the two signals only a person can give. Evaluating the tree that way says
 * whether everything except the cook is done, which is the moment to tell them so. */
/* How long this step has been doing what it is for. A Hold step's clock starts when the pit
 * reaches the set point, not when the step does: "hold 250 for an hour" is an hour at 250, and a
 * grill that took twenty minutes to climb there has not held it yet. Smoke and the ends of the
 * timeline count from the step's start, having no temperature to arrive at. */
static double step_elapsed(const pf_control *c, double now)
{
	const pf_recipe_step *s = &c->recipe.r.steps[c->recipe.step];
	if (s->mode == PF_MODE_HOLD) return c->recipe.at_temp_since > 0 ? now - c->recipe.at_temp_since : 0;
	return now - c->recipe.step_start;
}

/* seconds until the pit reaches the set point at its current climb, or -1 when it is not climbing */
static double arrival_eta(const pf_control *c)
{
	if (!c->pit_valid || c->recipe.at_temp_since > 0) return 0;
	double gap = c->setpoint_c - c->pit_c;
	if (gap <= 0) return 0;
	return c->pit_rate_c_min > 0.1 ? gap / c->pit_rate_c_min * 60.0 : -1;
}

static cJSON *step_facts(pf_control *c, double now, bool as_if_answered)
{
	/* Celsius throughout, which is what the step's conditions were converted to when the recipe
	 * was loaded. The grill's display unit does not come into it: both sides of every comparison
	 * are in the same unit, and it is the one the control loop already thinks in. */
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "units", "C");
	cJSON_AddStringToObject(o, "mode", pf_mode_name(c->mode));
	cJSON_AddNumberToObject(o, "setpoint", c->setpoint_c);
	cJSON_AddBoolToObject(o, "lid_open", c->lid_open);
	cJSON_AddNumberToObject(o, "cook_elapsed", c->cook_start_wall > 0 ? pf_wall() - c->cook_start_wall : 0);
	cJSON_AddNumberToObject(o, "aiming_s", now - c->aim_since);
	/* the pit, where the grill traits look for it */
	cJSON *probes = cJSON_AddArrayToObject(o, "probes");
	int pi = c->sensors.primary;
	if (pi >= 0 && pi < c->sensors.n && c->sensors.p[pi].valid) {
		cJSON *pp = cJSON_CreateObject();
		cJSON_AddStringToObject(pp, "role", "Primary");
		cJSON_AddNumberToObject(pp, "temp", c->sensors.p[pi].temp_c);
		cJSON_AddItemToArray(probes, pp);
	}
	food_stats f = food_numbers(c);
	cJSON *st = cJSON_AddObjectToObject(o, "step");
	cJSON_AddNumberToObject(st, "elapsed", step_elapsed(c, now));
	if (!isnan(f.hi)) cJSON_AddNumberToObject(st, "food_max", f.hi);
	if (!isnan(f.lo)) cJSON_AddNumberToObject(st, "food_min", f.lo);
	if (!isnan(f.avg)) cJSON_AddNumberToObject(st, "food_avg", f.avg);
	if (!isnan(f.rested)) cJSON_AddNumberToObject(st, "food_rested", f.rested);
	if (!isnan(f.battery)) cJSON_AddNumberToObject(st, "food_battery", f.battery);
	if (f.eta >= 0) cJSON_AddNumberToObject(st, "food_eta", f.eta);
	cJSON_AddBoolToObject(st, "prompt", as_if_answered || c->recipe.prompt_given);
	cJSON_AddBoolToObject(st, "lid", as_if_answered || c->recipe.lid_seen);
	return o;
}

/* Does this step's ending mention the cook at all? If it does there is a button to show and a
 * message to send when everything else is ready; if it does not, the step ends on its own. */
static bool tree_mentions(const cJSON *node, const char *trait)
{
	if (!node) return false;
	const cJSON *kids = cJSON_GetObjectItem((cJSON *)node, "conditions");
	if (cJSON_IsArray(kids)) {
		const cJSON *k;
		cJSON_ArrayForEach(k, kids) if (tree_mentions(k, trait)) return true;
		return false;
	}
	return !strcmp(pf_json_str((cJSON *)node, "trait", ""), trait);
}

/* A step that says nothing about when it ends ends as soon as it has been applied. The evaluator
 * reads an empty group as false, which is right for a half-written rule and wrong here. */
static bool tree_empty(const cJSON *node)
{
	if (!node) return true;
	const cJSON *kids = cJSON_GetObjectItem((cJSON *)node, "conditions");
	if (cJSON_GetObjectItem((cJSON *)node, "trait")) return false;
	return !cJSON_IsArray(kids) || cJSON_GetArraySize((cJSON *)kids) == 0;
}

/* Seconds until the step is due to end, or -1 when nothing can say.
 *
 * A condition can be about anything, so this is deliberately not general: it looks through the tree
 * for the two kinds of term that can be projected forwards -- a time in the step, and a food probe
 * climbing towards a number -- and takes whichever decides. That is what the warning before a step
 * is timed against and what the header's timer shows. Anything else contributes no estimate, which
 * is honest: the grill cannot say when somebody will open the lid. */
static double tree_eta_impl(pf_control *c, const cJSON *node, double now, bool clock_only);
static double tree_eta(pf_control *c, const cJSON *node, double now) { return tree_eta_impl(c, node, now, false); }
/* the step's own clock alone: what a countdown should show, because a probe's estimate wanders
 * with every reading and a number that wanders is not a countdown */
static double tree_clock(pf_control *c, const cJSON *node, double now) { return tree_eta_impl(c, node, now, true); }

static double tree_eta_impl(pf_control *c, const cJSON *node, double now, bool clock_only)
{
	if (!node) return -1;
	const cJSON *kids = cJSON_GetObjectItem((cJSON *)node, "conditions");
	if (cJSON_IsArray(kids)) {
		bool all = strcasecmp(pf_json_str((cJSON *)node, "op", "all"), "any") != 0;
		double best = -1;
		const cJSON *k;
		cJSON_ArrayForEach(k, kids) {
			double e = tree_eta_impl(c, k, now, clock_only);
			if (e < 0) continue;
			/* everything has to happen: the last one decides. any one will do: the first. */
			if (best < 0) best = e;
			else best = all ? fmax(best, e) : fmin(best, e);
		}
		return best;
	}
	const char *tr = pf_json_str((cJSON *)node, "trait", "");
	const char *op = pf_json_str((cJSON *)node, "op", "");
	if (strcmp(op, ">=") && strcmp(op, ">")) return -1;
	double want = pf_json_num((cJSON *)node, "value", 0);
	if (!strcmp(tr, "elapsed")) {
		/* the clock, plus the climb still to make before it starts */
		double left = fmax(0, want - step_elapsed(c, now));
		const pf_recipe_step *st = &c->recipe.r.steps[c->recipe.step];
		if (st->mode == PF_MODE_HOLD && c->recipe.at_temp_since <= 0) {
			double climb = arrival_eta(c);
			if (climb < 0) return -1;
			left += climb;
		}
		return left;
	}
	if (clock_only) return -1;
	if (strcmp(tr, "food_max") && strcmp(tr, "food_min") && strcmp(tr, "food_avg") && strcmp(tr, "food_rested")) return -1;

	double target_c = want;   /* the tree is in Celsius */
	double best = -1;
	for (int i = 0; i < c->sensors.n; i++) {
		const pf_probe_reading *p = &c->sensors.p[i];
		if (p->role != PF_PROBE_FOOD || !p->enabled || !p->in_use || !p->valid) continue;
		const pf_notify_probe *np = pf_notify_find(&c->notify, p->label);
		if (!np) continue;
		double lin[PF_ETA_SAMPLES];
		int n = np->hist_len;
		for (int k = 0; k < n; k++) lin[k] = np->hist[(np->hist_head - n + k + PF_ETA_SAMPLES) % PF_ETA_SAMPLES];
		double e = pf_notify_estimate_eta(lin, n, target_c, 3.0);
		if (e < 0) continue;
		/* the coolest probe decides when all of them have to be there; otherwise the first one */
		if (best < 0) best = e;
		else best = !strcmp(tr, "food_min") ? fmax(best, e) : fmin(best, e);
	}
	return best;
}

static void recipe_begin_step(pf_control *c, double now)
{
	pf_recipe_step *s = &c->recipe.r.steps[c->recipe.step];
	/* a step the cook marked to be skipped is passed over the moment the run reaches it, its
	 * message unsaid */
	if (c->recipe.step < 16 && c->recipe.flags[c->recipe.step] == 2) {
		LOGI(TAG, "recipe '%s' step %d/%d skipped as asked", c->recipe.r.name, c->recipe.step + 1, c->recipe.r.nsteps);
		c->recipe.flags[c->recipe.step] = 0;
		c->recipe.said = true;
		recipe_advance(c, now);
		return;
	}
	c->recipe.step_start = now;
	c->recipe.at_temp_since = 0;
	c->recipe.clock_s = -1;
	c->recipe.triggered = false;
	c->recipe.waiting = false;
	c->recipe.lead_fired = false;
	c->recipe.said = false;
	c->recipe.eta_s = -1;
	c->recipe.last_eval = 0;
	/* The two signals only a person can give, cleared with the step: the lid that was opened to
	 * put the food on must not answer the step that is waiting for it to come off. */
	c->recipe.prompt_given = false;
	c->recipe.lid_seen = false;
	/* When this step ends, parsed once. Its clocks start from here, so a "for ten minutes" inside
	 * it counts from the step beginning rather than from whenever the last one did. */
	cJSON_Delete(c->recipe.ends);
	c->recipe.ends = s->ends[0] ? cJSON_Parse(s->ends) : NULL;
	memset(&c->recipe.clocks, 0, sizeof c->recipe.clocks);
	c->recipe.wants_prompt = tree_mentions(c->recipe.ends, "prompt") || tree_mentions(c->recipe.ends, "lid");
	if (s->setpoint_c > 0) c->setpoint_c = s->setpoint_c;
	c->s_plus = s->s_plus;
	LOGI(TAG, "recipe '%s' step %d/%d: %s", c->recipe.r.name, c->recipe.step + 1, c->recipe.r.nsteps, pf_mode_name(s->mode));
	/* A step's message is what is said when the step ENDS -- the editor labels it so, and the
	 * timeline draws it on the rail between this step and the next. It used to go out as a step
	 * that asks nothing of the cook BEGAN, so a hold whose message was "Shutting down" announced
	 * that as the hold started, an hour before it meant it. For a step that waits on the cook it
	 * goes out when the step is ready for them (Recipe_Step_Done); for every other step,
	 * recipe_advance says it as the step gives way to the next. */
	switch (s->mode) {
	case PF_MODE_STARTUP:
		c->next_mode = c->recipe.step + 1 < c->recipe.r.nsteps ? c->recipe.r.steps[c->recipe.step + 1].mode : c->cfg.after_startup_mode;
		if (c->next_mode != PF_MODE_SMOKE && c->next_mode != PF_MODE_HOLD) c->next_mode = PF_MODE_SMOKE;
		/* A grill that is already lit is not lit again. Running a recipe on a grill that is
		 * already at temperature used to drop it back into Startup and put the igniter on over a
		 * live fire, and cost the cook the twenty minutes it takes to come back up. The step is
		 * satisfied by the fire that is already there, and the run picks up at the next one. */
		if (pf_mode_is_firing(c->mode)) LOGI(TAG, "recipe '%s': the grill is already lit, skipping the startup step", c->recipe.r.name);
		else if (c->mode != PF_MODE_STARTUP) enter_mode(c, PF_MODE_STARTUP, now);
		break;
	case PF_MODE_SMOKE: case PF_MODE_HOLD:
		if (c->mode == PF_MODE_STOP || c->mode == PF_MODE_MONITOR) { c->next_mode = s->mode; enter_mode(c, PF_MODE_STARTUP, now); }
		else if (c->mode == PF_MODE_STARTUP || c->mode == PF_MODE_REIGNITE) c->next_mode = s->mode; /* startup finishes into it */
		else if (c->mode != s->mode) enter_mode(c, s->mode, now);
		else if (s->mode == PF_MODE_HOLD) { c->target_reached = false; c->ctrl_reset_needed = true; }
		break;
	case PF_MODE_SHUTDOWN: enter_mode(c, PF_MODE_SHUTDOWN, now); break;
	case PF_MODE_STOP: enter_mode(c, PF_MODE_STOP, now); break;
	default: break;
	}
}

static void recipe_advance(pf_control *c, double now)
{
	{
		const pf_recipe_step *done = &c->recipe.r.steps[c->recipe.step];
		if (done->message[0] && !c->recipe.said) pf_events_emit("Recipe_Step_Message", c->recipe.r.name, "%s", done->message);
	}
	if (++c->recipe.step >= c->recipe.r.nsteps) {
		pf_events_emit("Recipe_Complete", c->recipe.r.name, "Recipe finished.");
		c->recipe.active = false;
		cJSON_Delete(c->recipe.ends);
		c->recipe.ends = NULL;
		/* A recipe whose last step was Shutdown has put the grill out. One that ends any other way
		 * has handed back a lit grill with nothing left to manage it, which the cook is told about
		 * and asked what to do with, until they answer or the fire is out. */
		c->recipe.left_running = pf_mode_is_firing(c->mode);
		return;
	}
	recipe_begin_step(c, now);
}

/* Asked every tick once a recipe has ended leaving the grill lit. It is a condition, not a moment:
 * it is true for as long as the fire is burning with no recipe behind it, and it ends by itself
 * when the grill goes out -- which is what makes a snooze harmless. Snoozing for an hour on a
 * grill that is shut down twenty minutes later costs nothing, because there is nothing left to
 * come back to. */
static void recipe_aftercare(pf_control *c)
{
	if (!c->recipe.left_running) return;
	if (!pf_mode_is_firing(c->mode)) {
		c->recipe.left_running = false;
		pf_alarms_clear("RECIPE:left_running");
		return;
	}
	char body[256];
	snprintf(body, sizeof body, "%s has finished and did not shut the grill down. It is still in %s.",
	         c->recipe.r.name[0] ? c->recipe.r.name : "The recipe", pf_mode_name(c->mode));
	pf_alarms_raise("RECIPE:left_running", "W11_RECIPE_LEFT_RUNNING", "Grill Still Running",
	                PF_CRIT_HIGH, PF_SINK_ALL, "The grill is still running", body);
	pf_alarms_offer("RECIPE:left_running", "shutdown", 3600);
}

/* A step held at its end by the cook: the run waits as it would for a prompt, and says so once. */
static void recipe_hold_here(pf_control *c, const pf_recipe_step *s)
{
	c->recipe.waiting = true;
	c->recipe.eta_s = -1; c->recipe.clock_s = -1;
	if (!c->recipe.said) {
		c->recipe.said = true;
		pf_events_emit("Recipe_Step_Done", c->recipe.r.name, "%s%sPaused here as you asked - continue when ready.",
		               s->message[0] ? s->message : "", s->message[0] ? " " : "");
	}
}

static void run_recipe(pf_control *c, double now)
{
	if (!c->recipe.active) return;
	if (c->mode == PF_MODE_ERROR) { c->recipe.active = false; return; }
	pf_recipe_step *s = &c->recipe.r.steps[c->recipe.step];

	/* The lid is a moment; a condition needs a fact. Latch it the instant it happens, whatever the
	 * step is doing, so a tree that asks about it has something to read a second later. */
	if (c->lid_open && !c->recipe.lid_seen && tree_mentions(c->recipe.ends, "lid")) {
		c->recipe.lid_seen = true;
		LOGI(TAG, "recipe '%s' step %d: the lid was opened", c->recipe.r.name, c->recipe.step + 1);
	}

	/* The steps that are about the grill's own mode end when the mode does, not on a condition:
	 * "light it" is over when it is lit. Everything else is decided by the step's tree. */
	bool mode_step = s->mode == PF_MODE_STARTUP || s->mode == PF_MODE_SHUTDOWN || s->mode == PF_MODE_STOP;
	if (mode_step) {
		bool trig = s->mode == PF_MODE_STARTUP
			? (c->mode != PF_MODE_STARTUP && c->mode != PF_MODE_REIGNITE && c->mode != PF_MODE_PRIME)
			: s->mode == PF_MODE_SHUTDOWN ? c->mode == PF_MODE_STOP : true;
		if (trig) {
			if (c->recipe.step < 16 && c->recipe.flags[c->recipe.step] == 1) { recipe_hold_here(c, s); return; }
			recipe_advance(c, now);
		}
		return;
	}
	if (c->mode != s->mode) return;   /* still getting into the mode this step runs in */

	/* A step that says nothing about its ending is a setting, and is done once it is applied. */
	if (tree_empty(c->recipe.ends)) {
		if (c->recipe.step < 16 && c->recipe.flags[c->recipe.step] == 1) { recipe_hold_here(c, s); return; }
		recipe_advance(c, now); return;
	}

	/* Once a second is often enough to ask a question whose answers are minutes and degrees, and
	 * it keeps the tick free of building a facts object a hundred times over. */
	if (now - c->recipe.last_eval < 1.0) return;
	c->recipe.last_eval = now;

	/* the clock of a Hold step starts the moment the pit gets there */
	if (s->mode == PF_MODE_HOLD && c->recipe.at_temp_since <= 0 && c->mode == PF_MODE_HOLD && c->target_reached) {
		c->recipe.at_temp_since = now;
		LOGI(TAG, "recipe '%s' step %d: at %.0f C, the clock starts", c->recipe.r.name, c->recipe.step + 1, c->setpoint_c);
	}
	cJSON *facts = step_facts(c, now, false);
	bool done = pf_rules_eval_tree(c->recipe.ends, facts, "step", &c->recipe.clocks, now);
	cJSON_Delete(facts);
	/* the cook asked the run to pause when this step ends: it is done, and it waits for them */
	if (done && c->recipe.step < 16 && c->recipe.flags[c->recipe.step] == 1) { recipe_hold_here(c, s); return; }

	/* Everything except the cook. Asking the tree again with the prompt and the lid forced true
	 * says whether the only thing still missing is the person -- which is the moment to tell them
	 * so, and the moment the button becomes worth showing. It is also what stops "take the ribs
	 * off and wrap them" going out at the start of the three hour smoke rather than at the end. */
	bool ready = done;
	if (!done && c->recipe.wants_prompt) {
		cJSON *as_if = step_facts(c, now, true);
		pf_rules_clocks spare = c->recipe.clocks;   /* asking must not advance the real clocks */
		ready = pf_rules_eval_tree(c->recipe.ends, as_if, "step", &spare, now);
		cJSON_Delete(as_if);
	}
	/* a step the cook marked to continue on its own answers its own prompt the moment the rest
	 * of its ending is satisfied; a lid it also waits for is still waited for */
	if (ready && !done && c->recipe.wants_prompt && !c->recipe.prompt_given
	    && c->recipe.step < 16 && c->recipe.flags[c->recipe.step] == 3) {
		c->recipe.prompt_given = true;
		LOGI(TAG, "recipe '%s' step %d: continued on its own as asked", c->recipe.r.name, c->recipe.step + 1);
		cJSON *again = step_facts(c, now, false);
		done = pf_rules_eval_tree(c->recipe.ends, again, "step", &c->recipe.clocks, now);
		cJSON_Delete(again);
	}
	c->recipe.waiting = ready && !done;

	if (c->recipe.waiting && !c->recipe.said) {
		c->recipe.said = true;
		pf_events_emit("Recipe_Step_Done", c->recipe.r.name, "%s",
		               s->message[0] ? s->message : "This step is done - tap Next to continue.");
	}

	/* How long until it ends, and the warning before it does. Fired once, and only while there is
	 * enough left for it to be worth saying: a warning a few seconds before the thing it warns
	 * about is just the thing itself, twice. */
	c->recipe.eta_s = c->recipe.waiting ? -1 : tree_eta(c, c->recipe.ends, now);
	c->recipe.clock_s = c->recipe.waiting ? -1 : tree_clock(c, c->recipe.ends, now);
	if (!c->recipe.lead_fired && s->lead_s > 0 && c->recipe.eta_s >= 0 && c->recipe.eta_s <= s->lead_s) {
		c->recipe.lead_fired = true;
		pf_events_emit("Recipe_Step_Soon", c->recipe.r.name, "%s",
		               s->lead_message[0] ? s->lead_message : "The next step is coming up.");
		LOGI(TAG, "recipe '%s' step %d: %d min warning", c->recipe.r.name, c->recipe.step + 1,
		     (int)lround(c->recipe.eta_s / 60.0));
	}

	if (done) recipe_advance(c, now);
}

static void run_notify(pf_control *c, double now)
{
	pf_notify_sync(&c->notify, &c->sensors);
	pf_notify_tick(&c->notify, &c->sensors, c->mode, now, c->cfg.units);
	int act = c->notify.pending_action;
	c->notify.pending_action = PF_AFTER_NONE;
	if (act == PF_AFTER_SHUTDOWN && (c->mode == PF_MODE_STARTUP || c->mode == PF_MODE_REIGNITE || c->mode == PF_MODE_SMOKE || c->mode == PF_MODE_HOLD))
		enter_mode(c, PF_MODE_SHUTDOWN, now);
	else if (act == PF_AFTER_KEEPWARM && (c->mode == PF_MODE_SMOKE || c->mode == PF_MODE_HOLD)) {
		c->setpoint_c = c->cfg.keepwarm_c;
		c->s_plus = c->cfg.keepwarm_splus;
		c->target_reached = false;
		if (c->mode == PF_MODE_HOLD) c->ctrl_reset_needed = true;
		else enter_mode(c, PF_MODE_HOLD, now);
	}
	/* mirror targets into the sensor snapshot for status/history */
	for (int i = 0; i < c->sensors.n; i++) {
		const pf_notify_probe *p = pf_notify_find(&c->notify, c->sensors.p[i].label);
		c->sensors.p[i].target_c = p ? p->target_c : 0;
	}
}

/* ------------------------------------------------------------------ per-mode logic */

/* ------------------------------------------------------------------ learning hooks */

static void learn_reset_window(pf_control *c, double now)
{
	c->learn.steady_since = 0;
	c->learn.u_sum = c->learn.pit_sum = c->learn.pit_sq = 0;
	c->learn.n = 0;
	c->learn.last_disturb_t = now;
}

/* called every HOLD cycle with the applied duty */
static void learn_track_steady(pf_control *c, double now)
{
	/* Not while the grill is being measured: a tuning run is a disturbance from end to end, and an
	 * observation taken during one describes the test rather than an ordinary cook. */
	if (!pf_learning_enabled() || c->autotune.active || pf_tuner_active(NULL, NULL, NULL)) return;
	double err = c->pit_c - c->setpoint_c;
	bool calm = fabs(err) < 3.0 && !c->lid_open && now - c->learn.last_disturb_t > 300 && c->saturated >= 0 && c->u_applied > c->cfg.u_min + 0.005;
	if (!calm) { c->learn.steady_since = 0; c->learn.u_sum = c->learn.pit_sum = c->learn.pit_sq = 0; c->learn.n = 0; return; }
	if (c->learn.steady_since == 0) c->learn.steady_since = now;
	c->learn.u_sum += c->u_applied; c->learn.pit_sum += c->pit_c; c->learn.pit_sq += c->pit_c * c->pit_c; c->learn.n++;
	if (now - c->learn.steady_since >= 180 && now - c->learn.last_obs_t >= 300 && c->learn.n >= 3) {
		double mean = c->learn.pit_sum / c->learn.n;
		double var = c->learn.pit_sq / c->learn.n - mean * mean;
		double amb = isnan(c->ambient_c) ? 20 : c->ambient_c;
		pf_learning_observe(c->cops ? c->cops->id : "?", c->setpoint_c, amb, c->learn.u_sum / c->learn.n, sqrt(fmax(0, var)), "");
		c->learn.last_obs_t = now;
		c->learn.u_sum = c->learn.pit_sum = c->learn.pit_sq = 0; c->learn.n = 0;
	}
}

/* Fit the grill as a first-order lag with a dead time, from the capture that just happened.
 *
 * Every approach to a set point is a step test that has already been run: a known duty went in and
 * the pit is on record for what it did about it. So fit the model to THAT, by least squares over
 * the whole capture, rather than reading two points off the curve.
 *
 * The two-point method this replaces took the set point as the final value of the step, which it is
 * not -- the pit only arrives there because the controller backs the feed off. Taking it as the
 * final value understates the gain and, because the 28 and 63 per cent marks are then measured
 * against the wrong total, collapses the time constant. On the simulator it returned 120 s and on a
 * real grill 534 s, where fitting four of that grill's own cooks properly gives 930 to 1110 s with
 * a residual of 4 F. A prediction is only as good as the model under it, so this is the measurement
 * everything else rests on.
 *
 * Cost: the grid below is about 900 candidate models over a few hundred samples, which is a few
 * hundred thousand multiply-adds -- tens of milliseconds, once, at the end of a capture.
 */
static bool fit_plant_from_history(double now, double window_s, double *K_out, double *tau_out, double *theta_out)
{
	const pf_history *h = pf_history_ctrl_view();
	if (!h) return false;
	enum { MAXN = 1200 };
	static double T[MAXN], U[MAXN], Y[MAXN];
	int n = 0;
	/* oldest first, so the simulation below runs forwards */
	for (int i = 0; i < h->len && n < MAXN; i++) {
		const pf_hist_pt *pt = pf_history_at(h, i);
		if (!pt || now - pt->t > window_s) continue;
		if (isnan(pt->pit_c) || isnan(pt->u_applied)) continue;
		T[n] = pt->t; U[n] = pt->u_applied; Y[n] = pt->pit_c; n++;
	}
	if (n < 60) return false;
	double ymin = Y[0], ymax = Y[0];
	for (int i = 1; i < n; i++) { if (Y[i] < ymin) ymin = Y[i]; if (Y[i] > ymax) ymax = Y[i]; }
	if (ymax - ymin < 20.0) return false;            /* not enough of a rise to fit anything to */

	double best_err = 1e30, bK = 0, bTau = 0, bTheta = 0;
	static int dly[MAXN];
	for (double theta = 0; theta <= 240; theta += 15) {
		/* for each sample, the sample one dead time earlier -- computed once per theta */
		int j = 0;
		for (int i = 0; i < n; i++) {
			while (j + 1 < n && T[j + 1] <= T[i] - theta) j++;
			dly[i] = T[j] <= T[i] - theta ? j : 0;
		}
		for (double tau = 180; tau <= 2400; tau += 60) {
			double x = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
			for (int i = 0; i < n; i++) {
				double dt = i ? T[i] - T[i - 1] : 0;
				if (dt > 0 && dt < 120) x += (U[dly[i]] - x) * (dt / tau);
				sx += x; sy += Y[i]; sxx += x * x; sxy += x * Y[i];
			}
			double den = n * sxx - sx * sx;
			if (fabs(den) < 1e-9) continue;
			double K = (n * sxy - sx * sy) / den;       /* C per unit duty */
			if (!(K > 0)) continue;
			double b = (sy - K * sx) / n, err = 0;
			/* second pass for the residual; cheap next to the first */
			x = 0;
			for (int i = 0; i < n; i++) {
				double dt = i ? T[i] - T[i - 1] : 0;
				if (dt > 0 && dt < 120) x += (U[dly[i]] - x) * (dt / tau);
				double r = K * x + b - Y[i];
				err += r * r;
			}
			if (err < best_err) { best_err = err; bK = K; bTau = tau; bTheta = theta; }
		}
	}
	if (!(bK > 0) || !(bTau > 0)) return false;
	double rms = sqrt(best_err / n);
	if (rms > pf_delta_to_c(15, PF_UNITS_F)) return false;   /* the model does not describe this grill */
	/* A model fitted from a loop that was already controlling is only identifiable if the feed
	 * actually went somewhere: when the duty barely moves, or moves only in step with the pit, an
	 * enormous gain with an enormous time constant fits as well as a modest one with a modest
	 * time constant, and the search simply runs to the end of the grid. Both of those are refusals,
	 * not answers -- a wrong model is worse than the previous one, because everything downstream
	 * trusts it. The rise from cold that follows a light is the well-excited case and is where
	 * this fit does its work. */
	if (bTau >= 2400 - 1 || bTheta >= 240 - 1 || bK >= 890) {
		LOGI(TAG, "plant fit ran to the end of its range (K %.0f, tau %.0f, theta %.0f): not identifiable from this capture", bK, bTau, bTheta);
		return false;
	}
	double umin = U[0], umax = U[0];
	for (int i = 1; i < n; i++) { if (U[i] < umin) umin = U[i]; if (U[i] > umax) umax = U[i]; }
	if (umax - umin < 0.15) {
		LOGI(TAG, "the feed hardly moved during this capture (%.2f to %.2f): nothing to fit a model to", umin, umax);
		return false;
	}
	if (theta_out) *theta_out = bTheta < 5 ? 5 : bTheta;
	if (tau_out) *tau_out = bTau;
	if (K_out) *K_out = bK;
	LOGI(TAG, "grill fitted from the last capture: %.0f C per unit feed, time constant %.0f s, dead time %.0f s (residual %.1f C over %d samples)",
	     bK, bTau, bTheta, rms, n);
	return true;
}

/* passive FOPDT: watch the rise from STARTUP entry until the pit first settles near the set point.
 *
 * Only from a cold grill. The two-point method reads a step response, and a step starts from rest:
 * light a barrel that is still 100 degrees warm from the last cook and the rise it makes is the
 * tail of the previous one, fitted as though it were the whole thing. The time constant comes out
 * short, the dead time long, and the model is wrong in a way nothing downstream can detect. Two
 * tuning runs on the same grill disagreed by nearly a factor of two, and one of them had been lit
 * warm. What the grill is cannot depend on how warm it happened to be when somebody pressed
 * start. */
static void learn_rise_begin(pf_control *c, double now)
{
	double amb = isnan(c->ambient_c) ? pf_to_c(70, PF_UNITS_F) : c->ambient_c;
	bool cold = c->pit_valid && c->pit_c < amb + pf_delta_to_c(30, PF_UNITS_F) && c->pit_c < pf_to_c(150, PF_UNITS_F);
	if (pf_learning_enabled() && c->pit_valid && !cold)
		LOGI(TAG, "startup began at %.0f C with ambient %.0f C: too warm to fit the grill's model from it",
		     c->pit_c, amb);
	c->learn.rise_active = pf_learning_enabled() && c->pit_valid && cold;
	c->learn.rise_t0 = 0; c->learn.rise_T0_c = c->pit_c; c->learn.rise_u_sum = 0; c->learn.rise_n = 0;   /* clock starts at ignition, see learn_rise_track */
	c->learn.rise_t28 = c->learn.rise_t63 = c->learn.rise_arrived_t = 0;
	c->learn.rise_sp_c = c->setpoint_c;
	c->learn.rise_from_step = false;
}

/* A step from one held set point to a higher one is the same experiment as the rise from cold, and
 * a cleaner one: the grill starts from a genuine steady state rather than from whatever the fire
 * was doing as it caught, which is the only reason the cold-start fit insists on being cold. Every
 * such step is therefore measured, which is what gives a tuning run -- a walk up through the
 * anchors -- a plant model at each of them rather than only at the first. */
static void learn_step_begin(pf_control *c, double now)
{
	c->learn.rise_active = pf_learning_enabled() && c->pit_valid;
	c->learn.rise_t0 = now;                  /* no ignition to wait for: the step is the start */
	c->learn.rise_T0_c = c->pit_c;
	c->learn.rise_u_sum = 0; c->learn.rise_n = 0;
	c->learn.rise_t28 = c->learn.rise_t63 = c->learn.rise_arrived_t = 0;
	c->learn.rise_sp_c = c->setpoint_c;
	c->learn.rise_from_step = true;
	if (c->learn.rise_active)
		LOGI(TAG, "measuring the grill on the step from %.0f C to %.0f C", c->learn.rise_T0_c, c->setpoint_c);
}

static void learn_rise_track(pf_control *c, double now)
{
	/* Every step up from one held set point to a higher one is a step test, and the grill is
	 * measured on it. What makes it a test is the condition it starts from: the pit had arrived at
	 * the previous set point and had been sitting at it a while, so the whole of what follows
	 * belongs to the step. "Arrived" is ten degrees rather than three on purpose -- a relay limit
	 * cycle swings either side of its target by design, and the one run that walks deliberately
	 * through the set points is a tuning profile, which is exactly where these models are wanted. */
	bool hold = c->mode == PF_MODE_HOLD && c->pit_valid && c->setpoint_c > 0;
	if (hold) {
		if (c->setpoint_c != c->learn.sp_seen_c) {
			bool step = c->learn.sp_seen_c > 0 && c->learn.sp_reached && !c->learn.rise_active
			            && c->learn.sp_since > 0 && now - c->learn.sp_since >= 300.0
			            && c->setpoint_c - c->learn.sp_seen_c >= 20.0;
			c->learn.sp_seen_c = c->setpoint_c;
			c->learn.sp_since = now;
			c->learn.sp_reached = false;
			if (step) learn_step_begin(c, now);
		}
		if (fabs(c->pit_c - c->setpoint_c) <= 10.0) c->learn.sp_reached = true;
	} else if (c->mode != PF_MODE_STARTUP) {
		c->learn.sp_seen_c = 0; c->learn.sp_since = 0; c->learn.sp_reached = false;
	}

	if (!c->learn.rise_active) return;
	/* The set point moved again part way through: the experiment no longer has one answer. */
	if (c->learn.rise_sp_c > 0 && fabs(c->setpoint_c - c->learn.rise_sp_c) > 1.0) { c->learn.rise_active = false; return; }
	if (c->mode == PF_MODE_STOP || c->mode == PF_MODE_ERROR || c->mode == PF_MODE_SHUTDOWN) { c->learn.rise_active = false; return; }
	/* the step test starts when the fire is evidently lit (+3 C over the startup baseline), so the ignition
	 * delay does not masquerade as plant dead time */
	if (c->learn.rise_t0 == 0) { if (c->pit_valid && c->pit_c >= c->learn.rise_T0_c + 3.0) { c->learn.rise_t0 = now; c->learn.rise_T0_c = c->pit_c; } return; }
	if (!c->learn.rise_arrived_t && now - c->learn.rise_t0 > 3600) { c->learn.rise_active = false; return; }
	if (c->mode != PF_MODE_HOLD) return;
	double span = c->setpoint_c - c->learn.rise_T0_c;
	if (span < 20) { c->learn.rise_active = false; return; }
	double frac = (c->pit_c - c->learn.rise_T0_c) / span;
	if (!c->learn.rise_t28 && frac >= 0.283) c->learn.rise_t28 = now - c->learn.rise_t0;
	if (!c->learn.rise_t63 && frac >= 0.632) c->learn.rise_t63 = now - c->learn.rise_t0;
	if (!c->learn.rise_arrived_t && c->learn.rise_t63 && c->learn.rise_t28 && fabs(c->pit_c - c->setpoint_c) < 3.0)
		c->learn.rise_arrived_t = now;
	/* Arriving is not the moment to fit. A rise that stops at the set point is still on the steep
	 * part of the curve: the pit never approached the asymptote it was heading for, so the data
	 * pins down only the ratio K/tau, and the search slides along that ray to whatever end of the
	 * grid it reaches -- a huge gain with a huge time constant fits the same straight climb as a
	 * modest one. What separates them is the hold that follows, where the feed settles to whatever
	 * balances the losses and fixes the static gain outright. So the capture is the rise AND the
	 * first stretch of the hold, fitted together once both exist. */
	if (c->learn.rise_arrived_t && now - c->learn.rise_arrived_t >= 600) {
		double K = 0, tau = 0, theta = 0;
		double since = now - c->learn.rise_t0 + 120;
		if (!fit_plant_from_history(now, since < 300 ? 300 : since, &K, &tau, &theta)) {
			c->learn.rise_active = false;
			return;
		}
		if (tau > 30 && tau < 3600 && K > 0) {
			pf_learning_store_fopdt(K, tau, theta);
			/* and against this set point's library entry, so the prediction runs on the grill as
			 * it behaves HERE rather than on one model stretched across the whole range */
			pf_learning_store_anchor_plant(c->learn.rise_sp_c > 0 ? c->learn.rise_sp_c : c->setpoint_c, K, tau, theta);
			/* Nest-style: hand the fresh plant model straight to the controller so its gains track
			 * the grill -- except during a tuning run. The startup at the head of a run is part of
			 * the measurement, and the model fitted from it was being handed over as a learned
			 * tuning there and then, which is how a single baseline run ended up running numbers it
			 * had never measured, labelled "learned". The run still needs the fit: the relay result
			 * is designed from it. It is the handing over that has to wait for the run to finish. */
			/* A step between set points measures the GRILL, and that is all it is allowed to do.
			 * The plant it fits reaches the controller through the library above, where it belongs
			 * to the set point it was taken at. Designing gains from it as well would have every
			 * set point change during an ordinary cook quietly re-tune the loop from a fit nobody
			 * asked for -- the same ratchet, arriving by a new door. Only the rise from cold, which
			 * is the whole grill from ambient upward and the only measurement a grill with no
			 * tuning library has, is allowed to set gains. */
			if (!c->learn.rise_from_step && pf_learning_enabled() && !pf_tuner_active(NULL, NULL, NULL) && c->cinst && c->cops->apply_tuning) {
				pf_fopdt p = pf_learning_fopdt();
				c->cops->apply_tuning(c->cinst, 0, 0, p.K, p.tau, p.theta);
				event(PF_LVL_INFO, "TUNING_LEARNED", "Controller tuning updated from this startup's plant model");
			}
		}
		c->learn.rise_active = false;
	}
}

/* ------------------------------------------------------------------ relay autotune */

static void autotune_finish(pf_control *c, bool ok, const char *why)
{
	c->autotune.active = false;
	/* The controller comes back with a bumpless reset seeded from the last feed applied -- and the
	 * last feed the relay applied was one half of its swing, not a steady state. Ending on the low
	 * half seeded a negative integral, and with Ti near fifteen minutes the pit then sagged five
	 * degrees for the whole of the next quarter hour: on the simulator the tune that had just been
	 * measured failed its own verification hold for that reason alone, and on the real grill it is
	 * why a fresh tune felt worse than the one before it. What the reset should inherit is the
	 * feed that holds the set point, which is what the run has just measured. */
	double steady = c->autotune.last_load > 0 ? c->autotune.last_load : c->autotune.u_center;
	if (steady > 0) c->u_raw = c->u_applied = pf_clamp(steady, c->cfg.u_min, c->cfg.u_max);
	if (!ok) { pf_events_emit("Autotune_Failed", "Autotune stopped", "%s", why); return; }
	int n = c->autotune.crossings < PF_AT_MAX ? c->autotune.crossings : PF_AT_MAX;
	/* Average the last few complete oscillations. The early ones are the transient on the way into
	 * the limit cycle and describe the starting conditions rather than the plant, so they are left
	 * out; the first half-cycle is not a cycle at all, since the test begins part way through a
	 * swing. */
	double Pu = 0, A = 0; int k = 0;
	for (int i = n - 1; i >= 2 && k < 3; i--) {
		double p, a;
		autotune_cycle(c, i, &p, &a);
		Pu += p; A += a; k++;
	}
	if (k < 2) { pf_events_emit("Autotune_Failed", "Autotune stopped", "Not enough oscillations were captured."); return; }
	Pu /= k; A /= k;
	/* The relay only switches once the error passes the hysteresis band, so the oscillation can
	 * never be smaller than that band. An amplitude at or under it means the readings are not
	 * describing a real swing. */
	/* The describing function divides by sqrt(A^2 - eps^2). At A = 1.05 eps that term is a third of
	 * A and the ultimate gain it reports is inflated threefold; the old guard let exactly that
	 * through, and the result was a proportional band that halved run after run while the grill
	 * itself had not changed. Below twice the band the measurement is not trustworthy and is
	 * refused rather than stored. */
	if (A < c->autotune.hyst_c * 2.0) {
		pf_events_emit("Autotune_Failed", "Autotune stopped",
		               "The swing was %.1f%s against a %.1f%s switching band -- too close to it to measure the gain. Run it again from a settled grill.",
		               pf_delta_from_c(A, c->cfg.units), c->cfg.units == PF_UNITS_C ? "C" : "F",
		               pf_delta_from_c(c->autotune.hyst_c, c->cfg.units), c->cfg.units == PF_UNITS_C ? "C" : "F");
		return;
	}

	/* Was the swing actually centred?
	 *
	 * Every number below reads the limit cycle as though it sat on the set point: the ultimate gain
	 * from its amplitude, the period from its halves. Two things that look like they would answer
	 * this do not. The mean temperature leans high on any grill, because a fire heats faster than a
	 * barrel cools. The ratio of the two halves is set by the same asymmetry and runs to three to
	 * one on a healthy plant.
	 *
	 * What does answer it, whatever the grill: over a full cycle the average feed delivered is the
	 * load, so if the centre is where it should be, each new cycle's average lands back on it. A
	 * cycle that still says the load is somewhere else is a cycle the centring has not caught up
	 * with, and it is sitting beside the set point rather than oscillating about it. A real run was
	 * swinging about 0.339 duty while its cycles averaged 0.267 -- seven points of duty out -- and
	 * returned a proportional band nearly twice what had been holding the grill.
	 *
	 * The centring loop exists to close that gap. This is the check for when it has not. */
	double bias = c->autotune.meas_err_n > 0 ? c->autotune.meas_err_sum / c->autotune.meas_err_n : 0;
	double bias_f = pf_delta_from_c(bias, c->cfg.units);
	double off = c->autotune.last_load > 0 ? fabs(c->autotune.last_load - c->autotune.u_center) : 0;
	/* Unless there is nowhere to put it: a grill holding a low set point sits on its minimum feed,
	 * the low half is clamped there, and no centre can make the cycle symmetric. That is the
	 * actuator, not a mistake, and the ultimate gain is already taken from the swing delivered
	 * rather than the one asked for. */
	bool pinned = c->autotune.u_center - c->autotune.h <= c->cfg.u_min + 0.01;
	if (off > 0.03 && !pinned) {
		pf_events_emit("Autotune_Failed", "Autotune stopped",
		               "The swing was centred on %.0f%% feed while its cycles averaged %.0f%%, so it was sitting beside the set point rather than oscillating about it. Nothing was filed.",
		               c->autotune.u_center * 100, c->autotune.last_load * 100);
		LOGW(TAG, "autotune rejected: centre %.3f against a cycle load of %.3f", c->autotune.u_center, c->autotune.last_load);
		return;
	}

	/* A limit cycle that is still drifting is not a measurement of anything.
	 *
	 * This used to be filed anyway, with "(still drifting)" appended to the message, and it is what
	 * made three runs on an unchanged grill return ultimate gains of 0.069, 0.067 and 0.052: each
	 * one caught whatever the transient happened to look like when the crossing budget ran out.
	 * Drift with no physical cause is worse than no answer, because it is averaged into the library
	 * and quietly widens the band run after run. The crossing budget is generous now, and the run
	 * still has its overall time limit; reaching the end of both without a steady cycle is a result
	 * to refuse, not to record. */
	bool settled = pf_control_autotune_settled(c);
	if (!settled) {
		pf_events_emit("Autotune_Failed", "Autotune stopped",
		               "The oscillation never settled -- its last two cycles still differed by more than a quarter. Nothing was filed. Run it again once the grill is steady.");
		LOGW(TAG, "autotune rejected: the limit cycle had not settled");
		return;
	}
	/* Half the swing the grill really saw. Where nothing was clamped this is the h it was asked
	 * for; where the low half hit the minimum feed it is smaller, and using the requested h there
	 * would overstate the ultimate gain and hand back a proportional band that is too narrow. */
	double h_eff = c->autotune.h;
	if (c->autotune.hi_n > 0 && c->autotune.lo_n > 0) {
		double hi = c->autotune.hi_sum / c->autotune.hi_n, lo = c->autotune.lo_sum / c->autotune.lo_n;
		if (hi - lo > 0.01) h_eff = (hi - lo) / 2.0;
	}
	/* Ultimate gain from the describing function of a relay with hysteresis.
	 *
	 * 4h/pi is the fundamental of a square wave that spends half its period in each state, and a
	 * pellet grill's limit cycle does not: it heats faster than it cools, so the halves run three
	 * to two and further at the extremes. For a two-level relay that holds its high level for a
	 * fraction g of the period, the fundamental is
	 *
	 *     U1 = (2/pi) * (u_hi - u_lo) * sin(pi*g)
	 *
	 * which is exactly 4h/pi at g = 0.5 and falls away either side of it -- six per cent down on
	 * the 39/61 split one of this grill's own cycles ran at. Using the even-split figure on an
	 * uneven cycle overstates the drive the grill actually received and so overstates its gain.
	 *
	 * Projecting onto the real axis with sqrt(A^2 - eps^2) is the ultimate gain the tuning rules
	 * are written against; the hysteresis adds phase lag, and without that projection the gain is
	 * read a little short of the -180 degree crossing. */
	double eps = c->autotune.hyst_c;
	double denom = sqrt(A * A - eps * eps);
	double gamma = 0.5;
	{
		double t_hi = 0, t_lo = 0; int gk = 0;
		/* the same cycles the period and amplitude came from, split into their two halves */
		for (int i = n - 1; i >= 2 && gk < 3; i -= 2, gk++) {
			/* halves[i] is the half that ended at crossing i+1 and ran under the phase that crossing
			 * switched AWAY from, so the last half, halves[n-1], was the high one exactly when the
			 * phase now is low. The parity was written the other way round; sin(pi*g) is symmetric
			 * about a half so the gain never noticed, but the number was still the wrong one. */
			double a = c->autotune.halves[i], b = c->autotune.halves[i - 1];
			bool i_was_high = ((n - i) % 2) == (c->autotune.phase > 0 ? 0 : 1);
			t_hi += i_was_high ? a : b;
			t_lo += i_was_high ? b : a;
		}
		if (t_hi + t_lo > 0) gamma = t_hi / (t_hi + t_lo);
		gamma = pf_clamp(gamma, 0.15, 0.85);
	}
	(void)denom;
	/* Where the relay measured, and where the tuning rules look.
	 *
	 * A relay with hysteresis does not switch at the set point: it waits until the error has passed
	 * eps, so the oscillation it sets up is the one where the grill's phase lag is not 180 degrees
	 * but 180 - asin(eps/A). That is a lower frequency, so a longer period, and a point further up
	 * the Nyquist curve where the gain is higher. The tuning rules want the ultimate point -- the
	 * 180 degree crossing -- and Tyreus-Luyben's Ti and Td scale with the period, so feeding them
	 * the measured period as though it were the ultimate one hands back an integral and a
	 * derivative time too long by the same factor, and a band too wide. On this grill's own run
	 * the hysteresis was 1.2 C against a 3.3 C swing: 22 degrees of phase, a period 24% long and
	 * a gain 22% short. That is a tune that is safe and slow, which is what the cooks have shown.
	 *
	 * The describing function gives the point exactly: |1/G(jw_m)| = 4h sin(pi g)/(pi A). Taking
	 * sqrt(A^2 - eps^2) for A projects it onto the real axis, which is right only if the curve fell
	 * straight down to it from there; on a lag-dominant plant it does not. So the point is taken
	 * as it is and carried along the plant's own curve to the crossing:
	 *
	 *   - with a time constant in hand, exactly for a first-order lag plus dead time: the dead
	 *     time is whatever puts the phase where the relay found it, and the crossing is where that
	 *     dead time takes it to 180;
	 *   - without one, along the asymptote every lag-dominant plant shares near its crossing, where
	 *     the phase is -90 - w*theta and the gain falls as 1/w, so the period shortens and the gain
	 *     rises by the same factor 1 - (2/pi) asin(eps/A).
	 *
	 * Astrom and Hagglund's advice is to keep eps just above the noise for this reason; here the
	 * noise is what sets it, so the correction is made instead. */
	double phi = A > eps ? asin(eps / A) : 0;
	double Ku_m = (2.0 / M_PI) * (2.0 * h_eff) * sin(M_PI * gamma) / A;
	double w_m = 2.0 * M_PI / Pu, w_u = w_m, Ku = Ku_m;
	pf_fopdt m0 = pf_learning_fopdt();
	const char *how = "asymptote";
	if (m0.valid && m0.tau > 0 && w_m * m0.tau > 1.0) {
		double theta_eff = (M_PI - phi - atan(w_m * m0.tau)) / w_m;
		if (theta_eff > 1.0) {
			double lo = w_m, hi = w_m * 3.0;
			for (int i = 0; i < 60; i++) {
				double mid = 0.5 * (lo + hi);
				if (mid * theta_eff + atan(mid * m0.tau) < M_PI) lo = mid; else hi = mid;
			}
			w_u = 0.5 * (lo + hi);
			Ku = Ku_m * sqrt(1 + w_u * m0.tau * w_u * m0.tau) / sqrt(1 + w_m * m0.tau * w_m * m0.tau);
			how = "first-order lag plus dead time";
		}
	}
	if (w_u == w_m) {
		double f = 1.0 - (2.0 / M_PI) * phi;
		if (f < 0.6) f = 0.6;   /* A >= 2 eps is enforced above, so this is 0.67 at worst */
		w_u = w_m / f;
		Ku = Ku_m / f;
	}
	double Pu_m = Pu;
	Pu = 2.0 * M_PI / w_u;
	LOGI(TAG, "autotune: relay found |1/G| %.4f at %.0f s with %.0f deg of hysteresis phase; ultimate point %.4f at %.0f s (%s)",
	     Ku_m, Pu_m, phi * 180 / M_PI, Ku, Pu, how);
	pf_autotune_result r = { .Ku = Ku, .Pu = Pu, .amplitude_c = A,
	                         .load = c->autotune.last_load > 0 ? c->autotune.last_load : c->autotune.u_center };

	/* The tuning comes out of what the relay measured, and nothing else.
	 *
	 * It used to be routed through the three-parameter model: borrow a static gain the relay cannot
	 * see, split the measured phase lag into a time constant and a dead time, then design from
	 * those. SIMC's band is proportional to that dead time, and the split is decided almost
	 * entirely by the period of the limit cycle. Two runs on this grill a day apart measured
	 * periods of 370 s and 603 s -- which the split turned into dead times of 99 s and 168 s, and
	 * bands of 82 F and 150 F, while the relay's own rule put the second run at 93 F. */
	pf_tuning_from_relay(Ku, Pu, &r.PB_c, &r.Ti, &r.Td);
	if (!(r.PB_c > 0) || !(r.Ti > 0)) {
		pf_events_emit("Autotune_Failed", "Autotune stopped", "The oscillation could not be turned into a tuning.");
		return;
	}
	/* The model is still worth having -- the controller looks ahead by the dead time, and the app
	 * shows the grill it measured -- so it is filed as a by-product when the relay and a static
	 * gain can describe one together. It no longer decides the tuning. */
	pf_ff_fit ff = pf_learning_fit();
	pf_fopdt plant = pf_learning_fopdt();
	/* the static gain this hold just measured -- degrees above ambient per unit of the feed that
	 * held them -- ahead of the feed-forward fit's slope, which with few cooks behind it leans
	 * on its prior */
	double K = 0;
	if (r.load > 0 && !isnan(c->ambient_c) && c->setpoint_c - c->ambient_c > 40) K = (c->setpoint_c - c->ambient_c) / r.load;
	if (!(K > 80 && K < 3000)) K = ff.n >= 3 && ff.b > 1e-5 ? 1.0 / ff.b : plant.valid ? plant.K : 0;
	double tau = 0, theta = 0;
	if (K > 0 && pf_plant_from_relay(Ku, Pu, K, &tau, &theta)) {
		pf_learning_store_fopdt(K, tau, theta);
		LOGI(TAG, "relay -> plant: K %.0f C per unit feed (%s), tau %.0f s, theta %.0f s", K,
		     ff.n >= 3 && ff.b > 1e-5 ? "feed-forward" : "startup rise", tau, theta);
	}
	pf_learning_store_autotune(&r);
	/* The cycles' average feed is the load at this set point, measured by design. It is also the
	 * best feed-forward observation this grill will ever produce -- the passive window needs a calm
	 * quarter hour that a tuning run never gives it, which is how a grill could be tuned four times
	 * and still be running the built-in prior. */
	if (r.load > 0) pf_learning_observe(c->cops ? c->cops->id : "", c->setpoint_c,
	                                    isnan(c->ambient_c) ? 20 : c->ambient_c, r.load, A, NULL);
	bool applied = false;
	if (pf_learning_enabled() && c->cinst && c->cops->apply_tuning) {
		/* the oscillation itself, so the controller designs from the measurement rather than from
		 * a model fitted around it */
		pf_fopdt m = pf_learning_fopdt();
		c->cops->apply_tuning(c->cinst, Ku, Pu, m.valid ? m.K : 0, m.valid ? m.tau : 0, m.valid ? m.theta : 0);
		applied = true;
	}
	pf_events_emit("Autotune_Done", "Autotune complete",
	               "Ku %.3f (swing ±%.2f duty), period %.0f s over %d cycle%s%s, amplitude ±%.1f, sitting %.1f from the set point. PB %.0f (%s), Ti %.0f s%s.",
	               Ku, h_eff, Pu, k, k == 1 ? "" : "s", "",
	               pf_delta_from_c(A, c->cfg.units), bias_f, pf_delta_from_c(r.PB_c, c->cfg.units),
	               c->cfg.units == PF_UNITS_C ? "C" : "F", r.Ti,
	               applied ? " - applied to the controller" : " - review under More > Learning");
}

/* The duty this grill actually ran while it was holding, averaged over the last few minutes of
 * history. This is the number the relay has to swing around: an estimate from the feed-forward
 * model is a guess about grills in general, whereas this is a measurement of the one in front of
 * us. NAN when there is not enough steady history to say. */
/* The duty this grill actually holds this set point on. Only samples taken while the pit was at
 * the set point say anything about that: averaging the window regardless of where the pit was took
 * in the climb, and on a cold start to the lowest set point the climb is nearly all of it. The
 * centre then came out far higher than the grill needs, so even the low half of the relay kept
 * feeding the fire and the pit walked away from the set point without ever crossing it. */
/* How much the pit wanders on its own, while it is not being driven anywhere.
 *
 * The relay's hysteresis has to sit above the noise, or the relay switches on noise and measures
 * nothing. It also has to sit well BELOW the oscillation it is about to produce, because the
 * describing function divides by sqrt(A^2 - eps^2): as the swing approaches the band, that term
 * collapses and the ultimate gain it reports runs away. A fixed 1 C band was generous for a settled
 * grill and left no room above it on a well-tuned one. Measure it instead: the standard deviation
 * of the pit about its own mean over the last few minutes of holding, doubled, is the conventional
 * choice and is what the band is set from. */
static double recent_pit_noise(double now, double window_s)
{
	const pf_history *h = pf_history_ctrl_view();
	if (!h) return NAN;
	double sum = 0, sum2 = 0; int n = 0;
	for (int i = h->len - 1; i >= 0; i--) {
		const pf_hist_pt *pt = pf_history_at(h, i);
		if (!pt || now - pt->t > window_s) break;
		if (isnan(pt->pit_c) || pt->setpoint_c <= 0) continue;
		if (fabs(pt->pit_c - pt->setpoint_c) > pf_delta_to_c(12, PF_UNITS_F)) continue;
		sum += pt->pit_c; sum2 += pt->pit_c * pt->pit_c; n++;
	}
	if (n < 30) return NAN;
	double mean = sum / n, var = sum2 / n - mean * mean;
	return var > 0 ? sqrt(var) : 0;
}


/* How long the pit has been within the band around the set point, from the history: the time
 * since it was last outside it. A grill that arrived a minute ago is still carrying the controller's
 * arrival transient in its feed, and a relay centred on that transient starts a long way off. */
static double hold_in_band_s(const pf_control *c, double now)
{
	const pf_history *h = pf_history_ctrl_view();
	if (!h) return 0;
	/* The same band the profile runner calls "close", and the same one the relay is allowed to
	 * start inside: a grill can be holding steadily a few degrees off its set point -- the
	 * simulator's plain PID sits nine degrees hot at 225 with the auger on its minimum -- and
	 * that is a settled hold whose feed is the load, not a transient. */
	double band = pf_delta_to_c(15, PF_UNITS_F), since = now;
	for (int i = h->len - 1; i >= 0; i--) {
		const pf_hist_pt *pt = pf_history_at(h, i);
		if (!pt) break;
		if (isnan(pt->pit_c) || pt->setpoint_c <= 0 || fabs(pt->pit_c - pt->setpoint_c) > band) break;
		since = pt->t;
	}
	(void)c;
	return now - since;
}

/* The feed a settled hold has been delivering: the plain mean over the last `window_s`, every
 * sample counted, because by the time this is asked the whole window is inside the band. */
static double settled_hold_duty(const pf_control *c, double now, double window_s)
{
	const pf_history *h = pf_history_ctrl_view();
	if (!h) return NAN;
	double sum = 0; int n = 0;
	for (int i = h->len - 1; i >= 0; i--) {
		const pf_hist_pt *pt = pf_history_at(h, i);
		if (!pt || now - pt->t > window_s) break;
		if (isnan(pt->u_applied) || pt->u_applied <= 0) continue;
		sum += pt->u_applied; n++;
	}
	(void)c;
	return n >= 30 ? sum / n : NAN;
}

static void autotune_start(pf_control *c, double now)
{
	/* Near the set point is enough to begin; the relay drives the pit across it from wherever it
	 * starts. Demanding that it had already touched the target meant a controller tuned too slowly
	 * to quite get there could never run the measurement that would retune it. */
	double at_band = pf_delta_to_c(15, PF_UNITS_F);
	if (c->mode != PF_MODE_HOLD || !c->pit_valid || c->setpoint_c <= 0 || fabs(c->pit_c - c->setpoint_c) > at_band) {
		pf_events_emit("Autotune_Failed", "Autotune not started",
		               "Hold near the set point first: the pit has to be within about 15 degrees of it.");
		return;
	}
	memset(&c->autotune, 0, sizeof c->autotune);
	c->autotune.active = true;
	/* Centre the swing on what the grill has been doing, falling back to the model and then to the
	 * last duty. Centring on a figure that is too high means the low half of the relay still heats,
	 * the pit never comes back down through the set point, and the test times out having learned
	 * nothing: that is exactly what an unlearned feed-forward did on a real grill at 225 F. */
	/* Half an hour, because only the samples taken at the set point count and a grill that has
	 * just arrived there has not yet produced many. */
	/* Where the swing starts decides how the first cycles go, and the first cycles are what a
	 * cook watches. On this grill a run centred on 90 s of the controller's arrival -- feed going
	 * 0.14, 0.48 as it caught the set point -- began at 0.328 against a real load near 0.22, put
	 * the pit 9 F over on its first swing and well under on its second, and spent two centrings
	 * finding its way back. What the grill needs to hold a set point does not depend on how it
	 * arrived there today, so the first choice is the load the last run at this set point
	 * measured; failing that, the feed of a hold that has genuinely settled -- five minutes inside
	 * the band, not ninety seconds; and failing that the run does not start. */
	double known = pf_learning_anchor_load(c->setpoint_c);
	double settled_s = hold_in_band_s(c, now);
	double measured = settled_s >= 300 ? settled_hold_duty(c, now, fmin(settled_s, 1800)) : NAN;
	const char *from;
	double centre;
	if (known > 0) { centre = known; from = "the last run at this set point"; }
	else if (!isnan(measured)) { centre = measured; from = "the settled hold"; }
	else {
		c->autotune.active = false;
		pf_events_emit("Autotune_Failed", "Autotune not started",
		               "Hold at the set point for five minutes first, so the swing can start from what the grill actually needs there.");
		return;
	}
	c->autotune.u_center = pf_clamp(centre, c->cfg.u_min + 0.05, c->cfg.u_max - 0.05);
	LOGI(TAG, "autotune: centring on %.3f feed from %s (hold settled %.0f s)", c->autotune.u_center, from, settled_s);

	/* Size the swing to fit between the minimum and maximum feed. The relay's maths assumes a
	 * symmetric square wave about the centre, and a grill that holds a low set point on very
	 * little fuel has almost no room below it: asking for a swing that gets clamped on one side
	 * gives a lopsided input and an ultimate gain that is too high. Shrink the swing instead, and
	 * only if that leaves too little to measure, move the centre up to make room. */
	c->autotune.h_cap = 0; c->autotune.h_floor = 0; c->autotune.resizes = 0;   /* this run has not yet learned what swing it needs */
	pf_control_autotune_size(c);
	/* Two sigma of the pit's own wander, kept inside sane bounds: small enough that the swing this
	 * run produces stands well clear of it, large enough that the relay does not chase noise. */
	double sigma = recent_pit_noise(now, 600);
	c->autotune.hyst_c = pf_clamp(isnan(sigma) ? 1.0 : 2.0 * sigma, 0.35, 1.2);
	c->autotune.start_t = now;
	c->autotune.last_cross_t = now;
	/* Start in the half that opposes what the pit is already doing.
	 *
	 * The relay begins the moment the pit arrives at the set point, which it does climbing. Choosing
	 * the first half from the sign of the error then starts it feeding, and the fresh feed lands on
	 * top of the momentum the pit already has: on this grill the first half lasted fifteen seconds
	 * and the first excursion reached eleven degrees, against five and a half once the cycle
	 * settled. Sitting at the target with a rising pit, the half worth running is the low one. The
	 * error still decides when the pit is plainly off the set point; the rate decides when it is
	 * not. */
	double e0 = c->pit_c - c->setpoint_c;
	c->autotune.phase = fabs(e0) > 2.0 ? (e0 > 0 ? -1 : +1)
	                  : (c->pit_rate_c_min > 0.2 ? -1 : c->pit_rate_c_min < -0.2 ? +1 : (e0 > 0 ? -1 : +1));
	c->autotune.err_at_move = c->pit_c - c->setpoint_c;
	c->autotune.peak_max = c->autotune.peak_min = c->pit_c;
	pf_events_emit("Autotune_Started", "Autotune running", "The grill will oscillate a few degrees around %.0f for 15-40 minutes. Do not cook food during the test.", pf_from_c(c->setpoint_c, c->cfg.units));
	pf_cycle_begin(&c->cycle, &c->ccfg, now, autotune_output(c));
	c->u_raw = c->u_applied = c->cycle.u_applied;
}

/* One full oscillation, numbered by its closing half-cycle: its period is the two halves together
 * and its amplitude spans both, because the pit's high and low peaks fall in different halves. */
static void autotune_cycle(const pf_control *c, int i, double *period, double *amp)
{
	double hi = fmax(c->autotune.hi_peak[i], c->autotune.hi_peak[i - 1]);
	double lo = fmin(c->autotune.lo_peak[i], c->autotune.lo_peak[i - 1]);
	if (period) *period = c->autotune.halves[i] + c->autotune.halves[i - 1];
	if (amp) *amp = (hi - lo) / 2.0;
}

/* Has the limit cycle settled? Two consecutive oscillations that agree in period and amplitude are
 * the standard evidence that what is being measured is the plant rather than the transient. */
bool pf_control_autotune_settled(const pf_control *c)
{
	int n = c->autotune.crossings < PF_AT_MAX ? c->autotune.crossings : PF_AT_MAX;
	/* Two DISJOINT cycles, not two overlapping windows.
	 *
	 * autotune_cycle(i) is halves[i] and halves[i-1], so cycle(n-2) and cycle(n-1) share a half
	 * between them -- and two sums that share one of their two terms agree almost whatever the
	 * grill is doing. On a real run whose consecutive cycles were 498 s and 362 s, twenty-seven
	 * per cent apart, this pair came out four per cent apart and the run called itself settled.
	 * A test that cannot fail is not a test, and it is why three runs on an unchanged grill filed
	 * ultimate gains of 0.069, 0.067 and 0.052: each one accepted a transient.
	 *
	 * The last cycle is halves[n-1] and halves[n-2]; the one before it is halves[n-3] and
	 * halves[n-4], which needs five crossings to exist. */
	if (n < 5) return false;
	double p1, a1, p2, a2;
	autotune_cycle(c, n - 1, &p1, &a1);
	autotune_cycle(c, n - 3, &p2, &a2);
	double pmax = fmax(p1, p2), amax = fmax(a1, a2);
	if (pmax <= 0 || amax <= 0) return false;
	return fabs(p1 - p2) <= 0.25 * pmax && fabs(a1 - a2) <= 0.30 * amax;
}

/* the feed this half of the swing asks for: up from the centre, or down from it */
static double autotune_output(const pf_control *c)
{
	return c->autotune.u_center + c->autotune.h * c->autotune.phase;
}

/* Size the swing around the centre -- the same step up as down.
 *
 * Stepping up harder than down was tried, to get more authority on a grill holding near its minimum
 * feed, and it costs the one property the whole test depends on: an uneven relay produces an uneven
 * limit cycle, whose mean sits off the set point even when the centre is exactly the load. Measured
 * in the simulator it left the swing averaging 255 F on a 250 F set point, which is the same three
 * to five degrees of bias that stretched a real run's period and doubled the band it returned. A
 * symmetric swing about the right centre averages out on the set point, which is what makes the
 * period and the amplitude describe the grill there. At a low set point there is little room below
 * the centre and the swing is small and slow; that is the honest price, and slow is recoverable
 * where biased is not. */
void pf_control_autotune_size(pf_control *c)
{
	/* No wider than the load can bear. A swing of +/-0.15 on a grill holding 250 F on 0.22 duty
	 * nearly doubles the fire on the high half and starves it on the low, which is the 9 F
	 * excursion this grill's own runs showed; the describing function wants a swing the plant
	 * answers linearly. Half the load either way is plenty to measure and stays inside that, and
	 * the widening below still grows it if the pit's answer turns out too small to read. */
	double h = fmin(0.15, fmin(0.55 * c->autotune.u_center, fmin(c->autotune.u_center - c->cfg.u_min, c->cfg.u_max - c->autotune.u_center)));
	if (h < 0.03) h = 0.03;
	if (c->autotune.h_cap > 0 && h > c->autotune.h_cap) h = c->autotune.h_cap;
	/* A swing grown on purpose is kept, as far as the room allows. Re-centring used to undo it
	 * silently -- this run widened to 0.165 and the very next centring put it back to 0.131 -- so
	 * the widening bought nothing and the oscillation stayed as thin as it had been. */
	if (c->autotune.h_floor > 0 && h < c->autotune.h_floor) {
		double room = fmin(c->autotune.u_center - c->cfg.u_min, c->cfg.u_max - c->autotune.u_center);
		h = fmin(c->autotune.h_floor, fmax(room, h));
	}
	c->autotune.h = h;
}

/* returns the relay output for this cycle */
static double autotune_step(pf_control *c, double now)
{
	double e = c->pit_c - c->setpoint_c;
	if (c->pit_c > c->autotune.peak_max) c->autotune.peak_max = c->pit_c;
	if (c->pit_c < c->autotune.peak_min) c->autotune.peak_min = c->pit_c;
	int want = c->autotune.phase;
	if (c->autotune.phase > 0 && e > c->autotune.hyst_c) want = -1;
	if (c->autotune.phase < 0 && e < -c->autotune.hyst_c) want = +1;
	if (want != c->autotune.phase) {
		c->autotune.phase = want;
		int k = c->autotune.crossings;
		if (k < PF_AT_MAX) {
			c->autotune.halves[k] = now - c->autotune.last_cross_t;
			c->autotune.hi_peak[k] = c->autotune.peak_max;
			c->autotune.lo_peak[k] = c->autotune.peak_min;
		}
		c->autotune.crossings++;
		LOGI(TAG, "autotune crossing %d: that half took %.0f s, pit %.1f to %.1f C, feed %.3f",
		     c->autotune.crossings, now - c->autotune.last_cross_t, c->autotune.peak_min, c->autotune.peak_max,
		     autotune_output(c));
		c->autotune.last_cross_t = now;
		c->autotune.peak_max = c->autotune.peak_min = c->pit_c;

		/* Condition the relay first, then measure with it.
		 *
		 * A relay test is only as good as the limit cycle it produces, and the cycle is only about
		 * the grill if it is centred on the set point and no bigger than it needs to be. Two things
		 * are therefore corrected from each completed cycle, before anything is measured:
		 *
		 *   The CENTRE. Over one full cycle the average feed delivered is the load the grill needs
		 *   at that temperature, whatever the centre was set to, so the centre moves to that
		 *   average. Feed a little too much and the pit lives above the set point, coming down only
		 *   on the low half: the halves stop being equal, the period stretches and the swing
		 *   widens, all of which the describing function reads as a grill that answers feed weakly.
		 *   A real run did exactly this -- 20 minutes above the set point against 8 below, peaks of
		 *   +12.9 and -5.7 F, a period two thirds longer than the same grill measured a day
		 *   earlier, and a band of 150 F where 82 F had been holding it.
		 *
		 *   The SWING. For a relay the oscillation is proportional to h, so h is scaled to land the
		 *   amplitude on a target a few times the hysteresis band: wide enough to measure against
		 *   the noise, narrow enough that the grill is barely disturbed and the food, if any, does
		 *   not care.
		 *
		 * Cycles recorded before a material change belong to a different experiment and are thrown
		 * away. The conditioning is capped so the test always terminates, and everything measured
		 * afterwards comes from one relay, centred, on the set point. */
		double A_target = fmax(3.0 * c->autotune.hyst_c, pf_delta_to_c(2.5, PF_UNITS_F));
		/* Conditioning belongs to the start of the run: after this the relay is left alone and what
		 * it does is the measurement. */
		/* Never from the first half. The run begins wherever the pit happens to be, part way
		 * through a swing, so the first half is whatever was left of it: on this grill at 250 F
		 * it was 90 s of the low feed, begun with the pit falling from 4.7 C over, against a full
		 * 166 s of the high feed. Read as a cycle that said the load was 0.306 on a grill whose
		 * settled hold had just been feeding 0.263, the centre stepped the wrong way, the next
		 * swing threw the pit 6 C over, and two cycles went on finding the way back. The first
		 * cycle that is a cycle is the second and third halves, so the conditioning starts at
		 * the third crossing and runs on the odd ones. */
		if (c->autotune.crossings >= 3 && c->autotune.crossings <= 9 &&
		    (c->autotune.crossings % 2) == 1 && c->autotune.cyc_n > 4) {
			int nx = c->autotune.crossings;
			double t_hi = c->autotune.halves[nx - 1], t_lo = c->autotune.halves[nx - 2];
			if (c->autotune.phase > 0) { double sw = t_hi; t_hi = t_lo; t_lo = sw; }
			/* The load is what the last complete CYCLE delivered, worked out from the two halves it
			 * was made of: the relay's two feeds, each weighted by how long that half lasted. A
			 * plain average of every tick since the window opened was doing this before, and it
			 * carried whatever the grill was doing before the oscillation began -- most of an
			 * approach at one end of the swing -- into the answer. One run came out of that with a
			 * centre of 0.20 on a grill whose load was 0.29, which put the low half on the feed
			 * floor, and from there no cycle could complete at all. */
			double u_hi = pf_clamp(c->autotune.u_center + c->autotune.h, c->cfg.u_min, c->cfg.u_max);
			double u_lo = pf_clamp(c->autotune.u_center - c->autotune.h, c->cfg.u_min, c->cfg.u_max);
			double load = t_hi + t_lo > 0 ? (t_hi * u_hi + t_lo * u_lo) / (t_hi + t_lo)
			                              : c->autotune.cyc_sum / c->autotune.cyc_n;
			c->autotune.last_load = load;
			if (t_hi > 0 && t_lo > 0) {
				double split = t_hi > t_lo ? t_hi / t_lo : t_lo / t_hi;
				if (split > c->autotune.worst_split) c->autotune.worst_split = split;
			}
			double top = fmax(c->autotune.hi_peak[nx - 1], c->autotune.hi_peak[nx - 2]);
			double bot = fmin(c->autotune.lo_peak[nx - 1], c->autotune.lo_peak[nx - 2]);
			double A = (top - bot) / 2.0;

			/* The first full cycle already answers the question -- its average feed is the load --
			 * so that correction is taken whole. Every later one is a trim, capped at half the
			 * swing so a single noisy cycle cannot move the experiment far, and each one costs two
			 * more cycles before the result may be read. */
			double aim = pf_clamp(load, c->cfg.u_min + 0.02, c->cfg.u_max - 0.02);
			double lim = c->autotune.adjusts == 0 ? 1.0 : c->autotune.h / 2;
			double c_step = pf_clamp(aim - c->autotune.u_center, -lim, lim);
			double centre = pf_clamp(c->autotune.u_center + c_step, c->cfg.u_min + 0.02, c->cfg.u_max - 0.02);
			/* The swing is never trimmed DOWN. A smaller swing is a relay with less authority,
			 * and one whose low half no longer cools the grill does not oscillate at all, it just
			 * sits above the set point -- which is the failure this whole exercise is about.
			 *
			 * It is grown when the oscillation comes out too small to read, which is the fault that
			 * made a well-tuned grill tune itself worse: the better it held, the less room the
			 * centre left for the swing, the smaller the swing, and the closer the amplitude came
			 * to the switching band -- where the describing function divides by very little and
			 * hands back an ultimate gain far larger than the grill's. Growing it early, while the
			 * run is still conditioning itself, costs a couple of cycles and fixes the measurement
			 * rather than failing it an hour later. */
			/* Sizing the swing and centring it are two different jobs, and a run gets its own
			 * allowance of each. Charging a resize to the centring budget -- as this did at first
			 * -- meant a run that had to narrow a wild swing twice arrived at the measurement with
			 * the centre still wherever it started, which is the one thing that must not happen. */
			/* A resize throws the previous swing away, but the two halves this amplitude was read
			 * from straddle the change: the first cycle after it still carries the old swing's
			 * decay and reads far too big. Judging the new swing on that cycle narrowed a
			 * reasonable 0.05 down to the floor and left a stretched, off-centre limit cycle that
			 * understated the grill's gain threefold. So a resize waits two whole cycles for the
			 * amplitude it is being judged on to be the new swing's own. */
			bool settled = c->autotune.resizes == 0 || c->autotune.crossings >= c->autotune.adjust_at_cross + 4;
			bool thin = A > 0 && A < A_target && c->autotune.resizes < 2 && settled;
			bool widened = false;
			/* And the other way about. The swing is sized from the room either side of the centre,
			 * which says nothing about what it does to this grill: on a well-fed cooker the biggest
			 * swing the clamps allow can throw the pit twenty degrees either way when two or three
			 * would measure it. That is not merely rude to the grill. A pellet cooker heats faster
			 * than it cools, so the wider the swing the more the cycle leans upward, and a limit
			 * cycle sitting above the set point is read by the describing function as a grill that
			 * answers feed weakly -- the very error the centring above exists to prevent. So a
			 * swing that comes back far larger than the measurement needs is brought down toward
			 * it, and capped there so re-centring cannot inflate it again. */
			bool fat = !thin && A > 2.0 * A_target && c->autotune.resizes < 2 && settled;
			if (fat) {
				double target_h = c->autotune.h * pf_clamp(A_target / A, 0.35, 1.0);
				/* Never below a swing the grill can actually deliver: the auger runs whole seconds
				 * out of a cycle, so a swing finer than one of those is not a square wave at all,
				 * and the describing function is being asked about an input that never happened. */
				double floor_h = fmax(0.05, c->ccfg.cycle_s > 0 ? 1.0 / c->ccfg.cycle_s : 0.05);
				double shrunk = fmax(target_h, floor_h);
				if (shrunk < c->autotune.h * 0.95) {
					LOGI(TAG, "autotune swing %.3f threw the pit %.1f C about a %.1f C measurement; narrowing to %.3f",
					     c->autotune.h, A, A_target, shrunk);
					c->autotune.h = shrunk;
					c->autotune.h_cap = shrunk;
					c->autotune.resizes++;
					c->autotune.adjust_at_cross = c->autotune.crossings;
					c->autotune.hi_sum = c->autotune.lo_sum = 0; c->autotune.hi_n = c->autotune.lo_n = 0;
					c->autotune.meas_err_sum = 0; c->autotune.meas_err_n = 0;
					c->autotune.cyc_sum = 0; c->autotune.cyc_n = 0;
					widened = true;
				}
			}
			if (thin) {
				double target_h = c->autotune.h * pf_clamp(A_target / A, 1.0, 2.0);
				double room = fmin(c->cfg.u_max - 0.02 - c->autotune.u_center,
				                   c->autotune.u_center - c->cfg.u_min - 0.02);
				double grown = pf_clamp(target_h, c->autotune.h, fmax(room, c->autotune.h));
				if (grown > c->autotune.h * 1.05) {
					LOGI(TAG, "autotune swing %.3f gave only %.2f C against a %.2f C band; widening to %.3f",
					     c->autotune.h, A, c->autotune.hyst_c, grown);
					c->autotune.h = grown;
					c->autotune.h_floor = grown;
					c->autotune.resizes++;
					c->autotune.adjust_at_cross = c->autotune.crossings;
					c->autotune.hi_sum = c->autotune.lo_sum = 0; c->autotune.hi_n = c->autotune.lo_n = 0;
					c->autotune.meas_err_sum = 0; c->autotune.meas_err_n = 0;
					c->autotune.cyc_sum = 0; c->autotune.cyc_n = 0;
					widened = true;
				}
			}
			/* A run that has just widened its swing has thrown its window away; the centring below
			 * belongs to the next one. */
			if (!widened) {
			bool material = fabs(c_step) > 0.15 * c->autotune.h;
			/* A cap of two centrings was a budget, not a test of anything. A pellet grill relights
			 * slower than it starves, so its limit cycle is asymmetric and the first centring
			 * lands past the answer as often as short of it: tonight's run went 0.328, 0.263,
			 * 0.292 and was then forbidden a fourth move, leaving it to finish uncentred or grind
			 * to the crossing cap. What decides whether another centring is worth making is
			 * whether they are converging: each step smaller than the last. A run that is homing
			 * in may keep going, within reason; one that is wandering is stopped where it was. */
			bool converging = c->autotune.adjusts < 2 || fabs(c_step) < 0.8 * fabs(c->autotune.last_c_step);
			if (material && converging && c->autotune.adjusts < 5) {
				c->autotune.last_c_step = c_step;
				c->autotune.u_center = centre;
				pf_control_autotune_size(c);
				c->autotune.adjusts++;
				/* Nothing recorded is thrown away -- the run is slow enough that starting the count
				 * again would spend an hour -- but the result may not be taken from cycles that
				 * straddle the change, so the finish waits for two whole cycles after it. */
				c->autotune.adjust_at_cross = c->autotune.crossings;
				c->autotune.hi_sum = c->autotune.lo_sum = 0; c->autotune.hi_n = c->autotune.lo_n = 0;
				c->autotune.meas_err_sum = 0; c->autotune.meas_err_n = 0;
				LOGI(TAG, "autotune centring %d: the cycle averaged %.3f feed and swung %.1f C; centre %.3f, swing +/-%.3f",
				     c->autotune.adjusts, load, A, c->autotune.u_center, c->autotune.h);
			} else if (fabs(c_step) > 0.003) {
				c->autotune.u_center = centre;   /* a trim this small leaves the record standing */
			}
			}
			c->autotune.cyc_sum = 0; c->autotune.cyc_n = 0;
		}

		/* Stop once the oscillation has settled, not merely once enough of it has gone by. A limit
		 * cycle that is still growing describes the transient, not the plant, and averaging it
		 * yields a period that belongs to no real oscillation. The cycles that follow the last
		 * centring are the measurement, so two whole cycles -- four crossings -- are required before any
		 * of it counts. One cycle was what the code asked for while this comment asked for two, and
		 * a single cycle taken straight after a centre moved is still half transient. */
		/* A limit cycle is only the measurement if it sits ON the set point. An off-centre cycle
		 * stretches its period and widens its swing, and the describing function reads that as a
		 * grill that answers feed weakly. The last run finished sitting three degrees high and came
		 * back with a band five per cent wider than the run before it; the centring trim was still
		 * working when the run declared itself done. So being centred is a condition of finishing,
		 * not something hoped for along the way -- with the crossing limit still there to end a run
		 * that cannot manage it. */
		double bias_now = c->autotune.meas_err_n > 0 ? fabs(c->autotune.meas_err_sum / c->autotune.meas_err_n) : 0;
		bool centred = bias_now <= pf_delta_to_c(2.0, PF_UNITS_F) || c->autotune.meas_err_n < 4;
		if (c->autotune.crossings >= PF_AT_MIN_CROSS &&
		    c->autotune.crossings >= c->autotune.adjust_at_cross + 4 &&
		    (((pf_control_autotune_settled(c) && centred)) || c->autotune.crossings >= PF_AT_MAX)) {
			autotune_finish(c, true, "");
			return c->autotune.u_center;
		}
	}

	/* Two ways the swing can be centred wrong, and both say the same thing. The pit runs far past
	 * the set point because even the low half of the relay is still feeding the fire, or a
	 * half-cycle simply never ends because the pit is parked on one side. Rather than give up,
	 * move the centre towards the side it is failing on and start the measurement over, so every
	 * recorded cycle comes from one centre. */
	bool ran_away = fabs(e) > pf_delta_to_c(50, PF_UNITS_F);
	/* A swing that has never crossed at this centre is stuck and worth moving early. Once it has
	 * crossed even once the centre is evidently workable, so be patient: a pellet grill's cooling
	 * half is genuinely slow, ten minutes on a real one at 180 F, and treating that as a stall
	 * resets the count on every down-swing and the test can never finish. */
	double patience = c->autotune.crossings == 0 ? 600.0 : 1200.0;
	bool stalled = now - c->autotune.last_cross_t > patience;
	/* Before the first crossing there is no evidence this centre works at all. A pit that is not
	 * merely sitting off the set point but steadily walking further from it has already answered
	 * the question, and waiting the full patience out just spends the budget climbing. */
	bool drifting = c->autotune.crossings == 0 &&
	                fabs(e) > fabs(c->autotune.err_at_move) + pf_delta_to_c(10, PF_UNITS_F);
	/* Give the grill time to answer the last move before judging it again, or a runaway fires on
	 * consecutive cycles and spends the whole budget in seconds without the pit having moved. */
	bool settled_since_move = now - c->autotune.last_recentre_t > 300;
	if ((ran_away || stalled || drifting) && settled_since_move && c->autotune.recentres < 8) {
		/* A runaway says the centre is a long way out, so move further than a mere stall does.
		 * The budget has to be big enough to walk in from a bad starting guess and still leave
		 * room to measure; the overall time limits are what stop a hopeless case. */
		double step = ran_away ? 0.15 : drifting ? 0.10 : 0.05;
		double moved = pf_clamp(c->autotune.u_center + (e > 0 ? -step : step),
		                        c->cfg.u_min + 0.05, c->cfg.u_max - 0.05);
		if (fabs(moved - c->autotune.u_center) < 1e-6) {
			/* already as far over as the feed limits allow: there is nothing to move, so wait for
			 * the pit to come back rather than spending the budget on a centre that cannot change */
			c->autotune.last_recentre_t = now;
			c->autotune.err_at_move = e;
			return autotune_output(c);
		}
		c->autotune.u_center = moved;
		pf_control_autotune_size(c);
		c->autotune.recentres++;
		c->autotune.err_at_move = e;
		c->autotune.last_recentre_t = now;
		c->autotune.crossings = 0;
		c->autotune.hi_sum = c->autotune.lo_sum = 0; c->autotune.hi_n = c->autotune.lo_n = 0;
		c->autotune.last_cross_t = now;
		c->autotune.phase = c->pit_c > c->setpoint_c ? -1 : +1;
		c->autotune.peak_max = c->autotune.peak_min = c->pit_c;
		LOGW(TAG, "autotune: %s after %.0f s, re-centring the swing on %.2f duty (%d)",
		     ran_away ? "pit ran away from the set point" : "no crossing", now - c->autotune.last_cross_t,
		     c->autotune.u_center, c->autotune.recentres);
		return autotune_output(c);
	}
	/* Only give up on a runaway once there is nothing left to try. Falling out of the block above
	 * merely because the grill has not had time to answer the last move is not a failure. */
	if (ran_away && c->autotune.recentres >= 8) { autotune_finish(c, false, "The pit would not stay near the set point."); return c->cfg.u_min; }
	if (now - c->autotune.last_cross_t > 1800) { autotune_finish(c, false, "The grill would not oscillate around the set point."); return c->autotune.u_center; }
	return autotune_output(c);
}

static void run_hold_cycle(pf_control *c, double now)
{
	if (!pf_cycle_done(&c->cycle, now)) return;
	double u;
	if (c->lid_open || !c->cinst) {
		u = c->cfg.u_min;
	} else if (c->autotune.active) {
		u = autotune_step(c, now);
		c->ctrl_reset_needed = true;
	} else {
		int nobs = 0;
		c->learn.u_ff = pf_learning_uff(c->setpoint_c, isnan(c->ambient_c) ? 20 : c->ambient_c, c->cfg.u_min, c->cfg.u_max, &nobs);
		/* The tuning library, if it has anything measured at this set point -- and if it is still
		 * being trusted. The library takes priority over the numbers typed into the controller,
		 * which is right while it is the better evidence and wrong the moment somebody writes
		 * values down and types them in expecting them to be used: they were overridden in silence
		 * by a measurement they had never seen. Turning the library off is how you say "use what I
		 * typed", which is also what makes the three numbers portable to another grill. */
		double sched_PB = 0, sched_Ti = 0, sched_Td = 0, sched_K = 0, sched_tau = 0, sched_theta = 0;
		if (c->cfg.use_library) pf_learning_gains(c->setpoint_c, &sched_PB, &sched_Ti, &sched_Td);
		pf_learning_plant(c->setpoint_c, &sched_K, &sched_tau, &sched_theta);
		pf_ctrl_in in = {
			.now_s = now, .pit_c = c->pit_c, .setpoint_c = c->setpoint_c, .ambient_c = c->ambient_c,
			.u_prev_raw = c->u_raw, .u_prev_applied = c->u_applied, .u_ff = c->learn.u_ff, .saturated = c->saturated,
			.sched_PB_c = sched_PB, .sched_Ti = sched_Ti, .sched_Td = sched_Td,
			.sched_K = sched_K, .sched_tau = sched_tau, .sched_theta = sched_theta,
			.cycle_time_s = c->ccfg.cycle_s, .u_min = c->ccfg.u_min, .u_max = c->ccfg.u_max,
			.target_reached = c->target_reached, .fan_on = pf_outputs_get(PF_OUT_FAN), .fan_pct = pf_outputs_get_fan_pct(),
			.tuning = c->autotune.active || pf_tuner_active(NULL, NULL, NULL),
			.hist = pf_history_ctrl_view(),
		};
		if (c->ctrl_reset_needed) { c->cops->reset(c->cinst, &in); c->ctrl_reset_needed = false; }
		u = c->cops->update(c->cinst, &in, &c->dbg);
		if (isnan(u) || isinf(u) || u < -5 || u > 5) {
			if (++c->safety.ctrl_fault_count >= 3 && strcmp(c->cops->id, "pid")) {
				pf_safety_set_error(c, "E06_CONTROLLER_FAULT", "Controller '%s' produced invalid output repeatedly; switched to builtin PID", c->cops->id);
				event(PF_LVL_ERROR, "E06_CONTROLLER_FAULT", c->safety.error_msg);
				controller_load(c, "pid");
			}
			u = c->cfg.u_min;
		} else c->safety.ctrl_fault_count = 0;
	}
	c->u_raw = u;
	pf_cycle_begin(&c->cycle, &c->ccfg, now, u);
	c->u_applied = c->cycle.u_applied;
	c->saturated = c->cycle.saturated;
	if (c->autotune.active) {
		if (c->autotune.phase > 0) { c->autotune.hi_sum += c->u_applied; c->autotune.hi_n++; }
		else { c->autotune.lo_sum += c->u_applied; c->autotune.lo_n++; }
		c->autotune.cyc_sum += c->u_applied; c->autotune.cyc_n++;
		if (c->autotune.crossings > c->autotune.adjust_at_cross && c->pit_valid) {
			c->autotune.meas_err_sum += c->pit_c - c->setpoint_c;
			c->autotune.meas_err_n++;
		}
	}
	learn_track_steady(c, now);
	if (c->learn.rise_active) { c->learn.rise_u_sum += c->u_applied; c->learn.rise_n++; }
	/* fan-PID: below u_min on a non-PWM fan, modulate the fan instead (control.py:923) */
	c->fan_pid_active = c->cfg.fan_pid && !c->pwm_control && c->target_reached && u < c->cfg.u_min;
}

static void run_fan_logic(pf_control *c, double now)
{
	const pf_cfg *g = &c->cfg;
	bool manual_fan = c->manual_until[PF_OUT_FAN] > now;
	if (manual_fan || c->lid_open) return;
	bool fan = pf_outputs_get(PF_OUT_FAN);
	bool splus_phase = c->mode == PF_MODE_SMOKE || (c->mode == PF_MODE_HOLD && c->target_reached);

	/* PWM duty from temperature profile (HOLD, DC fan, pwm_control) */
	if (g->dc_fan && c->mode == PF_MODE_HOLD && c->pwm_control && now - c->fan_update_t > g->pwm_update_s) {
		c->fan_update_t = now;
		int duty = g->pwm_max_duty;
		if (c->pit_c > c->setpoint_c) duty = g->pwm_min_duty;
		else {
			for (int i = 0; i < g->pwm_n; i++)
				if (c->setpoint_c - c->pit_c <= g->pwm_ranges_c[i]) { duty = g->pwm_profiles[i]; break; }
		}
		c->duty_cycle = (int)pf_clamp(duty, g->pwm_min_duty, g->pwm_max_duty);
	}

	if (c->mode == PF_MODE_HOLD && c->fan_pid_active) {
		double total = c->s_plus ? g->splus_on_s + g->splus_off_s : c->ccfg.cycle_s;
		double max_ratio = c->s_plus ? g->splus_on_s / total : 1.0;
		double ratio = fmax(0, c->u_raw / g->u_min) * max_ratio;
		double on_s = total * ratio, off_s = total * (1 - ratio);
		if (fan && now - c->fan_toggle_t > on_s) { pf_outputs_set(PF_OUT_FAN, false); c->fan_toggle_t = now; }
		else if (!fan && now - c->fan_toggle_t > off_s) { fan_on(c, c->duty_cycle); c->fan_toggle_t = now; }
		return;
	}

	if (splus_phase && c->s_plus) {
		if (c->pit_c > g->splus_max_c || c->pit_c < g->splus_min_c) {
			if (!fan) fan_on(c, c->duty_cycle);
		} else if (fan && now - c->fan_toggle_t > g->splus_on_s) {
			pf_outputs_set(PF_OUT_FAN, false);
			c->fan_toggle_t = now;
		} else if (!fan && now - c->fan_toggle_t > g->splus_off_s) {
			c->fan_toggle_t = now;
			if (g->dc_fan && g->splus_ramp && (c->mode == PF_MODE_SMOKE || !c->pwm_control)) {
				pf_outputs_set(PF_OUT_FAN, true);
				pf_outputs_fan_pct(g->pwm_min_duty);
				c->fan_ramping = true;
				c->ramp_end_t = now + g->splus_on_s;
			} else fan_on(c, c->duty_cycle);
		}
		if (c->fan_ramping) {
			double frac = 1.0 - fmax(0, c->ramp_end_t - now) / g->splus_on_s;
			int target = (int)(g->pwm_max_duty * g->splus_duty / 100.0);
			pf_outputs_fan_pct((int)(g->pwm_min_duty + (target - g->pwm_min_duty) * frac));
			if (frac >= 1.0) c->fan_ramping = false;
		}
		return;
	}

	/* default: fan on at the configured duty */
	if (!fan) { fan_on(c, c->duty_cycle); return; }
	if (g->dc_fan) {
		int want = c->pwm_control && c->mode == PF_MODE_HOLD ? c->duty_cycle : g->pwm_max_duty;
		if (!c->pwm_control) c->duty_cycle = g->pwm_max_duty;
		if (pf_outputs_get_fan_pct() != want) pf_outputs_fan_pct(want);
	}
}

static void run_mode(pf_control *c, double now)
{
	const pf_cfg *g = &c->cfg;
	bool feeding = c->mode == PF_MODE_STARTUP || c->mode == PF_MODE_REIGNITE || c->mode == PF_MODE_SMOKE ||
	               c->mode == PF_MODE_HOLD || c->mode == PF_MODE_PRIME;

	/* lid-open detection / expiry */
	if (c->mode == PF_MODE_HOLD || c->mode == PF_MODE_SMOKE) {
		if (c->mode == PF_MODE_HOLD && c->pit_c >= c->setpoint_c) c->target_reached = true;
		/* threshold is a percentage of the setpoint in the user's units, as in the original */
		double lid_thresh_c = pf_to_c(pf_from_c(c->setpoint_c, g->units) * (100.0 - g->lid_threshold_pct) / 100.0, g->units);
		if (c->target_reached && g->lid_detect && !c->lid_open && c->pit_c < lid_thresh_c) {
			c->lid_open = true;
			c->lid_open_until = now + g->lid_pause_s;
			c->target_reached = false;
			pf_outputs_set(PF_OUT_AUGER, false);
			pf_outputs_set(PF_OUT_FAN, false);
			pf_cycle_stop(&c->cycle);
			learn_reset_window(c, now);
			LOGI(TAG, "lid open detected: pausing feed for %.0f s", g->lid_pause_s);
			event(PF_LVL_INFO, "LID_OPEN", "Lid open detected, feed paused");
		}
		if (c->lid_open && now > c->lid_open_until) {
			c->lid_open = false;
			fan_on(c, c->duty_cycle);
			pf_cycle_begin(&c->cycle, &c->ccfg, now, g->u_min);
			LOGI(TAG, "lid open pause ended");
		}
	}

	/* cycle engine */
	if (feeding && !c->lid_open) {
		if (c->mode == PF_MODE_HOLD) run_hold_cycle(c, now);
		else if (c->mode != PF_MODE_PRIME && pf_cycle_done(&c->cycle, now)) smoke_cycle(c, now);
		bool want = pf_cycle_auger_on(&c->cycle, now);
		if (c->manual_until[PF_OUT_AUGER] <= now) {
			bool is = pf_outputs_get(PF_OUT_AUGER);
			if (want && !is) { pf_outputs_set(PF_OUT_AUGER, true); c->auger_on_since = now; }
			else if (!want && is) { pf_outputs_set(PF_OUT_AUGER, false); c->auger_total_on_s += now - c->auger_on_since; }
		}
	} else if (c->mode != PF_MODE_MANUAL && pf_outputs_get(PF_OUT_AUGER) && c->manual_until[PF_OUT_AUGER] <= now) {
		pf_outputs_set(PF_OUT_AUGER, false);
	}
	/* absolute auger cap regardless of source (manual included) */
	if (pf_outputs_get(PF_OUT_AUGER) && now - c->auger_on_since > g->auger_max_on_s) {
		pf_outputs_set(PF_OUT_AUGER, false);
		/* The pellets it fed while it ran still went in the pot. Every other path that switches the
		 * auger off adds its run to the total; this one did not, so a capped run was fuel the
		 * hopper estimate never saw. */
		c->auger_total_on_s += now - c->auger_on_since;
		c->manual_until[PF_OUT_AUGER] = 0;
		LOGW(TAG, "auger on for %.0f s: forced off (safety cap)", g->auger_max_on_s);
		event(PF_LVL_WARN, "W08_AUGER_CAP", "Auger exceeded maximum continuous on time and was switched off");
	}

	if (c->mode == PF_MODE_SMOKE || c->mode == PF_MODE_HOLD) run_fan_logic(c, now);

	/* manual override expiry: outputs return to mode defaults on the next pass */
	for (int i = 0; i < PF_OUT_COUNT; i++)
		if (c->manual_until[i] && c->manual_until[i] <= now && c->mode != PF_MODE_MANUAL) c->manual_until[i] = 0;

	/* timed transitions */
	switch (c->mode) {
	case PF_MODE_STARTUP:
	case PF_MODE_REIGNITE: {
		bool timer = now - c->mode_start > c->startup_duration_s;
		bool exit_temp = c->startup_exit_c > 0 && c->pit_c >= c->startup_exit_c;
		/* the fire is evidently lit once the pit has climbed exit_rise above where this startup began */
		/* rise-based exit needs the fire to be evidently established: the rise, a sustained climb rate, and at least 90 s */
		bool exit_rise_up = g->startup_exit_rise_c > 0 && !isnan(c->startup_base_c) && c->pit_valid && now - c->mode_start > 90
		                    && c->pit_c - c->startup_base_c >= g->startup_exit_rise_c && c->pit_rate_c_min >= 2.0
		                    && (c->safety.coldstart_active || !c->safety.floor_set || c->pit_c >= c->safety.floor_c);   /* never hand over below the flame-out floor */
		/* optional: leave startup as soon as cold-start has confirmed a rise and the pit is past the classic minimum */
		bool exit_rise = g->coldstart_exit_on_rise && c->safety.coldstart_active && c->safety.coldstart_reached && c->pit_c >= g->min_startup_c;
		if ((timer && pf_safety_startup_can_finish(c, now)) || exit_temp || exit_rise || exit_rise_up) {
			pf_safety_on_startup_exit(c, now);
			pf_mode nm = c->mode == PF_MODE_REIGNITE ? c->safety.reignite_last : c->next_mode;
			if (nm != PF_MODE_SMOKE && nm != PF_MODE_HOLD) nm = PF_MODE_SMOKE;
			if (nm == PF_MODE_HOLD && c->setpoint_c <= 0) c->setpoint_c = g->after_startup_setpoint_c;
			c->s_plus = c->s_plus || g->splus_default;
			enter_mode(c, nm, now);
		}
		break;
	}
	case PF_MODE_SHUTDOWN:
		if (now - c->mode_start > g->shutdown_s) {
			enter_mode(c, PF_MODE_STOP, now);
			if (g->auto_power_off) c->power_off_requested = true;
		}
		break;
	case PF_MODE_PRIME:
		if (now - c->mode_start > c->prime_duration_s) {
			pf_mode nm = c->next_mode == PF_MODE_STARTUP ? PF_MODE_STARTUP : PF_MODE_STOP;
			if (nm == PF_MODE_STARTUP) c->next_mode = g->after_startup_mode;
			enter_mode(c, nm, now);
		}
		break;
	case PF_MODE_ERROR:
		if (c->safety.error_fan_until && now > c->safety.error_fan_until) {
			c->safety.error_fan_until = 0;
			pf_outputs_all_off();
		}
		break;
	default: break;
	}
}

/* ------------------------------------------------------------------ public */

void pf_control_reload_settings(pf_control *c)
{
	char old_id[32];
	pf_strlcpy(old_id, c->cfg.controller_id, sizeof old_id);
	pf_units old_u = c->cfg.units;
	load_cfg(&c->cfg);
	cycle_cfg_for(c);
	if (c->cfg.units != old_u) {
		/* setpoint is stored in C; nothing to convert. Controllers get new PB in C. */
	}
	/* The starting values are the ground everything else is built on. When they are typed in
	 * again, the observations, the fitted plant and the per-temperature corrections were all
	 * learned against a grill that is no longer described the same way, so they go and the
	 * learning starts from the new numbers. The tuning library stays: it is measured, not learned. */
	double now_typed[3];
	typed_gains(c->cfg.controller_id, now_typed);
	bool typed_changed = !strcmp(old_id, c->cfg.controller_id) && c->cfg.units == old_u &&
	                     (now_typed[0] != c->typed_gains[0] || now_typed[1] != c->typed_gains[1] ||
	                      now_typed[2] != c->typed_gains[2]);
	if (strcmp(old_id, c->cfg.controller_id)) controller_load(c, c->cfg.controller_id);
	else if (typed_changed) forget_learning(c, PF_CLEAR_LEARNING, "the starting values were changed");
	else if (c->cinst && c->cops->configure) {
		char *cfg = controller_config_json(c->cops->id, c->cfg.units);
		c->cops->configure(c->cinst, cfg);
		free(cfg);
		c->ctrl_reset_needed = true;
	}
	memcpy(c->typed_gains, now_typed, sizeof now_typed);
	if (c->cfg.dc_fan) pf_outputs_pwm_frequency(c->cfg.pwm_hz);
	LOGI(TAG, "settings reloaded");
}

void pf_control_init(pf_control *c, bool sim)
{
	memset(c, 0, sizeof *c);
	c->sim = sim;
	c->hopper_pct = -1;
	c->ambient_c = NAN;
	c->pit_c = NAN;
	controller_fill_defaults();
	load_cfg(&c->cfg);
	cycle_cfg_for(c);
	c->s_plus = c->cfg.splus_default;
	c->pwm_control = c->cfg.pwm_control_default;
	c->duty_cycle = c->cfg.pwm_max_duty;
	c->setpoint_c = c->cfg.after_startup_setpoint_c;
	pf_safety_reset(c);
	pf_notify_init(&c->notify);
	controller_load(c, c->cfg.controller_id);
	c->mode = PF_MODE_STOP;
	pf_outputs_all_off();
	if (c->cfg.dc_fan) pf_outputs_pwm_frequency(c->cfg.pwm_hz);
}

void pf_control_shutdown(pf_control *c)
{
	pf_outputs_all_off();
	controller_destroy(c);
	/* a recipe still running at shutdown leaves its parsed ending behind otherwise -- harmless in
	 * the daemon, which is exiting, and a leak the sanitised tests rightly refuse */
	cJSON_Delete(c->recipe.ends);
	c->recipe.ends = NULL;
}

void pf_control_boot_check(pf_control *c, bool unclean_restart, double now)
{
	if (!unclean_restart) return;
	if (c->pit_valid && c->pit_c > c->cfg.restart_hot_c) {
		LOGW(TAG, "unclean restart with a hot pit (%.0f C): entering Shutdown for cooldown", c->pit_c);
		event(PF_LVL_WARN, "W09_HOT_RESTART", "Restarted after a crash with a hot pit; running Shutdown cooldown");
		enter_mode(c, PF_MODE_SHUTDOWN, now);
	} else {
		event(PF_LVL_WARN, "W10_UNCLEAN_RESTART", "Restarted after an unclean shutdown");
	}
}

/* ------------------------------------------------------------------ warm restart
 * A software update restarts the service in the middle of a cook. The outgoing process writes this
 * snapshot on its clean shutdown; the incoming one re-enters the same mode with the same set point,
 * timers, probe targets and cook bookkeeping, so the grill only loses the few seconds the restart
 * takes (outputs park OFF while no daemon owns the GPIO lines). Only safe modes resume: Manual and
 * Prime drop to Stop, Error stays whatever the safety code decides on the next tick. */
static bool resumable(pf_mode m)
{
	return m == PF_MODE_STARTUP || m == PF_MODE_REIGNITE || m == PF_MODE_SMOKE || m == PF_MODE_HOLD || m == PF_MODE_SHUTDOWN || m == PF_MODE_MONITOR;
}

char *pf_control_resume_json(const pf_control *c, double now)
{
	if (!resumable(c->mode)) return NULL;
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "version", PF_VERSION);
	cJSON_AddNumberToObject(o, "saved_wall", pf_wall());
	cJSON_AddStringToObject(o, "mode", pf_mode_name(c->mode));
	cJSON_AddStringToObject(o, "next_mode", pf_mode_name(c->next_mode));
	cJSON_AddNumberToObject(o, "mode_elapsed", now - c->mode_start);
	cJSON_AddNumberToObject(o, "setpoint_c", c->setpoint_c);
	cJSON_AddBoolToObject(o, "s_plus", c->s_plus);
	cJSON_AddBoolToObject(o, "pwm_control", c->pwm_control);
	cJSON_AddNumberToObject(o, "u_applied", c->u_applied);
	cJSON_AddNumberToObject(o, "cook_start_wall", c->cook_start_wall);
	cJSON_AddNumberToObject(o, "auger_total_on_s", c->auger_total_on_s);
	cJSON_AddNumberToObject(o, "cook_max_pit_c", c->cook_max_pit_c);
	cJSON_AddNumberToObject(o, "startup_duration_s", c->startup_duration_s);
	cJSON_AddNumberToObject(o, "startup_exit_c", c->startup_exit_c);
	cJSON_AddNumberToObject(o, "startup_base_c", isnan(c->startup_base_c) ? -1000 : c->startup_base_c);
	cJSON_AddNumberToObject(o, "raw_startup_c", c->raw_startup_c);
	cJSON_AddNumberToObject(o, "ambient_c", isnan(c->ambient_c) ? -1000 : c->ambient_c);
	cJSON_AddBoolToObject(o, "floor_set", c->safety.floor_set);
	cJSON_AddNumberToObject(o, "floor_c", c->safety.floor_c);
	cJSON_AddNumberToObject(o, "reignite_retries_left", c->safety.reignite_retries_left);
	/* A recipe outlives a restart. Losing it mid-cook left the grill holding at whatever the
	 * last step had set, with nothing to advance it and nothing on screen to say so -- and a
	 * recipe is six hours long precisely when an update or a watchdog restart is most likely to
	 * land inside one. The recipe is reloaded by id and the place in it is restored. */
	if (c->recipe.active) {
		cJSON *rc = cJSON_AddObjectToObject(o, "recipe");
		cJSON_AddNumberToObject(rc, "id", c->recipe.r.id);
		cJSON_AddNumberToObject(rc, "step", c->recipe.step);
		cJSON_AddNumberToObject(rc, "step_elapsed", now - c->recipe.step_start);
		cJSON_AddNumberToObject(rc, "at_temp_elapsed", c->recipe.at_temp_since > 0 ? now - c->recipe.at_temp_since : -1);
		cJSON_AddBoolToObject(rc, "triggered", c->recipe.triggered);
		cJSON_AddBoolToObject(rc, "waiting", c->recipe.waiting);
		cJSON_AddBoolToObject(rc, "lead_fired", c->recipe.lead_fired);
		cJSON_AddBoolToObject(rc, "lid_seen", c->recipe.lid_seen);
		{ cJSON *fl = cJSON_AddArrayToObject(rc, "flags"); for (int i = 0; i < c->recipe.r.nsteps && i < 16; i++) cJSON_AddItemToArray(fl, cJSON_CreateNumber(c->recipe.flags[i])); }
		cJSON_AddBoolToObject(rc, "prompt_given", c->recipe.prompt_given);
		cJSON_AddBoolToObject(rc, "said", c->recipe.said);
	}
	/* Which probes the cook said were in the food: a fact only they can supply, so it must not
	 * have to be supplied twice. */
	char in_use[256];
	pf_probes_in_use_csv(in_use, sizeof in_use);
	if (in_use[0]) cJSON_AddStringToObject(o, "probes_in_use", in_use);
	cJSON *pr = cJSON_AddArrayToObject(o, "probes");
	for (int i = 0; i < c->notify.n; i++) {
		const pf_notify_probe *p = &c->notify.probes[i];
		if (p->target_c <= 0 && p->limit_high_c <= 0 && p->limit_low_c <= 0) continue;
		cJSON *po = cJSON_CreateObject();
		cJSON_AddStringToObject(po, "label", p->label);
		cJSON_AddNumberToObject(po, "target_c", p->target_c);
		cJSON_AddNumberToObject(po, "after", p->after);
		cJSON_AddNumberToObject(po, "limit_high_c", p->limit_high_c);
		cJSON_AddNumberToObject(po, "limit_low_c", p->limit_low_c);
		cJSON_AddItemToArray(pr, po);
	}
	if (c->notify.timer.running) {
		cJSON *t = cJSON_AddObjectToObject(o, "timer");
		cJSON_AddNumberToObject(t, "remaining", c->notify.timer.paused ? c->notify.timer.remaining : c->notify.timer.end_t - now);
		cJSON_AddNumberToObject(t, "duration", c->notify.timer.duration);
		cJSON_AddNumberToObject(t, "after", c->notify.timer.after);
		cJSON_AddBoolToObject(t, "paused", c->notify.timer.paused);
	}
	char *txt = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	return txt;
}

bool pf_control_resume(pf_control *c, const char *json, double now)
{
	cJSON *o = cJSON_Parse(json);
	if (!o) return false;
	int m = pf_mode_from_name(pf_json_str(o, "mode", ""));
	double age = pf_wall() - pf_json_num(o, "saved_wall", 0);
	if (m < 0 || !resumable((pf_mode)m) || age < 0 || age > 300) { cJSON_Delete(o); return false; }
	if (c->mode != PF_MODE_STOP) { cJSON_Delete(o); return false; }   /* boot check already chose a mode */

	int nm = pf_mode_from_name(pf_json_str(o, "next_mode", ""));
	c->next_mode = nm >= 0 ? (pf_mode)nm : PF_MODE_STOP;
	double sp = pf_json_num(o, "setpoint_c", 0);
	if (sp > 0) c->setpoint_c = sp;
	c->s_plus = pf_json_bool(o, "s_plus", c->s_plus);
	c->pwm_control = pf_json_bool(o, "pwm_control", c->pwm_control);
	double amb = pf_json_num(o, "ambient_c", -1000);
	if (amb > -999 && amb <= AMBIENT_MAX_C && !c->ambient_from_probe) c->ambient_c = amb;

	enter_mode(c, (pf_mode)m, now);

	/* continuity: timers keep counting from where they were, the cook keeps its start time */
	double elapsed = pf_json_num(o, "mode_elapsed", 0);
	if (elapsed > 0) c->mode_start = now - elapsed;
	double csw = pf_json_num(o, "cook_start_wall", 0);
	if (csw > 0) c->cook_start_wall = csw;
	c->auger_total_on_s = pf_json_num(o, "auger_total_on_s", 0);
	c->cook_max_pit_c = pf_json_num(o, "cook_max_pit_c", 0);
	cJSON *rc = cJSON_GetObjectItem(o, "recipe");
	if (rc) {
		/* Reloaded from the database rather than from the snapshot, so a recipe that was edited
		 * between the two runs comes back as it is now -- and one that was deleted does not come
		 * back at all, which is the right answer to "run the recipe that is no longer there". */
		int rid = pf_json_int(rc, "id", 0);
		if (rid > 0 && pf_recipe_load(rid, &c->recipe.r) == 0) {
			int step = pf_json_int(rc, "step", 0);
			if (step >= 0 && step < c->recipe.r.nsteps) {
				c->recipe.active = true;
				c->recipe.step = step;
				c->recipe.step_start = now - pf_json_num(rc, "step_elapsed", 0);
				{ double ate = pf_json_num(rc, "at_temp_elapsed", -1); c->recipe.at_temp_since = ate >= 0 ? now - ate : 0; }
				c->recipe.triggered = pf_json_bool(rc, "triggered", false);
				c->recipe.waiting = pf_json_bool(rc, "waiting", false);
				c->recipe.lead_fired = pf_json_bool(rc, "lead_fired", false);
				c->recipe.said = pf_json_bool(rc, "said", false);
				/* What the cook had already done before the restart. Losing these would ask them
				 * to open the lid a second time for a step they had already answered. */
				c->recipe.prompt_given = pf_json_bool(rc, "prompt_given", false);
				{ cJSON *fl = cJSON_GetObjectItem(rc, "flags"); int i = 0; cJSON *v; cJSON_ArrayForEach(v, fl) { if (i < 16) c->recipe.flags[i] = (unsigned char)v->valueint; i++; } }
				c->recipe.lid_seen = pf_json_bool(rc, "lid_seen", false);
				/* The step's condition comes back with the recipe, and its clocks start again:
				 * a "for ten minutes" cannot be said to have been running while nothing was. */
				cJSON_Delete(c->recipe.ends);
				c->recipe.ends = c->recipe.r.steps[step].ends[0] ? cJSON_Parse(c->recipe.r.steps[step].ends) : NULL;
				memset(&c->recipe.clocks, 0, sizeof c->recipe.clocks);
				c->recipe.wants_prompt = tree_mentions(c->recipe.ends, "prompt") || tree_mentions(c->recipe.ends, "lid");
				c->recipe.last_eval = 0;
				c->recipe.eta_s = -1;
				LOGI(TAG, "recipe '%s' resumed at step %d/%d", c->recipe.r.name, step + 1, c->recipe.r.nsteps);
			}
		} else LOGW(TAG, "recipe %d could not be resumed after the restart", rid);
	}
	if (m == PF_MODE_STARTUP || m == PF_MODE_REIGNITE) {
		c->startup_duration_s = pf_json_num(o, "startup_duration_s", c->startup_duration_s);
		c->startup_exit_c = pf_json_num(o, "startup_exit_c", c->startup_exit_c);
		double b = pf_json_num(o, "startup_base_c", -1000);
		c->startup_base_c = b > -999 ? b : NAN;
		c->raw_startup_c = pf_json_num(o, "raw_startup_c", c->raw_startup_c);
	}
	if (m == PF_MODE_SMOKE || m == PF_MODE_HOLD) {
		c->safety.floor_set = pf_json_bool(o, "floor_set", false);
		c->safety.floor_c = pf_json_num(o, "floor_c", 0);
	}
	c->safety.reignite_retries_left = (int)pf_json_num(o, "reignite_retries_left", c->safety.reignite_retries_left);
	if (m == PF_MODE_HOLD) {
		double u = pf_json_num(o, "u_applied", c->u_applied);
		if (u > 0 && u <= 1) c->u_raw = c->u_applied = u;   /* bumpless controller reset on the first cycle */
	}
	pf_notify_sync(&c->notify, &c->sensors);
	const char *in_use = pf_json_str(o, "probes_in_use", NULL);
	if (in_use) pf_probes_set_in_use(in_use);
	cJSON *pr = cJSON_GetObjectItem(o, "probes"), *po;
	cJSON_ArrayForEach(po, pr) {
		const char *label = pf_json_str(po, "label", "");
		pf_notify_set_target(&c->notify, label, pf_json_num(po, "target_c", 0), (int)pf_json_num(po, "after", 0));
		pf_notify_set_limits(&c->notify, label, pf_json_num(po, "limit_high_c", 0), pf_json_num(po, "limit_low_c", 0));
	}
	cJSON *t = cJSON_GetObjectItem(o, "timer");
	if (t) {
		double rem = pf_json_num(t, "remaining", 0);
		if (rem > 0) {
			pf_notify_timer_start(&c->notify, rem, (int)pf_json_num(t, "after", 0), now);
			c->notify.timer.duration = pf_json_num(t, "duration", rem);
			if (pf_json_bool(t, "paused", false)) pf_notify_timer_pause(&c->notify, now);
		}
	}
	char msg[128];
	snprintf(msg, sizeof msg, "Resumed %s after a software restart (%.0f s gap)", pf_mode_name((pf_mode)m), age);
	LOGW(TAG, "%s", msg);
	event(PF_LVL_WARN, "W11_RESUMED", msg);
	cJSON_Delete(o);
	return true;
}

static void read_sensors(pf_control *c)
{
	pf_probes_snapshot(&c->sensors);
	int p = c->sensors.primary;
	if (p >= 0 && c->sensors.p[p].valid) { c->pit_c = c->sensors.p[p].temp_c; c->pit_valid = true; if (c->pit_c > c->cook_max_pit_c) c->cook_max_pit_c = c->pit_c; }
	else c->pit_valid = false;
	/* filtered pit slope in C/min (30 s time constant) for the startup exit rule */
	if (c->pit_valid) {
		double now_t = c->last_step > 0 ? c->last_step : 0;
		if (c->pit_rate_last_t > 0 && now_t > c->pit_rate_last_t) {
			double dt = now_t - c->pit_rate_last_t, inst = (c->pit_c - c->pit_rate_last_c) / dt * 60.0, a = dt / 30.0;
			if (a > 1) a = 1;
			c->pit_rate_c_min += (inst - c->pit_rate_c_min) * a;
		}
		c->pit_rate_last_c = c->pit_c; c->pit_rate_last_t = now_t;
	}
	c->hopper_pct = pf_pellets_hopper_pct();
	c->ambient_from_probe = false;
	for (int i = 0; i < c->sensors.n; i++)
		if (c->sensors.p[i].ambient && c->sensors.p[i].valid) { c->ambient_c = c->sensors.p[i].temp_c; c->ambient_from_probe = true; break; }
	if (c->ambient_from_probe && c->ambient_c > AMBIENT_MAX_C) c->ambient_from_probe = false;   /* an "ambient" probe inside the grill is not outdoor air */
	if (!c->ambient_from_probe) {
		/* local weather for the configured postal code: the best outdoor reference there is without a probe */
		pf_weather w;
		pf_weather_get(&w);
		if (w.valid && !isnan(w.temp_c) && w.temp_c <= AMBIENT_MAX_C) { c->ambient_c = w.temp_c; c->ambient_from_probe = true; }
	}
	if (!c->ambient_from_probe && c->safety.coldstart_active && !isnan(c->safety.baseline_c) && c->safety.baseline_c <= AMBIENT_MAX_C) c->ambient_c = c->safety.baseline_c;
	if (isnan(c->ambient_c) || c->ambient_c > AMBIENT_MAX_C) ambient_provisional(c);
	ambient_remember(c);
}

static void publish(pf_control *c, double now)
{
	pf_status s;
	memset(&s, 0, sizeof s);
	s.t = now;
	s.aim_since = c->aim_since;
	s.wall = pf_wall();
	s.mode = c->mode;
	s.next_mode = c->next_mode;
	s.mode_start = c->mode_start;
	s.setpoint_c = c->setpoint_c;
	s.s_plus = c->s_plus;
	s.pwm_control = c->pwm_control;
	s.duty_cycle = c->duty_cycle;
	s.outputs = pf_outputs_mask();
	s.fan_pct = pf_outputs_get_fan_pct();
	s.u_raw = c->u_raw;
	s.u_applied = (c->mode == PF_MODE_SMOKE || c->mode == PF_MODE_HOLD || c->mode == PF_MODE_STARTUP || c->mode == PF_MODE_REIGNITE) ? c->u_applied : 0;
	s.saturated = c->saturated;
	s.cycle_s = c->ccfg.cycle_s;
	s.lid_open = c->lid_open;
	s.lid_open_until = c->lid_open_until;
	s.target_reached = c->target_reached;
	s.startup_duration = c->startup_duration_s;
	s.shutdown_duration = c->cfg.shutdown_s;
	s.prime_duration = c->mode == PF_MODE_PRIME ? c->prime_duration_s : 0;
	s.prime_amount = c->mode == PF_MODE_PRIME ? c->prime_amount_g : 0;
	s.coldstart_active = c->safety.coldstart_active;
	s.coldstart_reached = c->safety.coldstart_reached;
	s.startup_exit_c = c->startup_exit_c;
	s.pmode = c->cfg.pmode;
	s.cook_start_wall = c->cook_start_wall;
	s.coldstart_baseline_c = c->safety.baseline_c;
	s.coldstart_deadline = c->safety.coldstart_deadline;
	pf_strlcpy(s.error_code, c->safety.error_code, sizeof s.error_code);
	pf_strlcpy(s.error_msg, c->safety.error_msg, sizeof s.error_msg);
	pf_strlcpy(s.controller_id, c->cops ? c->cops->id : "", sizeof s.controller_id);
	s.ctrl_dbg = c->dbg;
	s.tuning = ctrl_tuning(c);
	s.ambient_c = c->ambient_c;
	s.reignite_retries_left = c->safety.reignite_retries_left;
	s.sensors = c->sensors;
	s.hopper_pct = c->hopper_pct;
	s.sim = c->sim;
	for (int i = 0; i < c->sensors.n && i < PF_MAX_PROBES; i++) {
		const pf_notify_probe *p = pf_notify_find(&c->notify, c->sensors.p[i].label);
		s.notify[i].after = p ? p->after : 0;
		s.notify[i].eta_s = p ? p->eta_s : -1;
		s.notify[i].eta_step_s = p ? p->eta_step_s : -1;
		pf_strlcpy(s.notify[i].next_step, p ? p->next_step : "", sizeof s.notify[i].next_step);
		s.notify[i].limit_high_c = p ? p->limit_high_c : 0;
		s.notify[i].limit_low_c = p ? p->limit_low_c : 0;
		s.notify[i].nsteps = p ? p->nsteps : 0;
		for (int k = 0; p && k < p->nsteps && k < PF_MAX_STEPS; k++) {
			pf_strlcpy(s.notify[i].steps[k].name, p->steps[k].name, sizeof s.notify[i].steps[k].name);
			s.notify[i].steps[k].temp_c = p->steps[k].temp_c;
			s.notify[i].steps[k].fired = p->steps[k].fired;
		}
	}
	s.timer.running = c->notify.timer.running;
	s.timer.paused = c->notify.timer.paused;
	s.timer.remaining = c->notify.timer.running ? (c->notify.timer.paused ? c->notify.timer.remaining : c->notify.timer.end_t - now) : 0;
	s.timer.duration = c->notify.timer.duration;
	s.timer.after = c->notify.timer.after;
	s.autotune_active = c->autotune.active;
	s.autotune_crossings = c->autotune.crossings;
	s.u_ff = c->learn.u_ff;
	s.recipe.active = c->recipe.active;
	if (c->recipe.active) {
		const pf_recipe_step *rs = &c->recipe.r.steps[c->recipe.step];
		pf_strlcpy(s.recipe.name, c->recipe.r.name, sizeof s.recipe.name);
		s.recipe.step = c->recipe.step;
		s.recipe.stage = 0; s.recipe.stages = 0;
		for (int i = 0; i < c->recipe.r.nsteps; i++) {
			pf_mode m = c->recipe.r.steps[i].mode;
			if (m != PF_MODE_HOLD && m != PF_MODE_SMOKE) continue;
			s.recipe.stages++;
			if (i == c->recipe.step) s.recipe.stage = s.recipe.stages;
		}
		s.recipe.nsteps = c->recipe.r.nsteps;
		s.recipe.waiting = c->recipe.waiting;
		s.recipe.step_mode = rs->mode;
		memcpy(s.recipe.flags, c->recipe.flags, sizeof s.recipe.flags);
		/* What the cook is actually waiting for: the clock when there is one, and otherwise the
		 * estimate of when the meat gets there. Either way it is "how long until something
		 * happens", which is the only question the number is asked. */
		s.recipe.remaining_s = c->recipe.waiting ? -1 : c->recipe.eta_s;
		s.recipe.clock_s = c->recipe.waiting ? -1 : c->recipe.clock_s;
		s.recipe.at_temp = rs->mode != PF_MODE_HOLD || c->recipe.at_temp_since > 0;
		s.recipe.id = c->recipe.r.id;
		pf_strlcpy(s.recipe.message, rs->message, sizeof s.recipe.message);
		/* Waiting on both signals with the lid still shut: the app says which half is missing
		 * rather than offering a button that does nothing. */
		s.recipe.needs_lid = c->recipe.waiting && !c->recipe.lid_seen
		                     && tree_mentions(c->recipe.ends, "lid") && tree_mentions(c->recipe.ends, "prompt");
	}
	pf_status_publish(&s);
	pf_history_record(&s, now, c->cfg.history_sample_s);
}

void pf_control_step(pf_control *c, double now)
{
	if (c->mode_start == 0) c->mode_start = now;
	pf_cmd cmd;
	while (pf_cmdq_pop(&cmd)) handle_cmd(c, &cmd, now);

	read_sensors(c);
	apply_request(c, now);
	run_notify(c, now);
	run_recipe(c, now);
	recipe_aftercare(c);
	learn_rise_track(c, now);

	int action = pf_safety_tick(c, now);
	if (action == PF_MODE_ERROR) enter_mode(c, PF_MODE_ERROR, now);
	else if (action == PF_MODE_REIGNITE) enter_mode(c, PF_MODE_REIGNITE, now);

	run_mode(c, now);

	/* physical inputs */
	if (pf_outputs_read_input(PF_IN_SHUTDOWN) && c->mode != PF_MODE_STOP) {
		LOGI(TAG, "shutdown input asserted");
		enter_mode(c, c->mode == PF_MODE_HOLD || c->mode == PF_MODE_SMOKE ? PF_MODE_SHUTDOWN : PF_MODE_STOP, now);
	}

	publish(c, now);
	c->last_step = now;
}
