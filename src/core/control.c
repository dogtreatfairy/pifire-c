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
#include "platform/sim.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "control"

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
	g->error_cooldown_fan_s = N("safety.error_cooldown_fan_s", 300);
	g->coldstart = B("safety.coldstart.enabled", false);
	g->coldstart_delta_c = D("safety.coldstart.delta_rise", 12);
	g->coldstart_timeout_s = N("safety.coldstart.timeout_s", 0);
	g->coldstart_window_s = N("safety.coldstart.baseline_window_s", 60);

	g->startup_duration_s = N("startup.duration", 240);
	g->prime_on_startup_g = N("startup.prime_on_startup", 0);
	double exit_user = N("startup.startup_exit_temp", 0);
	g->startup_exit_c = exit_user > 0 ? pf_to_c(exit_user, u) : 0;
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
	c->ctrl_reset_needed = true;
	c->safety.ctrl_fault_count = 0;
	LOGI(TAG, "controller '%s' loaded", ops->id);
	return 0;
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

/* ------------------------------------------------------------------ mode transitions */

static void enter_mode(pf_control *c, pf_mode m, double now)
{
	pf_mode prev = c->mode;
	c->mode = m;
	c->mode_start = now;
	c->lid_open = false;
	c->fan_pid_active = false;
	c->fan_ramping = false;
	c->target_reached = false;
	memset(c->manual_until, 0, sizeof c->manual_until);
	pf_cycle_stop(&c->cycle);
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
		if (m == PF_MODE_STARTUP) {
			c->cook_start_wall = pf_wall();
			c->auger_total_on_s = 0;
			pf_safety_reset(c);
			if (c->cfg.clear_history_on_startup && prev == PF_MODE_STOP) pf_history_clear();
			if (!c->ambient_from_probe) c->ambient_c = c->pit_c; /* provisional; refined by cold-start baseline */
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
		if (c->cfg.prime_on_startup_g > 0 && c->mode == PF_MODE_STOP) {
			c->prime_amount_g = c->cfg.prime_on_startup_g;
			c->next_mode = PF_MODE_STARTUP;
			enter_mode(c, PF_MODE_PRIME, now);
			return;
		}
	}
	if (m == PF_MODE_HOLD && c->mode == PF_MODE_STOP) {
		/* Hold from cold: run startup first, then hold */
		c->next_mode = PF_MODE_HOLD;
		enter_mode(c, PF_MODE_STARTUP, now);
		return;
	}
	if (m == PF_MODE_SMOKE && c->mode == PF_MODE_STOP) {
		c->next_mode = PF_MODE_SMOKE;
		enter_mode(c, PF_MODE_STARTUP, now);
		return;
	}
	if (m == PF_MODE_HOLD && c->mode == PF_MODE_HOLD) {
		/* setpoint change only */
		c->target_reached = false;
		if (c->cinst) { c->ctrl_reset_needed = true; }
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
		c->req_pending = false;
		c->safety.error_code[0] = 0; c->safety.error_msg[0] = 0;
		break;
	case PF_CMD_MODE:
		pf_control_request(c, cmd->mode, cmd->num > 0 ? pf_to_c(cmd->num, u) : 0);
		break;
	case PF_CMD_SETPOINT:
		if (cmd->num > 0) {
			c->setpoint_c = pf_to_c(cmd->num, u);
			c->target_reached = false;
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
		if (c->mode != PF_MODE_MANUAL && !c->cfg.allow_manual) { LOGW(TAG, "manual output change refused (not in Manual mode)"); break; }
		double until = c->mode == PF_MODE_MANUAL ? 1e18 : now + c->cfg.manual_override_s;
		if (!strcmp(cmd->str, "pwm")) { pf_outputs_fan_pct((int)cmd->num); c->manual_until[PF_OUT_FAN] = until; break; }
		for (int i = 0; i < PF_OUT_COUNT; i++)
			if (!strcmp(cmd->str, pf_output_name((pf_output)i))) {
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
	default: break;
	}
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

static void run_hold_cycle(pf_control *c, double now)
{
	if (!pf_cycle_done(&c->cycle, now)) return;
	double u;
	if (c->lid_open || !c->cinst) {
		u = c->cfg.u_min;
	} else {
		pf_ctrl_in in = {
			.now_s = now, .pit_c = c->pit_c, .setpoint_c = c->setpoint_c, .ambient_c = c->ambient_c,
			.u_prev_raw = c->u_raw, .u_prev_applied = c->u_applied, .saturated = c->saturated,
			.cycle_time_s = c->ccfg.cycle_s, .u_min = c->ccfg.u_min, .u_max = c->ccfg.u_max,
			.target_reached = c->target_reached, .fan_on = pf_outputs_get(PF_OUT_FAN), .fan_pct = pf_outputs_get_fan_pct(),
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
		if ((timer && pf_safety_startup_can_finish(c, now)) || exit_temp) {
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
	if (strcmp(old_id, c->cfg.controller_id)) controller_load(c, c->cfg.controller_id);
	else if (c->cinst && c->cops->configure) {
		char *cfg = controller_config_json(c->cops->id, c->cfg.units);
		c->cops->configure(c->cinst, cfg);
		free(cfg);
		c->ctrl_reset_needed = true;
	}
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

static void read_sensors(pf_control *c)
{
	pf_probes_snapshot(&c->sensors);
	int p = c->sensors.primary;
	if (p >= 0 && c->sensors.p[p].valid) { c->pit_c = c->sensors.p[p].temp_c; c->pit_valid = true; }
	else c->pit_valid = false;
	c->ambient_from_probe = false;
	for (int i = 0; i < c->sensors.n; i++)
		if (c->sensors.p[i].ambient && c->sensors.p[i].valid) { c->ambient_c = c->sensors.p[i].temp_c; c->ambient_from_probe = true; break; }
	if (!c->ambient_from_probe && c->safety.coldstart_active && !isnan(c->safety.baseline_c)) c->ambient_c = c->safety.baseline_c;
}

static void publish(pf_control *c, double now)
{
	pf_status s;
	memset(&s, 0, sizeof s);
	s.t = now;
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
	s.u_applied = c->u_applied;
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
