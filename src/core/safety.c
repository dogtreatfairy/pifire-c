#include "core/control.h"
#include "core/db.h"
#include "core/events.h"
#include "features/alarms.h"
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
	pf_events_emit(code, "Grill error", "%s", s->error_msg);
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

	/* 5. dynamic flame-out assist.
	 *
	 * The fixed floor below is the last word: by the time the pit has fallen that far the fire is
	 * out and the grill has to start again. Long before that, a pit sliding away from a set point
	 * it was holding is a fire that is failing, and the cheapest answer is the igniter -- it costs
	 * nothing but electricity and it catches the fire before there is nothing left to catch.
	 *
	 * It stays on until the pit has climbed back a little way from the lowest point it reached,
	 * rather than until it is back at the set point: recovery is the evidence that the fire has
	 * taken, and waiting for the whole way back would hold the igniter on through the entire
	 * recovery. The lowest point keeps moving down while the pit is still falling, so the test is
	 * always against the bottom of this dip and not the one before it. The igniter's own
	 * continuous-on cap above still applies and still wins.
	 *
	 * It only applies to a pit that had arrived. A grill on its way up to a set point, or climbing
	 * to a new one, is far below it for entirely ordinary reasons, and lighting the igniter for
	 * that would fire on every cook -- the same mistake the running-cold rule made before it
	 * learned to wait for a stall. target_reached is cleared when the set point changes, so a step
	 * up re-arms it exactly as a fresh cook does. An open lid is excluded for the same reason from
	 * the other end: the pit falls twenty degrees because the heat walked out, not because the
	 * fire went out, and the igniter has nothing to fix. */
	if (m == PF_MODE_HOLD && cfg->relight_enabled && c->pit_valid && c->setpoint_c > 0 &&
	    c->target_reached && !c->lid_open) {
		double gap = c->setpoint_c - c->pit_c;
		/* Coming back counts only as climbing back towards the set point, not as the small rise
		 * the igniter itself can produce. Judging recovery by the rise off the lowest point is
		 * right for switching the igniter off -- that rise is the fire taking -- but it is the
		 * wrong clock to escalate on: the assist cycles on and off while the pit hovers, and a
		 * deadline that restarted on every cycle would never expire with the fire still out. */
		if (gap <= cfg->relight_drop_c / 2) {
			s->relight_below_since = 0;
			if (s->relight_active) {
				s->relight_active = false;
				pf_outputs_set(PF_OUT_IGNITER, false);
				pf_alarms_clear("SAFETY:relight");
			}
		} else if (gap >= cfg->relight_drop_c || s->relight_below_since > 0) {
			if (s->relight_below_since == 0) s->relight_below_since = now;
			if (now - s->relight_below_since > cfg->relight_timeout_s) {
				/* A rescue is an attempt, not a way to run. The igniter has had its window and the
				 * pit has not climbed back, so the fire is out rather than struggling: hand it to
				 * the flame-out path, which knows how to start the grill again and when to stop
				 * trying. Without this the assist would hold the igniter on until its own cap and
				 * keep the pit just warm enough that nothing else noticed. */
				s->relight_active = false;
				s->relight_below_since = 0;
				pf_outputs_set(PF_OUT_IGNITER, false);
				pf_alarms_clear("SAFETY:relight");
				LOGW(TAG, "pit stayed %.0f C below the set point for %.0f s: treating it as a flame-out",
				     gap, cfg->relight_timeout_s);
				return flameout(c);
			}
			if (!s->relight_active) {
				if (!s->igniter_locked_out) {
					s->relight_active = true;
					s->relight_low_c = c->pit_c;
					pf_outputs_set(PF_OUT_IGNITER, true);
					LOGW(TAG, "pit %.0f C is %.0f C below the %.0f C set point: igniter on to catch the fire",
					     c->pit_c, gap, c->setpoint_c);
					pf_alarms_raise("SAFETY:relight", "W08_RELIGHT", "Flame-out protection", PF_CRIT_HIGH, PF_SINK_ALL,
					                "Flame-out protection",
					                "The pit fell away from the set point, so the igniter is on until the fire catches.");
					if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "W08_RELIGHT", "Pit fell away from the set point; igniter on");
				}
			} else {
				if (c->pit_c < s->relight_low_c) s->relight_low_c = c->pit_c;
				if (c->pit_c >= s->relight_low_c + cfg->relight_recover_c) {
					/* the fire has taken: stop feeding it electricity and let the grill work */
					s->relight_active = false;
					pf_outputs_set(PF_OUT_IGNITER, false);
					LOGI(TAG, "pit recovered to %.0f C from a low of %.0f C: igniter off", c->pit_c, s->relight_low_c);
					pf_alarms_clear("SAFETY:relight");
				} else if (s->igniter_locked_out) {
					s->relight_active = false;   /* the cap has taken it; stop claiming otherwise */
					pf_alarms_clear("SAFETY:relight");
				} else {
					pf_outputs_set(PF_OUT_IGNITER, true);
				}
			}
		}
	} else if (s->relight_active || s->relight_below_since > 0) {
		s->relight_active = false;
		s->relight_below_since = 0;
		pf_outputs_set(PF_OUT_IGNITER, false);
		pf_alarms_clear("SAFETY:relight");
	}

	/* 6. flame-out in SMOKE / HOLD */
	if ((m == PF_MODE_SMOKE || m == PF_MODE_HOLD) && cfg->startup_check && s->floor_set && c->pit_c < s->floor_c)
		return flameout(c);

	return 0;
}
