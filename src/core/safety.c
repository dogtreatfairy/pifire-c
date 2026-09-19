#include "core/control.h"
#include "core/db.h"
#include "core/log.h"
#include "core/outputs.h"
#include "core/util.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TAG "safety"

void pf_safety_set_error(pf_control *c, const char *code, const char *fmt, ...)
{
	pf_safety *s = &c->safety;
	pf_strlcpy(s->error_code, code, sizeof s->error_code);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->error_msg, sizeof s->error_msg, fmt, ap);
	va_end(ap);
	LOGE(TAG, "%s: %s", code, s->error_msg);
	if (pf_db_handle()) pf_db_event(PF_LVL_ERROR, code, s->error_msg);
}

void pf_safety_reset(pf_control *c)
{
	pf_safety *s = &c->safety;
	memset(s, 0, sizeof *s);
	s->reignite_retries_left = c->cfg.reignite_retries;
	s->baseline_c = NAN;
	s->filt_c = NAN;
	s->floor_c = NAN;
}

/* Classic floor: clamp(0.9 * T_F, minstartup, maxstartup), computed in Fahrenheit like the original. */
static double classic_floor_c(const pf_cfg *cfg, double t_c)
{
	double f = pf_c_to_f(t_c) * 0.9;
	f = pf_clamp(f, pf_c_to_f(cfg->min_startup_c), pf_c_to_f(cfg->max_startup_c));
	return pf_f_to_c(f);
}

void pf_safety_on_startup_enter(pf_control *c, double now)
{
	pf_safety *s = &c->safety;
	const pf_cfg *cfg = &c->cfg;
	s->floor_c = classic_floor_c(cfg, c->pit_c);
	s->floor_set = true;
	s->igniter_on_since = 0;
	s->igniter_locked_out = false;
	s->above_count = 0;
	s->filt_c = c->pit_c;
	s->coldstart_active = false;
	s->coldstart_reached = false;
	if (cfg->coldstart) {
		if (cfg->startup_exit_c > 0 && c->pit_c >= cfg->startup_exit_c) {
			LOGI(TAG, "cold-start skipped: grill already at %.0f C (>= exit temp)", c->pit_c);
		} else {
			s->coldstart_active = true;
			s->baseline_c = c->pit_c;
			s->baseline_window_end = now + cfg->coldstart_window_s;
			double timeout = cfg->coldstart_timeout_s > 0 ? cfg->coldstart_timeout_s : c->startup_duration_s;
			s->coldstart_deadline = now + timeout;
			LOGI(TAG, "cold-start armed: baseline %.1f C, need +%.1f C within %.0f s", s->baseline_c, cfg->coldstart_delta_c, timeout);
		}
	}
	LOGI(TAG, "startup floor set to %.1f C", s->floor_c);
}

void pf_safety_on_startup_exit(pf_control *c, double now)
{
	(void)now;
	pf_safety *s = &c->safety;
	const pf_cfg *cfg = &c->cfg;
	if (s->coldstart_active) {
		/* no minstartuptemp clamp here: a cold grill legitimately exits startup below it */
		double exit_based = fmin(pf_f_to_c(pf_c_to_f(c->pit_c) * 0.9), cfg->max_startup_c);
		double cold = s->baseline_c + cfg->coldstart_delta_c;
		s->floor_c = fmax(cold, exit_based);
		s->coldstart_active = false;
		LOGI(TAG, "cold-start complete; flame-out floor %.1f C", s->floor_c);
	}
}

bool pf_safety_startup_can_finish(pf_control *c, double now)
{
	(void)now;
	return !c->safety.coldstart_active || c->safety.coldstart_reached;
}

static int flameout(pf_control *c)
{
	pf_safety *s = &c->safety;
	if (s->reignite_retries_left <= 0) {
		pf_safety_set_error(c, "E02_FLAMEOUT", "Pit temperature %.0f C fell below the startup floor %.0f C and no re-ignite retries remain",
		                    c->pit_c, s->floor_c);
		return PF_MODE_ERROR;
	}
	s->reignite_retries_left--;
	s->reignite_last = c->mode;
	LOGW(TAG, "possible flame-out (%.0f C < floor %.0f C): re-igniting (%d retries left)", c->pit_c, s->floor_c, s->reignite_retries_left);
	if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "W03_REIGNITE", "Possible flame-out detected, re-igniting");
	return PF_MODE_REIGNITE;
}

int pf_safety_tick(pf_control *c, double now)
{
	pf_safety *s = &c->safety;
	const pf_cfg *cfg = &c->cfg;
	pf_mode m = c->mode;
	bool active = m == PF_MODE_STARTUP || m == PF_MODE_REIGNITE || m == PF_MODE_SMOKE || m == PF_MODE_HOLD;

	/* 1. over-temperature: every mode except STOP */
	if (m != PF_MODE_STOP && m != PF_MODE_ERROR && c->pit_valid && c->pit_c > cfg->max_temp_c) {
		pf_safety_set_error(c, "E01_OVERTEMP", "Pit temperature %.0f exceeded the maximum %.0f", c->pit_c, cfg->max_temp_c);
		return PF_MODE_ERROR;
	}

	/* 2. primary probe fault while we depend on it */
	if (active) {
		if (!c->pit_valid) {
			if (s->primary_invalid_since == 0) s->primary_invalid_since = now;
			else if (now - s->primary_invalid_since > cfg->probe_fault_s) {
				pf_safety_set_error(c, "E05_PROBE_FAULT", "Primary probe gave no valid reading for %.0f s", cfg->probe_fault_s);
				return PF_MODE_ERROR;
			}
			return 0; /* nothing else can be judged without a reading */
		}
		s->primary_invalid_since = 0;
	}

	/* 3. igniter continuous-on cap */
	if (pf_outputs_get(PF_OUT_IGNITER)) {
		if (s->igniter_on_since == 0) s->igniter_on_since = now;
		else if (now - s->igniter_on_since > cfg->igniter_max_on_s && !s->igniter_locked_out) {
			s->igniter_locked_out = true;
			pf_outputs_set(PF_OUT_IGNITER, false);
			LOGW(TAG, "igniter on for %.0f s: forced off for the rest of this mode", cfg->igniter_max_on_s);
			if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "W07_IGNITER_CAP", "Igniter exceeded maximum on time and was switched off");
		}
	} else {
		s->igniter_on_since = 0;
	}

	/* 4. cold-start progress (STARTUP / REIGNITE) */
	if ((m == PF_MODE_STARTUP || m == PF_MODE_REIGNITE) && s->coldstart_active) {
		/* 30 s single-pole filter at the 100 ms tick */
		double dt = c->last_step > 0 ? now - c->last_step : 0.1;
		double a = dt / 30.0; if (a > 1) a = 1;
		s->filt_c = isnan(s->filt_c) ? c->pit_c : s->filt_c + (c->pit_c - s->filt_c) * a;
		if (now < s->baseline_window_end) {
			if (s->filt_c < s->baseline_c) s->baseline_c = s->filt_c;
		} else if (!s->coldstart_reached) {
			if (s->filt_c >= s->baseline_c + cfg->coldstart_delta_c) {
				if (++s->above_count >= 2) { s->coldstart_reached = true; LOGI(TAG, "cold-start: temperature rise confirmed (%.1f C)", s->filt_c); }
			} else s->above_count = 0;
			if (!s->coldstart_reached && now > s->coldstart_deadline) {
				if (s->reignite_retries_left > 0) {
					s->reignite_retries_left--;
					s->reignite_last = c->cfg.after_startup_mode;
					LOGW(TAG, "cold-start: no rise within timeout, re-igniting (%d left)", s->reignite_retries_left);
					if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "W04_STARTUP_RETRY", "Startup did not raise the pit temperature in time; retrying");
					return PF_MODE_REIGNITE;
				}
				pf_safety_set_error(c, "E04_STARTUP_FAILED", "Pit temperature did not rise %.0f degrees above the %.0f C baseline within the startup timeout",
				                    cfg->coldstart_delta_c, s->baseline_c);
				return PF_MODE_ERROR;
			}
		}
	}

	/* 5. flame-out in SMOKE / HOLD */
	if ((m == PF_MODE_SMOKE || m == PF_MODE_HOLD) && cfg->startup_check && s->floor_set && c->pit_c < s->floor_c)
		return flameout(c);

	return 0;
}
