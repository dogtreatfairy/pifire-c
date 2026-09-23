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
static void autotune_cycle(const pf_control *c, int i, double *period, double *amp);
static bool autotune_settled(const pf_control *c);

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

/* Throw away what was learned, or what was measured, or both -- and tell the controller, because it
 * holds its own copy: clearing only the stored copy would leave the old numbers running until the
 * next restart. `what` is a mask of PF_FORGET_*. */
static void forget_learning(pf_control *c, unsigned what, const char *why)
{
	if (what & PF_FORGET_REFINEMENT) pf_learning_forget();
	if (what & PF_FORGET_TUNING) pf_learning_clear_tuning();
	if (c->cinst && c->cops->forget) c->cops->forget(c->cinst, what);
	if (what & PF_FORGET_TUNING)
		pf_events_emit("Tuning_Cleared", "Measured tuning cleared",
		               "The tuning library is gone and the grill is back to the Proportional Band, Integral Time and Derivative Time typed on the controller page (%s).", why);
	else
		pf_events_emit("Learning_Cleared", "Learning cleared",
		               "The grill starts learning again from the tuning it has (%s).", why);
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
		recipe_begin_step(c, now);
		break;
	case PF_CMD_RECIPE_NEXT:
		if (c->recipe.active && c->recipe.waiting) recipe_advance(c, now);
		break;
	case PF_CMD_RECIPE_STOP:
		if (c->recipe.active) { c->recipe.active = false; LOGI(TAG, "recipe stopped by user"); }
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
		forget_learning(c, (unsigned)cmd->aux, cmd->str[0] ? cmd->str : "asked for");
		break;
	default: break;
	}
}

/* ------------------------------------------------------------------ recipe runner */

static void recipe_begin_step(pf_control *c, double now)
{
	pf_recipe_step *s = &c->recipe.r.steps[c->recipe.step];
	c->recipe.step_start = now;
	c->recipe.triggered = false;
	c->recipe.waiting = false;
	if (s->setpoint_c > 0) c->setpoint_c = s->setpoint_c;
	c->s_plus = s->s_plus;
	LOGI(TAG, "recipe '%s' step %d/%d: %s", c->recipe.r.name, c->recipe.step + 1, c->recipe.r.nsteps, pf_mode_name(s->mode));
	if (s->message[0]) pf_events_emit("Recipe_Step_Message", c->recipe.r.name, "%s", s->message);
	switch (s->mode) {
	case PF_MODE_STARTUP:
		c->next_mode = c->recipe.step + 1 < c->recipe.r.nsteps ? c->recipe.r.steps[c->recipe.step + 1].mode : c->cfg.after_startup_mode;
		if (c->next_mode != PF_MODE_SMOKE && c->next_mode != PF_MODE_HOLD) c->next_mode = PF_MODE_SMOKE;
		if (c->mode != PF_MODE_STARTUP) enter_mode(c, PF_MODE_STARTUP, now);
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
	if (++c->recipe.step >= c->recipe.r.nsteps) {
		pf_events_emit("Recipe_Complete", c->recipe.r.name, "Recipe finished.");
		c->recipe.active = false;
		return;
	}
	recipe_begin_step(c, now);
}

static void run_recipe(pf_control *c, double now)
{
	if (!c->recipe.active) return;
	if (c->mode == PF_MODE_ERROR) { c->recipe.active = false; return; }
	pf_recipe_step *s = &c->recipe.r.steps[c->recipe.step];
	if (c->recipe.waiting) return;
	if (!c->recipe.triggered) {
		bool trig = false;
		if (s->mode == PF_MODE_STARTUP) trig = c->mode != PF_MODE_STARTUP && c->mode != PF_MODE_REIGNITE && c->mode != PF_MODE_PRIME;
		else if (s->mode == PF_MODE_SHUTDOWN) trig = c->mode == PF_MODE_STOP;
		else if (s->mode == PF_MODE_STOP) trig = true;
		else {
			bool in_mode = c->mode == s->mode;
			if (in_mode && s->timer_s > 0 && now - c->recipe.step_start >= s->timer_s) trig = true;
			if (in_mode && s->probe_temp_c > 0) {
				int i = pf_probes_find(&c->sensors, s->probe);
				if (i >= 0 && c->sensors.p[i].valid && c->sensors.p[i].temp_c >= s->probe_temp_c) trig = true;
			}
			if (in_mode && s->timer_s <= 0 && s->probe_temp_c <= 0) trig = true; /* nothing to wait for */
		}
		if (!trig) return;
		c->recipe.triggered = true;
		if (s->pause) {
			c->recipe.waiting = true;
			pf_events_emit("Recipe_Step_Done", c->recipe.r.name, "Step %d is done - tap Next to continue.", c->recipe.step + 1);
			return;
		}
	}
	recipe_advance(c, now);
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

/* passive FOPDT: watch the rise from STARTUP entry until the pit first settles near the set point */
static void learn_rise_begin(pf_control *c, double now)
{
	c->learn.rise_active = pf_learning_enabled() && c->pit_valid;
	c->learn.rise_t0 = 0; c->learn.rise_T0_c = c->pit_c; c->learn.rise_u_sum = 0; c->learn.rise_n = 0;   /* clock starts at ignition, see learn_rise_track */
	c->learn.rise_t28 = c->learn.rise_t63 = 0;
}

static void learn_rise_track(pf_control *c, double now)
{
	if (!c->learn.rise_active) return;
	if (c->mode == PF_MODE_STOP || c->mode == PF_MODE_ERROR || c->mode == PF_MODE_SHUTDOWN) { c->learn.rise_active = false; return; }
	/* the step test starts when the fire is evidently lit (+3 C over the startup baseline), so the ignition
	 * delay does not masquerade as plant dead time */
	if (c->learn.rise_t0 == 0) { if (c->pit_valid && c->pit_c >= c->learn.rise_T0_c + 3.0) { c->learn.rise_t0 = now; c->learn.rise_T0_c = c->pit_c; } return; }
	if (now - c->learn.rise_t0 > 3600) { c->learn.rise_active = false; return; }
	if (c->mode != PF_MODE_HOLD) return;
	double span = c->setpoint_c - c->learn.rise_T0_c;
	if (span < 20) { c->learn.rise_active = false; return; }
	double frac = (c->pit_c - c->learn.rise_T0_c) / span;
	if (!c->learn.rise_t28 && frac >= 0.283) c->learn.rise_t28 = now - c->learn.rise_t0;
	if (!c->learn.rise_t63 && frac >= 0.632) c->learn.rise_t63 = now - c->learn.rise_t0;
	if (c->learn.rise_t63 && c->learn.rise_t28 && fabs(c->pit_c - c->setpoint_c) < 3.0) {
		double tau = 1.5 * (c->learn.rise_t63 - c->learn.rise_t28);
		double theta = c->learn.rise_t63 - tau;
		if (theta < 5) theta = 5;
		double u_mean = c->learn.rise_n ? c->learn.rise_u_sum / c->learn.rise_n : c->cfg.u_max;
		double K = u_mean > 0.05 ? span / u_mean : 0;
		if (tau > 30 && tau < 3600 && K > 0) {
			pf_learning_store_fopdt(K, tau, theta);
			/* Nest-style: hand the fresh plant model straight to the controller so its gains track the grill */
			if (pf_learning_enabled() && c->cinst && c->cops->apply_tuning) {
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
	if (A <= c->autotune.hyst_c * 1.05) { pf_events_emit("Autotune_Failed", "Autotune stopped", "Oscillation too small to measure."); return; }
	bool settled = autotune_settled(c);
	/* Half the swing the grill really saw. Where nothing was clamped this is the h it was asked
	 * for; where the low half hit the minimum feed it is smaller, and using the requested h there
	 * would overstate the ultimate gain and hand back a proportional band that is too narrow. */
	double h_eff = c->autotune.h;
	if (c->autotune.hi_n > 0 && c->autotune.lo_n > 0) {
		double hi = c->autotune.hi_sum / c->autotune.hi_n, lo = c->autotune.lo_sum / c->autotune.lo_n;
		if (hi - lo > 0.01) h_eff = (hi - lo) / 2.0;
	}
	/* Ultimate gain from the describing function of a relay with hysteresis. Taking 4h/(pi*A)
	 * alone gives the gain at the point the relay actually identifies, which sits a little short
	 * of the -180 degree crossing because the hysteresis adds phase lag. Projecting onto the real
	 * axis with sqrt(A^2 - eps^2) is the ultimate gain the tuning rules are written against; the
	 * difference is a couple of percent on a healthy swing and a quarter on a marginal one. */
	double eps = c->autotune.hyst_c;
	double denom = sqrt(A * A - eps * eps);
	double Ku = 4.0 * h_eff / (M_PI * (denom > 0.1 ? denom : A));
	pf_autotune_result r = { .Ku = Ku, .Pu = Pu, .amplitude_c = A };

	/* Turn the measurement into the grill's model rather than straight into a tuning. The static
	 * gain is the one thing a relay test cannot see, so it comes from the feed-forward fit, which
	 * measures the steady feed this grill needs per degree across cooks, and failing that from the
	 * last startup rise. The model is then filed like any other, and the tuning comes out of it by
	 * the same rule a passively fitted model does. */
	pf_ff_fit ff = pf_learning_fit();
	pf_fopdt plant = pf_learning_fopdt();
	double K = ff.n >= 3 && ff.b > 1e-5 ? 1.0 / ff.b : plant.valid ? plant.K : 0;
	double tau = 0, theta = 0;
	const char *K_src = ff.n >= 3 && ff.b > 1e-5 ? "feed-forward" : "startup rise";
	if (K > 0 && pf_plant_from_relay(Ku, Pu, K, &tau, &theta)) {
		pf_learning_store_fopdt(K, tau, theta);
		pf_tuning_from_plant(K, tau, theta, &r.PB_c, &r.Ti, &r.Td);
		LOGI(TAG, "relay -> plant: K %.0f C per unit feed (%s), tau %.0f s, theta %.0f s", K, K_src, tau, theta);
	} else if (plant.valid) {
		/* the relay and the static gain cannot describe a plant together; keep what is known */
		pf_tuning_from_plant(plant.K, plant.tau, plant.theta, &r.PB_c, &r.Ti, &r.Td);
		LOGW(TAG, "relay result does not describe a first-order plant with K %.0f; keeping the fitted model", K);
	}
	if (!(r.PB_c > 0) || !(r.Ti > 0)) {
		pf_events_emit("Autotune_Failed", "Autotune stopped",
		               "The oscillation could not be turned into a model of the grill. Let it run an ordinary cook or two first, so the feed it needs per degree is known.");
		return;
	}
	pf_learning_store_autotune(&r);
	bool applied = false;
	if (pf_learning_enabled() && c->cinst && c->cops->apply_tuning) {
		/* hand over the model, not the raw oscillation: the controller designs from the same three
		 * numbers whichever measurement produced them */
		pf_fopdt m = pf_learning_fopdt();
		if (m.valid) { c->cops->apply_tuning(c->cinst, Ku, Pu, m.K, m.tau, m.theta); applied = true; }
	}
	pf_events_emit("Autotune_Done", "Autotune complete",
	               "Ku %.3f (swing ±%.2f duty), period %.0f s over %d cycle%s%s, amplitude ±%.1f. PB %.0f (%s), Ti %.0f s%s.",
	               Ku, h_eff, Pu, k, k == 1 ? "" : "s", settled ? "" : " (still drifting)",
	               pf_delta_from_c(A, c->cfg.units), pf_delta_from_c(r.PB_c, c->cfg.units),
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
static double recent_hold_duty(const pf_control *c, double now, double window_s)
{
	const pf_history *h = pf_history_ctrl_view();
	if (!h) return NAN;
	/* Wide enough to admit a grill that is holding but still swinging -- which is every grill this
	 * test has not run on yet -- and narrow enough to exclude the climb that precedes it. */
	double band = pf_delta_to_c(10, PF_UNITS_F);
	double sum = 0; int n = 0;
	for (int i = h->len - 1; i >= 0; i--) {
		const pf_hist_pt *pt = pf_history_at(h, i);
		if (!pt || now - pt->t > window_s) break;
		if (isnan(pt->u_applied) || pt->u_applied <= 0) continue;
		if (isnan(pt->pit_c) || pt->setpoint_c <= 0) continue;
		if (fabs(pt->pit_c - pt->setpoint_c) > band) continue;
		sum += pt->u_applied;
		n++;
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
	double measured = recent_hold_duty(c, now, 1800);
	c->autotune.u_center = pf_clamp(!isnan(measured) ? measured : c->learn.u_ff > 0 ? c->learn.u_ff : c->u_applied,
	                                c->cfg.u_min + 0.05, c->cfg.u_max - 0.05);

	/* Size the swing to fit between the minimum and maximum feed. The relay's maths assumes a
	 * symmetric square wave about the centre, and a grill that holds a low set point on very
	 * little fuel has almost no room below it: asking for a swing that gets clamped on one side
	 * gives a lopsided input and an ultimate gain that is too high. Shrink the swing instead, and
	 * only if that leaves too little to measure, move the centre up to make room. */
	double h = fmin(0.15, fmin(c->autotune.u_center - c->cfg.u_min, c->cfg.u_max - c->autotune.u_center));
	if (h < 0.05) {
		c->autotune.u_center = pf_clamp(c->cfg.u_min + 0.05, c->cfg.u_min + 0.05, c->cfg.u_max - 0.05);
		h = fmin(0.05, fmin(c->autotune.u_center - c->cfg.u_min, c->cfg.u_max - c->autotune.u_center));
	}
	c->autotune.h = h;
	c->autotune.hyst_c = 1.0;
	c->autotune.start_t = now;
	c->autotune.last_cross_t = now;
	c->autotune.phase = c->pit_c > c->setpoint_c ? -1 : +1;
	c->autotune.err_at_move = c->pit_c - c->setpoint_c;
	c->autotune.peak_max = c->autotune.peak_min = c->pit_c;
	pf_events_emit("Autotune_Started", "Autotune running", "The grill will oscillate a few degrees around %.0f for 15-40 minutes. Do not cook food during the test.", pf_from_c(c->setpoint_c, c->cfg.units));
	pf_cycle_begin(&c->cycle, &c->ccfg, now, c->autotune.u_center + c->autotune.h * c->autotune.phase);
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
static bool autotune_settled(const pf_control *c)
{
	int n = c->autotune.crossings < PF_AT_MAX ? c->autotune.crossings : PF_AT_MAX;
	if (n < 4) return false;
	double p1, a1, p2, a2;
	autotune_cycle(c, n - 2, &p1, &a1);
	autotune_cycle(c, n - 1, &p2, &a2);
	double pmax = fmax(p1, p2), amax = fmax(a1, a2);
	if (pmax <= 0 || amax <= 0) return false;
	return fabs(p1 - p2) <= 0.25 * pmax && fabs(a1 - a2) <= 0.30 * amax;
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
		c->autotune.last_cross_t = now;
		c->autotune.peak_max = c->autotune.peak_min = c->pit_c;
		/* Stop once the oscillation has settled, not merely once enough of it has gone by. A limit
		 * cycle that is still growing describes the transient, not the plant, and averaging it
		 * yields a period that belongs to no real oscillation. Give it room to settle, and take
		 * what it has at the cap either way. */
		if (c->autotune.crossings >= PF_AT_MIN_CROSS &&
		    (autotune_settled(c) || c->autotune.crossings >= PF_AT_MAX)) {
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
			return c->autotune.u_center + c->autotune.h * c->autotune.phase;
		}
		c->autotune.u_center = moved;
		c->autotune.h = fmin(0.15, fmin(c->autotune.u_center - c->cfg.u_min, c->cfg.u_max - c->autotune.u_center));
		if (c->autotune.h < 0.05) c->autotune.h = 0.05;
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
		return c->autotune.u_center + c->autotune.h * c->autotune.phase;
	}
	/* Only give up on a runaway once there is nothing left to try. Falling out of the block above
	 * merely because the grill has not had time to answer the last move is not a failure. */
	if (ran_away && c->autotune.recentres >= 8) { autotune_finish(c, false, "The pit would not stay near the set point."); return c->cfg.u_min; }
	if (now - c->autotune.last_cross_t > 1800) { autotune_finish(c, false, "The grill would not oscillate around the set point."); return c->autotune.u_center; }
	return c->autotune.u_center + c->autotune.h * c->autotune.phase;
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
		double sched_PB = 0, sched_Ti = 0, sched_Td = 0;
		if (c->cfg.use_library) pf_learning_gains(c->setpoint_c, &sched_PB, &sched_Ti, &sched_Td);
		pf_ctrl_in in = {
			.now_s = now, .pit_c = c->pit_c, .setpoint_c = c->setpoint_c, .ambient_c = c->ambient_c,
			.u_prev_raw = c->u_raw, .u_prev_applied = c->u_applied, .u_ff = c->learn.u_ff, .saturated = c->saturated,
			.sched_PB_c = sched_PB, .sched_Ti = sched_Ti, .sched_Td = sched_Td,
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
	else if (typed_changed) forget_learning(c, PF_FORGET_REFINEMENT, "the starting values were changed");
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
	s.ambient_c = c->ambient_c;
	s.reignite_retries_left = c->safety.reignite_retries_left;
	s.sensors = c->sensors;
	s.hopper_pct = c->hopper_pct;
	s.sim = c->sim;
	for (int i = 0; i < c->sensors.n && i < PF_MAX_PROBES; i++) {
		const pf_notify_probe *p = pf_notify_find(&c->notify, c->sensors.p[i].label);
		s.notify[i].after = p ? p->after : 0;
		s.notify[i].eta_s = p ? p->eta_s : -1;
		s.notify[i].limit_high_c = p ? p->limit_high_c : 0;
		s.notify[i].limit_low_c = p ? p->limit_low_c : 0;
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
		s.recipe.nsteps = c->recipe.r.nsteps;
		s.recipe.waiting = c->recipe.waiting;
		s.recipe.step_mode = rs->mode;
		s.recipe.remaining_s = rs->timer_s > 0 ? fmax(0, rs->timer_s - (now - c->recipe.step_start)) : -1;
		pf_strlcpy(s.recipe.message, rs->message, sizeof s.recipe.message);
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
