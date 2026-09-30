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
	pf_events_emit(code, "Grill Error", "%s", s->error_msg);
}

void pf_safety_reset(pf_control *c)
{
	pf_safety *s = &c->safety;
	memset(s, 0, sizeof *s);
	s->reignite_retries_left = c->cfg.reignite_retries;
	s->baseline_c = NAN;
	s->filt_c = NAN;
	s->floor_c = NAN;
	s->hold_peak_c = NAN;
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
	s->hot_relight = false;
	if (cfg->coldstart) {
		/* A hot grill is not a cold start: a relight after a power blip, or a new set point on a
		 * grill that is already running, cannot be asked to climb another twelve degrees on cue. The
		 * check is for a cold pot -- below 140 F, or the startup exit temperature if that is higher.
		 * A fire that fails on a hot grill is the flame-out protection's to catch. */
		double hot_c = fmax(pf_f_to_c(140), cfg->startup_exit_c);
		if (c->pit_c >= hot_c) {
			LOGI(TAG, "smart start skipped: grill already at %.0f C", c->pit_c);
			if (c->mode == PF_MODE_REIGNITE) {
				s->hot_relight = true;
				s->hot_relight_low_c = c->pit_c;
				s->hot_relight_deadline = now + (cfg->coldstart_timeout_s > 0 ? cfg->coldstart_timeout_s : 300);
			}
		} else {
			s->coldstart_active = true;
			s->baseline_c = c->pit_c;
			s->baseline_window_end = now + cfg->coldstart_window_s;
			double timeout = cfg->coldstart_timeout_s > 0 ? cfg->coldstart_timeout_s : c->startup_duration_s;
			s->coldstart_deadline = now + timeout;
			LOGI(TAG, "smart start armed: baseline %.1f C, need +%.1f C within %.0f s", s->baseline_c, cfg->coldstart_delta_c, timeout);
		}
	}
	LOGI(TAG, "startup floor set to %.1f C", s->floor_c);
}

/* Hold and Smoke always have a flame-out floor. One entered without a startup behind it -- after a
 * restart, a resume that did not carry one -- had none, and the floor check never ran. */
void pf_safety_ensure_floor(pf_control *c)
{
	if (c->safety.floor_set) return;
	c->safety.floor_c = classic_floor_c(&c->cfg, c->pit_c);
	c->safety.floor_set = true;
	LOGI(TAG, "flame-out floor set to %.1f C", c->safety.floor_c);
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
		LOGI(TAG, "smart start complete; flame-out floor %.1f C", s->floor_c);
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

	/* 1b. igniter cap -- before the probe check, which returns early while the probe is invalid: the
	 * igniter must be capped whether or not the pit can be read. Once it has tripped in a mode, the
	 * igniter is refused for the rest of that mode, whoever switches it back on. */
	if (pf_outputs_get(PF_OUT_IGNITER)) {
		if (s->igniter_locked_out) {
			pf_outputs_set(PF_OUT_IGNITER, false);
		} else if (s->igniter_on_since == 0) s->igniter_on_since = now;
		else if (now - s->igniter_on_since > cfg->igniter_max_on_s) {
			s->igniter_locked_out = true;
			pf_outputs_set(PF_OUT_IGNITER, false);
			LOGW(TAG, "igniter on for %.0f s: forced off for the rest of this mode", cfg->igniter_max_on_s);
			if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "W07_IGNITER_CAP", "Igniter exceeded maximum on time and was switched off");
		}
	} else {
		s->igniter_on_since = 0;
	}

	/* 1d. a relight of a hot grill must show a rise, as a cold start must: the lowest pit since the
	 * relight began, and three degrees above it within the Smart Start time. Otherwise a relight
	 * that did not take ran its whole startup feeding a dead pot and went back to Hold. */
	if (m == PF_MODE_REIGNITE && s->hot_relight && c->pit_valid) {
		if (c->pit_c < s->hot_relight_low_c) s->hot_relight_low_c = c->pit_c;
		if (c->pit_c >= s->hot_relight_low_c + 3.0) s->hot_relight = false;   /* it caught */
		else if (now > s->hot_relight_deadline) {
			pf_safety_set_error(c, "E02_FLAMEOUT", "Relight failed: no rise. Clear the fire pot before lighting.");
			return PF_MODE_ERROR;
		}
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
				if (++s->above_count >= 2) { s->coldstart_reached = true; LOGI(TAG, "smart start: temperature rise confirmed (%.1f C)", s->filt_c); }
			} else s->above_count = 0;
			if (!s->coldstart_reached && now > s->coldstart_deadline) {
				/* Smart Start: no rise in the time allowed is an error, not another light. A pot that
				 * did not catch is a pot full of pellets, and lighting it again is how a grill flares. */
				double mins = (cfg->coldstart_timeout_s > 0 ? cfg->coldstart_timeout_s : c->startup_duration_s) / 60.0;
				pf_safety_set_error(c, "E04_STARTUP_FAILED", "No temperature rise in %.0f min. Check igniter and fire pot.", mins);
				return PF_MODE_ERROR;
			}
		}
	}

	/* 5. flame-out protection.
	 *
	 * The fixed floor below is the last word: by the time the pit has fallen that far the fire is
	 * out and the grill has to start again. Long before that, a pit that is not where the grill is
	 * trying to keep it is a fire that is failing, and the cheapest answer is the igniter -- it
	 * costs nothing but electricity and it catches the fire before there is nothing left to catch.
	 *
	 * There are two ways to arrive at that, and they need different triggers.
	 *
	 *   HOLDING. The grill reached its set point and the pit is sliding away from it. Nothing has
	 *   been asked of the grill, so any real distance below the target is a fault: the trigger is
	 *   falling `relight_drop` below it. A grill still on its way up is far below its target for
	 *   ordinary reasons, so there the measure is the highest the pit has reached in this Hold: a
	 *   climbing pit that falls `relight_drop` back from its own peak, with the controller feeding
	 *   to raise it, is a fire going out, and is caught at that point rather than at the floor.
	 *
	 *   COMING DOWN. The set point was lowered a long way, so the grill deliberately starves the
	 *   fire and coasts. That coast is exactly when a fire dies, and by the end of it there may be
	 *   nothing left to catch. Waiting for another twenty degrees of undershoot would be waiting
	 *   through the most dangerous part of the manoeuvre, so the trigger here is the moment the
	 *   pit crosses the new set point on the way down -- the point from which it should be
	 *   recovering rather than still falling.
	 *
	 * Both end on the pit climbing back above the lowest point it reached -- recovery from the
	 * bottom of the dip is the evidence the fire is winning, where waiting for the whole way back
	 * to the set point would hold the igniter on through the entire recovery. The lowest point
	 * keeps moving down while the pit is still falling, so the test is always against the bottom of
	 * this dip and not where the igniter came on.
	 *
	 * How much of a climb counts depends on which trigger started it, because the two are asking
	 * different questions. After a fire has fallen away from its target the question is whether
	 * there is a fire at all, and only a substantial rise answers it: `relight_recover`, ten
	 * degrees. On a coast down the fire was never in doubt, only starved, and the question is
	 * merely whether the pit has stopped falling -- so a couple of degrees of turnaround is the
	 * whole answer: `relight_recover_step`, three.
	 *
	 * An open lid is excluded from both: the pit falls because the heat walked out, not because
	 * the fire went out, and the igniter has nothing to fix. The igniter's own continuous-on cap
	 * above always applies and always wins.
	 *
	 * None of it applies during a tuning measurement. The relay deliberately drives the pit to
	 * both sides of the set point and leaves it there for minutes at a time: that is the
	 * measurement, not a fire in trouble, and lighting the igniter would both corrupt it and have
	 * nothing to fix. */
	bool relight_ok = m == PF_MODE_HOLD && cfg->relight_enabled && c->pit_valid && c->setpoint_c > 0 &&
	                  !c->lid_open && !c->autotune.active;

	/* Notice the set point being lowered a long way, and arm the coast. It is only armed when the
	 * pit is above the new target, because a set point dropped to somewhere the grill has not
	 * reached yet involves no coast at all. */
	if (m == PF_MODE_HOLD && c->pit_valid) {
		if (s->last_sp_c > 0 && c->setpoint_c > 0 && s->last_sp_c - c->setpoint_c >= cfg->relight_drop_c &&
		    c->pit_c > c->setpoint_c) {
			s->stepdown_armed = true;
			LOGI(TAG, "set point lowered %.0f C to %.0f C: watching the coast down for the fire going out",
			     s->last_sp_c - c->setpoint_c, c->setpoint_c);
		}
		if (c->setpoint_c > 0) s->last_sp_c = c->setpoint_c;
	} else {
		s->last_sp_c = 0;
		s->stepdown_armed = false;
	}

	/* the highest the pit has reached in this Hold, never above the set point */
	if (m == PF_MODE_HOLD && c->pit_valid && !c->lid_open) {
		if (isnan(s->hold_peak_c) || c->pit_c > s->hold_peak_c) s->hold_peak_c = c->pit_c;
	} else if (m != PF_MODE_HOLD) s->hold_peak_c = NAN;

	if (relight_ok) {
		double gap = c->setpoint_c - c->pit_c;
		double ref = c->target_reached || isnan(s->hold_peak_c) ? c->setpoint_c : fmin(c->setpoint_c, s->hold_peak_c);
		double drop = ref - c->pit_c;   /* how far the pit has fallen from where the fire had it */

		/* The escalation clock, kept apart from the igniter itself. Judging recovery by the rise
		 * off the lowest point is right for switching the igniter off, but it is the wrong clock to
		 * escalate on: the igniter's own heat can lift the pit a few degrees with the fire still
		 * out, so the assist cycles, and a deadline that restarted on every cycle would never
		 * expire. Only the pit genuinely climbing back towards the set point resets this. */
		if (drop <= cfg->relight_drop_c / 2) s->relight_below_since = 0;
		else if (drop >= cfg->relight_drop_c && s->relight_below_since == 0) s->relight_below_since = now;

		if (s->relight_below_since > 0 && now - s->relight_below_since > cfg->relight_timeout_s) {
			/* A rescue is an attempt, not a way to run. The igniter has had its window and the pit
			 * has not climbed back, so the fire is out rather than struggling: hand it to the
			 * flame-out path, which knows how to start the grill again and when to stop trying.
			 * Without this the assist would hold the igniter on until its own cap and keep the pit
			 * just warm enough that nothing else noticed. */
			s->relight_active = false;
			s->relight_below_since = 0;
			s->stepdown_armed = false;
			pf_outputs_set(PF_OUT_IGNITER, false);
			pf_alarms_clear("SAFETY:relight");
			LOGW(TAG, "pit stayed %.0f C below the set point for %.0f s: treating it as a flame-out",
			     drop, cfg->relight_timeout_s);
			return flameout(c);
		}

		if (!s->relight_active) {
			bool holding = drop >= cfg->relight_drop_c;
			bool crossed = s->stepdown_armed && gap > 0;   /* through the new set point, going down */
			if ((holding || crossed) && !s->igniter_locked_out) {
				s->relight_active = true;
				s->relight_low_c = c->pit_c;
				s->relight_from_step = crossed;
				s->stepdown_armed = false;
				pf_outputs_set(PF_OUT_IGNITER, true);
				LOGW(TAG, "%s: igniter on to catch the fire (pit %.0f C, set point %.0f C)",
				     crossed ? "pit crossed the lowered set point on the way down" : "pit fell away from the set point",
				     c->pit_c, c->setpoint_c);
				pf_alarms_raise("SAFETY:relight", "W08_RELIGHT", "Flame-out protection", PF_CRIT_HIGH, PF_SINK_ALL,
				                "Flame-out protection",
				                crossed ? "The grill is coasting down to a lower temperature, so the igniter is on until the pit stops falling."
				                        : "The pit fell away from the set point, so the igniter is on until the fire catches.");
				if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "W08_RELIGHT", "Flame-out protection: igniter on");
			}
		} else {
			if (c->pit_c < s->relight_low_c) s->relight_low_c = c->pit_c;
			double need = s->relight_from_step ? cfg->relight_recover_step_c : cfg->relight_recover_c;
			if (c->pit_c >= s->relight_low_c + need) {
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
