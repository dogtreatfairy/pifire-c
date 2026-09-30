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
	s->ss_baseline_c = NAN;
	s->filt_c = NAN;
	s->peak_c = NAN;
}

/* ------------------------------------------------------------------ Smart Start
 *
 * The one way this grill decides it is lit, for a first light and for a relight alike. It is built
 * on what a burner control does -- a trial for ignition with a hard time limit, and a second proof
 * before the fire is handed over -- with a temperature in place of a flame sensor:
 *
 *   PROVE. The pit must climb `prove_rise` above the lowest it has read since the light began,
 *   and hold it for PROVE_HOLD_S, within `prove_s`. Measured from the running minimum, not from
 *   where it started: the fan blows the pit down first, and a relight begins on a falling pit, and
 *   in both the turn upward is the evidence. No proof in time is a failed start, and the grill
 *   stops: a pot that did not catch is a pot full of pellets, and every pellet appliance says to
 *   empty it before lighting again.
 *
 *   A HOT PIT may not show the rise in time and does not need to: a pot that catches in a barrel
 *   already at 300 F, or a fire that lived through a short power cut, holds the pit up rather than
 *   lifting it. There the proof is the fall that stopped. A barrel HOT_DELTA_C or more above the
 *   outdoor air with no fire in it cannot stop cooling -- even one three times slower than Ryan's
 *   (time constant 4000 s against his 1470 s) loses more than 4 F in two minutes -- so a lowest
 *   reading that has not moved down by HOT_SLIP_C in HOT_FLAT_S is a fire holding it. Nothing
 *   learned is trusted here: a fitted time constant that was half the real one would let a dead
 *   pot pass. A cold start, which has nothing to fall from, is always proven by the rise.
 *
 *   EXIT. Proven, startup carries on until the pit is `exit_rise` above that minimum, and then
 *   hands over to Smoke or Hold -- within a second `prove_s`, or it is a failed start too. The
 *   small rise alone cannot tell a fire from the igniter: a few hundred watts in the pot can lift a
 *   still, cold barrel a few degrees in five minutes (Ryan's warm restart showed none, but there
 *   the fan was cooling a warm pit and hid it). Twelve degrees is past what the igniter can do, and
 *   a real fire makes it at once: 27 s from the small rise to the large one on his grill. The same
 *   holds at -30 F outside -- every threshold is a rise over the grill's own lowest reading, not a
 *   temperature, so the weather moves where it starts and nothing else. */
/* How the rise is read. Ryan's grill, restarted warm on 2026-09-29, fell for 219 s under the fan
 * with the igniter on and caught at about 220 s: +3 F at 258 s raw. A 30 s filter and a 30 s hold
 * put the proof at 309 s -- a failed start on a fire that had lit. Readings on a flat pit move by
 * 0.3 F at most, so a 10 s filter and a 15 s hold reject noise just as well and prove at 279 s. */
#define PROVE_FILTER_S 10.0
#define PROVE_HOLD_S 15.0
#define LID_ALLOW_S  300.0
#define HOT_DELTA_C  83.3    /* 150 F above the outdoor air */
#define HOT_FLAT_S   120.0
#define HOT_SLIP_C   0.56    /* 1 F */

void pf_safety_on_startup_enter(pf_control *c, double now)
{
	pf_safety *s = &c->safety;
	s->igniter_on_since = 0;
	s->igniter_locked_out = false;
	s->ss_active = true;
	s->ss_proven = false;
	s->ss_above_since = 0;
	s->filt_c = c->pit_valid ? c->pit_c : NAN;
	s->ss_baseline_c = s->filt_c;
	s->ss_deadline = now + c->cfg.ss_prove_s;
	s->ss_start = now;
	s->ss_entry_c = s->filt_c;
	s->ss_held = false;
	s->ss_flat_t = 0;
	s->peak_c = NAN;
	s->heating = false;
	s->proving = false;
	s->stepdown_armed = false;
	LOGI(TAG, "smart start: need +%.1f C over the lowest reading within %.0f s", c->cfg.ss_prove_rise_c, c->cfg.ss_prove_s);
}

void pf_safety_on_startup_exit(pf_control *c, double now)
{
	pf_safety *s = &c->safety;
	s->ss_active = false;
	s->heating = true;
	s->established = false;
	s->peak_c = c->pit_valid ? c->pit_c : NAN;
	s->progress_c = s->peak_c;
	s->progress_t = now;
	s->handover_c = s->peak_c;
	LOGI(TAG, "smart start complete at %.1f C", c->pit_c);
}

bool pf_safety_startup_done(pf_control *c, double now)
{
	const pf_safety *s = &c->safety;
	(void)now;
	if (!s->ss_active || !s->ss_proven) return false;
	return s->ss_held || s->filt_c >= s->ss_baseline_c + c->cfg.ss_exit_rise_c;
}

void pf_safety_on_run_enter(pf_control *c, pf_mode prev, double now)
{
	pf_safety *s = &c->safety;
	(void)prev;
	/* Smoke to Hold and back keep the peak: the fire is the same fire. A mode entered other than
	 * from a light has a fire already known to be burning. */
	if (isnan(s->peak_c) && c->pit_valid) s->peak_c = c->pit_c;
	if (c->pit_valid) { s->progress_c = c->pit_c; s->progress_t = now; }
	if (prev != PF_MODE_STARTUP && prev != PF_MODE_REIGNITE) s->established = true;
	s->reached = false;
	s->stall_said = false;
	/* entered with the pit above where this mode works is a coast, as a lowered set point is */
	double work = c->mode == PF_MODE_HOLD ? c->setpoint_c : c->cfg.smoke_min_c;
	s->stepdown_armed = c->pit_valid && work > 0 && c->pit_c > work;
}

/* ------------------------------------------------------------------ flame-out
 *
 * A proven fire that loses its heat is relit, once by default, through the Smart Start sequence;
 * past that, or with relighting switched off, the grill stops in error. */
static int flameout(pf_control *c, const char *why)
{
	pf_safety *s = &c->safety;
	s->proving = false;
	pf_outputs_set(PF_OUT_IGNITER, false);
	if (!c->cfg.relight_enabled || s->reignite_retries_left <= 0) {
		pf_safety_set_error(c, "E02_FLAMEOUT", "Flame lost: %s. Clear the fire pot before lighting.", why);
		return PF_MODE_ERROR;
	}
	s->reignite_retries_left--;
	s->reignite_last = c->mode;
	LOGW(TAG, "flame lost (%s): relighting (%d attempt(s) left)", why, s->reignite_retries_left);
	if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "W03_RELIGHT", "Flame lost, relighting");
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
		pf_safety_set_error(c, "E01_OVERTEMP", "Overheat: pit %.0f°%s, limit %.0f°%s", pf_from_c(c->pit_c, cfg->units), cfg->units == PF_UNITS_C ? "C" : "F",
		                    pf_from_c(cfg->max_temp_c, cfg->units), cfg->units == PF_UNITS_C ? "C" : "F");
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



	/* the filtered pit Smart Start judges on */
	if (active) {
		double dt = c->last_step > 0 ? now - c->last_step : 0.1;
		double a = dt / PROVE_FILTER_S; if (a > 1) a = 1;
		s->filt_c = isnan(s->filt_c) ? c->pit_c : s->filt_c + (c->pit_c - s->filt_c) * a;
	}

	/* 3. Smart Start (Startup, Relight) */
	if ((m == PF_MODE_STARTUP || m == PF_MODE_REIGNITE) && s->ss_active) {
		if (!s->ss_proven) {
			if (isnan(s->ss_baseline_c) || s->filt_c < s->ss_baseline_c) s->ss_baseline_c = s->filt_c;
			if (s->filt_c >= s->ss_baseline_c + cfg->ss_prove_rise_c) {
				if (s->ss_above_since == 0) s->ss_above_since = now;
				else if (now - s->ss_above_since >= PROVE_HOLD_S) {
					s->ss_proven = true;
					s->ss_deadline = now + cfg->ss_prove_s;
					LOGI(TAG, "smart start: ignition proven (%.1f C over a low of %.1f C)", s->filt_c, s->ss_baseline_c);
					if (pf_db_handle()) pf_db_event(PF_LVL_INFO, "I03_IGNITION_PROVEN", "Ignition proven");
				}
			} else s->ss_above_since = 0;
			/* the hot-pit proof: the lowest reading has stopped moving down */
			if (s->filt_c < s->ss_flat_c - HOT_SLIP_C || s->ss_flat_t == 0) { s->ss_flat_c = s->filt_c; s->ss_flat_t = now; }
			double amb = isnan(c->ambient_c) ? 20.0 : c->ambient_c;
			if (!s->ss_proven && s->filt_c - amb >= HOT_DELTA_C && now - s->ss_flat_t >= HOT_FLAT_S) {
				s->ss_proven = s->ss_held = true;
				LOGI(TAG, "smart start: fire holding the pit at %.1f C (no fall in %.0f s)", s->filt_c, HOT_FLAT_S);
				if (pf_db_handle()) pf_db_event(PF_LVL_INFO, "I03_IGNITION_PROVEN", "Ignition proven: fire holding the pit");
			}
		}
		if (now > s->ss_deadline) {
			/* no rise in the time, or a small rise that never became a fire */
			double mins = cfg->ss_prove_s / 60.0;
			const char *what = s->ss_proven ? "no fire after the first rise in" : "no rise in";
			pf_outputs_set(PF_OUT_AUGER, false);
			pf_outputs_set(PF_OUT_IGNITER, false);
			if (m == PF_MODE_REIGNITE)
				pf_safety_set_error(c, "E04_STARTUP_FAILED", "Relight failed: %s %.0f min. Clear the fire pot before lighting.", what, mins);
			else
				pf_safety_set_error(c, "E04_STARTUP_FAILED", "Failed start: %s %.0f min. Check the igniter and clear the fire pot.", what, mins);
			return PF_MODE_ERROR;
		}
		return 0;
	}

	/* 4. Flame supervision in Smoke and Hold.
	 *
	 * Ryan's rule: startup raises the pit from its baseline, and Smoke and Hold go on raising it from
	 * where startup handed over until it reaches the working temperature -- the set point in Hold,
	 * `smoke_min` (180 F) in Smoke, where P-mode is set so that Smoke sits at or above it. A pit that
	 * does anything else is a flame-out or a feed that is too low.
	 *
	 *   FALL. The reference is the working temperature once the pit has reached it, and before that
	 *   the highest the pit has been. Falling `relight_drop` below it is a flame-out: relit, or the
	 *   grill stops.
	 *
	 *   CLIMB. Below the working temperature (by more than half the drop, so the slow last degrees of
	 *   an approach are not held against it) the pit must gain `prove_rise` in every `prove_s`. Just
	 *   after a light, until it is `exit_rise` past the handover, a pit that does not is a fire that
	 *   never got going -- a failed start, and the grill stops, since relighting a pot that never
	 *   caught is relighting a pot full of pellets. After that a pit that has stopped short is burning
	 *   what it is given: nothing is piling up, so it is reported (feed too low, or the fire failing)
	 *   rather than stopped; if the fire is failing the fall will say so.
	 *
	 *   COAST. The pit above the working temperature -- a set point lowered, or Smoke from a hotter
	 *   Hold -- is a coast: the grill starves the fire on purpose, and a starved fire and a dead one
	 *   cool alike. Nothing is judged on the way down. When the pit reaches the working temperature
	 *   the igniter runs for `relight_prove_s` while the feed comes back, catching the fire if it has
	 *   died down; from there the fall and the climb apply as usual.
	 *
	 * An open lid (and the recovery after it) and a tuning measurement are neither: the heat walked
	 * out, or the relay is swinging the pit on purpose. The climb clock stands still meanwhile. */
	if ((m == PF_MODE_SMOKE || m == PF_MODE_HOLD) && c->pit_valid) {
		bool hold = m == PF_MODE_HOLD;
		double work = hold ? c->setpoint_c : cfg->smoke_min_c;
		/* the lid for as long as a cook at the meat plausibly takes, as the notifications allow it:
		 * a fall that is still going after that is not the lid */
		bool lid = c->lid_open || (c->lid_event && now - c->lid_event_t < LID_ALLOW_S);
		bool paused = lid || c->autotune.active;

		/* a set point lowered below the pit starts a coast */
		if (hold && s->last_sp_c > 0 && c->setpoint_c > 0 && c->setpoint_c < s->last_sp_c - 0.01 && c->pit_c > c->setpoint_c) {
			s->stepdown_armed = true;
			LOGI(TAG, "set point lowered to %.0f C: igniter proves the fire when the pit reaches it", c->setpoint_c);
		}
		s->last_sp_c = hold ? c->setpoint_c : 0;
		if (!hold && c->pit_c >= work) s->reached = true;

		if (paused || s->stepdown_armed) {
			s->progress_c = c->pit_c;
			s->progress_t = now;
		} else {
			if (isnan(s->peak_c) || c->pit_c > s->peak_c) s->peak_c = c->pit_c;
			/* Hold asks the controller, whose "reached" starts again with every new set point */
			double ref = (hold ? c->target_reached : s->reached) ? work : fmin(s->peak_c, work);
			if (c->pit_c <= ref - cfg->relight_drop_c) {
				char why[96];
				snprintf(why, sizeof why, "pit %.0f° below %.0f°", pf_delta_from_c(ref - c->pit_c, cfg->units), pf_from_c(ref, cfg->units));
				return flameout(c, why);
			}
			if (s->heating && c->pit_c >= work) {
				s->heating = false;
				LOGI(TAG, "working temperature reached (%.0f C)", c->pit_c);
			}
			if (!s->established && c->pit_c >= s->handover_c + cfg->ss_exit_rise_c) s->established = true;
			if (c->pit_c >= work - cfg->relight_drop_c / 2 || c->pit_c >= s->progress_c + cfg->ss_prove_rise_c) {
				s->progress_c = c->pit_c;
				s->progress_t = now;
				s->stall_said = false;
			} else if (now - s->progress_t > cfg->ss_prove_s) {
				if (!s->established) {
					pf_outputs_set(PF_OUT_AUGER, false);
					pf_safety_set_error(c, "E04_STARTUP_FAILED", "No heat gain in %.0f min after lighting: fire not taking or feed too low%s. Clear the fire pot before lighting.",
					                    cfg->ss_prove_s / 60.0, hold ? "" : " (lower the P-mode)");
					return PF_MODE_ERROR;
				}
				if (!s->stall_said) {
					s->stall_said = true;
					const char *u = cfg->units == PF_UNITS_C ? "C" : "F";
					pf_events_emit("W10_NOT_HEATING", "Not Heating", "Pit %.0f°%s, needs %.0f°%s. Flame failing or feed too low%s.",
					               pf_from_c(c->pit_c, cfg->units), u, pf_from_c(work, cfg->units), u, hold ? "" : ": lower the P-mode");
				}
				s->progress_c = c->pit_c;
				s->progress_t = now;
			}
		}
		/* the end of a coast: the pit has come down to the working temperature */
		if (s->stepdown_armed && c->pit_c <= work) {
			s->stepdown_armed = false;
			if (!hold) s->reached = true;
			s->peak_c = c->pit_c;
			if (cfg->relight_prove_s > 0 && !s->igniter_locked_out) {
				s->proving = true;
				s->prove_until = now + cfg->relight_prove_s;
				LOGI(TAG, "pit down to %.0f C: igniter on for %.0f s", work, cfg->relight_prove_s);
				if (pf_db_handle()) pf_db_event(PF_LVL_INFO, "I04_PROVING", "Coast over: igniter proving the fire");
			}
		}
		if (s->proving) {
			if (now >= s->prove_until || s->igniter_locked_out) {
				s->proving = false;
				pf_outputs_set(PF_OUT_IGNITER, false);
			} else pf_outputs_set(PF_OUT_IGNITER, true);
		}
	} else if (m != PF_MODE_SMOKE && m != PF_MODE_HOLD) {
		s->last_sp_c = 0;
		s->stepdown_armed = false;
		s->proving = false;
		if (m != PF_MODE_STARTUP && m != PF_MODE_REIGNITE) { s->peak_c = NAN; s->heating = false; }
	}

	return 0;
}
