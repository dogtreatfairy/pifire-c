	/* The coast look-ahead that used to live here -- cut back to the feed-forward once the pit,
	 * rising at its current rate, would reach the target within one dead time -- is gone. It was
	 * the same idea done from the slope instead of from the model, and it could only ever cut back
	 * TO the steady-state feed, never below it, which is why it left ten degrees of overshoot on
	 * the table. The prediction above does the whole job, and two mechanisms aiming at one thing
	 * would fight. */
/* Adaptive controller: learned feed-forward plus a self-tuning PID on the error.
 *   u = ff_gain * u_ff(setpoint, ambient)  +  Kp*e + Ki*∫e + Kd*de/dt
 * u_ff comes from the daemon (features/learning.c) via pf_ctrl_in.u_ff; the PID only has to
 * correct what the feed-forward gets wrong, so it can be gentle (large PB, long Ti).
 *
 * Learning, all automatic while the Learning switch is on:
 *  - apply_tuning() receives the plant model the daemon identifies from every startup rise
 *    (K, tau, theta) and derives SIMC gains from it; a relay autotune result (Ku, Pu) is used the
 *    same way. Learned gains are blended with the previous ones and persisted (env kv "learned").
 *  - a performance monitor watches every 10 minutes of Hold: sustained oscillation lowers the gain
 *    the band itself: a sluggish loop that sits off target narrows it, big overshoot after a
 *    set-point change widens it, always staying near the tuning it is refining. The refined band is
 *    persisted, so the grill keeps getting better across cooks and the number on display is the
 *    number in force.
 * Integration is conditional (paused while saturated) and the integrator is seeded for bumpless
 * transfer. */
#include "controllers/pid_common.h"
#include "pifire/common.h"
#include "pifire/controller.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WINDOW_S      600.0   /* performance window */
#define DEADBAND_C    1.5     /* error must cross +/- this to count as an oscillation half-cycle */
/* How far learning may move the band away from the tuning it is refining. A measurement is
 * evidence; learning is a correction to it, not a licence to replace it. */
#define LEARN_MIN     0.7
#define LEARN_MAX     1.6
#define IBAND_C       8.5     /* +/- 15 F: entering this band trims the integrator (approach wind-up) */
#define THETA_MIN     40.0
#define THETA_MAX     240.0
#define THETA_DEFAULT 90.0
/* the plant, until a run measures this grill's own */
#define K_DEFAULT     330.0     /* C per unit duty */
#define TAU_DEFAULT   1000.0    /* s */
#define K_MIN 60.0
#define K_MAX 900.0
#define TAU_MIN 120.0
#define TAU_MAX 3000.0
/* theta / cycle_time entries at most; 64 covers a 20 minute dead time at a 20 s cycle */
#define PF_MDL_RING 64
#define SURPLUS_MAX 100.0   /* C; only a guard against a nonsense model, not a working limit */
#define PF_SCALE_BANDS 4      /* temperature bands the loop-gain correction is learned in */

typedef struct {
	const pf_env *env;
	pf_units units;
	bool auto_tune;
	/* configured (user) gains */
	double cfg_PB_c, cfg_Ti, cfg_Td, ff_gain;
	/* learned gains, used when valid and auto_tune */
	double l_PB_c, l_Ti, l_Td; bool l_valid; double l_ts; char l_src[8];
	/* The loop gain correction the monitor learns, kept per temperature band. A grill that hunts
	 * at 180 F is not necessarily hunting at 450 F, and one number across the whole range would
	 * average away exactly the difference this controller is trying to learn. */
	/* the band learning settled on for each temperature range, in degrees -- not a factor */
	double band_learned[PF_SCALE_BANDS];
	/* The band each lesson was learned against, so a later tune can inherit the lesson instead of
	 * discarding it: what learning actually discovered is that this grill wants a band some
	 * fraction of whatever was measured, and that fraction still holds when the measurement moves. */
	double band_anchor[PF_SCALE_BANDS];
	double band_PB_c;   /* the lesson for the band being held right now, re-anchored; 0 when there is none */
	/* tuning autotune measured at this set point, handed over fresh each cycle */
	double sch_PB_c, sch_Ti, sch_Td; bool sch_valid;
	/* effective */
	double PB_c, Ti, Td, kp, ki, kd;
	double inter, last_err, last_t, last_pit;
	double p, i, d, ff, u;
	bool have_last;
	double setpoint_c;
	/* performance monitor */
	double win_start, win_abs_sum, win_peak; int win_n, win_changes, win_sat, last_sign;
	double step_t, step_size, step_peak; bool step_open;
	int settled_n;
	/* The grill as a first-order lag with a dead time, which is what a pellet grill is: fuel goes
	 * in, the pot takes a while to notice, and the pit then follows with a long time constant. */
	double K, tau, theta;   /* C per unit duty, time constant (s), dead time (s) */
	/* The Smith predictor's model state and its delay line. `mdl` is the normalised lag state, the
	 * fraction of the current feed's effect the pot has taken up; the ring holds what it was over
	 * the last theta seconds, so K times the difference between the two is the heat that is
	 * committed and has not yet been felt. */
	double mdl;
	double ring_t[PF_MDL_RING], ring_y[PF_MDL_RING];
	int ring_n, ring_head;
	double surplus;         /* C of committed rise the pit has not shown yet */
	bool in_band;
} ad_t;

/* No learning switch here. Whether the grill learns is one decision, made once, in the Learning
 * section; the daemon passes the answer down as `auto_tune`. Offering it again as a controller
 * option meant two switches for one question, sitting on the same page, able to disagree. */
static const char schema[] =
"[{\"option_name\":\"PB\",\"option_friendly_name\":\"Proportional Band (PB)\",\"option_description\":\"Correction band around the set point; starting point until the grill has learned its own. [Default 80]\",\"option_type\":\"float\",\"option_default\":80.0,\"option_step\":1,\"units\":\"temp_delta\"},"
 "{\"option_name\":\"Ti\",\"option_friendly_name\":\"Integral Time (Ti)\",\"option_description\":\"Seconds to correct residual error. [Default 400]\",\"option_type\":\"float\",\"option_default\":400.0,\"option_step\":1},"
 "{\"option_name\":\"Td\",\"option_friendly_name\":\"Derivative Time (Td)\",\"option_description\":\"Damping against fast swings (lid, wind). [Default 30]\",\"option_type\":\"float\",\"option_default\":30.0,\"option_step\":1},"
 "{\"option_name\":\"ff_gain\",\"option_friendly_name\":\"Feed-forward gain\",\"option_description\":\"Scale on the learned steady-state feed (1.0 = trust the model fully). [Default 1.0]\",\"option_type\":\"float\",\"option_default\":1.0,\"option_step\":0.05}]";

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Band edges in C: below 93 (200 F), to 135 (275 F), to 204 (400 F), above. Four is enough to
 * separate low smoking from searing without splitting the data too thin to learn from. */
static const double BAND_EDGE_C[PF_SCALE_BANDS - 1] = { 93.3, 135.0, 204.4 };

static void use_band(ad_t *s, double setpoint_c);
static double anchor_PB(const ad_t *s);

static int band_of(double setpoint_c)
{
	for (int i = 0; i < PF_SCALE_BANDS - 1; i++) if (setpoint_c < BAND_EDGE_C[i]) return i;
	return PF_SCALE_BANDS - 1;
}

static void recompute(ad_t *s)
{
	/* One answer, in degrees, and it is the one on display. The tune is the authority: the library
	 * entry measured at this very set point if there is one, else the fit from the last cook, else
	 * the numbers that were typed in. Learning then refines that answer per temperature range, and
	 * because its lesson is carried as a proportion of whatever it was learned against, a later
	 * tune replaces the measurement without throwing the lesson away.
	 *
	 * There is no gain factor anywhere in here any more. Learning used to multiply whichever tuning
	 * won by a number nobody could see, so the band actually in force was never the band on
	 * display: a measured 123 running at 1.15 is a 107 that appears nowhere. Written down and typed
	 * into another identical grill, the number carried none of what made this one work. */
	/* The schedule arrives only when the tuning library is switched on -- the daemon decides that,
	 * and simply passes nothing when it is off -- so its presence is the whole test here. */
	bool use_sched = s->sch_valid;
	bool use_learned = s->auto_tune && s->l_valid;
	double anchor = anchor_PB(s);
	s->PB_c = s->auto_tune && s->band_PB_c > 0 ? s->band_PB_c : anchor;
	s->Ti = use_sched ? s->sch_Ti : use_learned ? s->l_Ti : s->cfg_Ti;
	s->Td = use_sched ? s->sch_Td : use_learned ? s->l_Td : s->cfg_Td;
	double ki_was = s->ki;
	s->kp = s->PB_c > 0 ? -1.0 / s->PB_c : 0;
	s->ki = s->Ti > 0 ? s->kp / s->Ti : 0;
	s->kd = s->kp * s->Td;
	/* The integrator holds an accumulated error, and its contribution to the output is ki times
	 * that. When the gains move under it -- a new set point picking a different entry from the
	 * tuning library, a band correction, a fresh tune -- the accumulated error has to be rescaled
	 * or the contribution jumps by the ratio of the gains, which at the extremes is enough to send
	 * the output off scale and get the controller swapped out for the fallback PID. */
	if (ki_was != 0 && s->ki != 0) s->inter *= ki_was / s->ki;
}

static void save_learned(ad_t *s)
{
	if (!s->env || !s->env->kv_put) return;
	char buf[512];
	snprintf(buf, sizeof buf, "{\"PB_c\":%.2f,\"Ti\":%.1f,\"Td\":%.1f,\"band_learned\":[%.2f,%.2f,%.2f,%.2f],"
	         "\"band_anchor\":[%.2f,%.2f,%.2f,%.2f],\"valid\":%s,\"ts\":%.0f,\"src\":\"%s\","
	         "\"theta\":%.0f,\"K\":%.1f,\"tau\":%.0f}",
	         s->l_PB_c, s->l_Ti, s->l_Td,
	         s->band_learned[0], s->band_learned[1], s->band_learned[2], s->band_learned[3],
	         s->band_anchor[0], s->band_anchor[1], s->band_anchor[2], s->band_anchor[3],
	         s->l_valid ? "true" : "false", s->l_ts, s->l_src, s->theta, s->K, s->tau);
	s->env->kv_put(s->env, "learned", buf);
}

static void load_learned(ad_t *s)
{
	for (int i = 0; i < PF_SCALE_BANDS; i++) { s->band_learned[i] = 0; s->band_anchor[i] = 0; }
	if (!s->env || !s->env->kv_get) return;
	char buf[512];
	if (s->env->kv_get(s->env, "learned", buf, sizeof buf) != 0) return;
	cJSON *j = cJSON_Parse(buf);
	if (!j) return;
	s->l_PB_c = pf_pid_cfg_num(j, "PB_c", 0); s->l_Ti = pf_pid_cfg_num(j, "Ti", 0); s->l_Td = pf_pid_cfg_num(j, "Td", 0);

	/* Per-band corrections, or the old single value spread across every band when upgrading from
	 * a release that only had one. */
	cJSON *bs = cJSON_GetObjectItem(j, "band_learned"), *ba = cJSON_GetObjectItem(j, "band_anchor");
	for (int i = 0; i < PF_SCALE_BANDS; i++) {
		cJSON *it = cJSON_IsArray(bs) ? cJSON_GetArrayItem(bs, i) : NULL;
		cJSON *an = cJSON_IsArray(ba) ? cJSON_GetArrayItem(ba, i) : NULL;
		s->band_learned[i] = cJSON_IsNumber(it) && it->valuedouble > 0 ? it->valuedouble : 0;
		s->band_anchor[i] = cJSON_IsNumber(an) && an->valuedouble > 0 ? an->valuedouble : 0;
	}
	s->l_ts = pf_pid_cfg_num(j, "ts", 0);
	s->theta = clampd(pf_pid_cfg_num(j, "theta", THETA_DEFAULT), THETA_MIN, THETA_MAX);
	s->K = clampd(pf_pid_cfg_num(j, "K", K_DEFAULT), K_MIN, K_MAX);
	s->tau = clampd(pf_pid_cfg_num(j, "tau", TAU_DEFAULT), TAU_MIN, TAU_MAX);
	cJSON *v = cJSON_GetObjectItem(j, "valid"), *src = cJSON_GetObjectItem(j, "src");
	s->l_valid = cJSON_IsTrue(v) && s->l_PB_c > 0 && s->l_Ti > 0;
	snprintf(s->l_src, sizeof s->l_src, "%.7s", cJSON_IsString(src) ? src->valuestring : "");
	cJSON_Delete(j);
}

static void apply_config(ad_t *s, const char *json)
{
	cJSON *c = json ? cJSON_Parse(json) : NULL;
	s->units = pf_pid_cfg_units(c);
	s->cfg_PB_c = pf_delta_to_c(pf_pid_cfg_num(c, "PB", 80.0), s->units);
	s->cfg_Ti = pf_pid_cfg_num(c, "Ti", 400.0);
	s->cfg_Td = pf_pid_cfg_num(c, "Td", 30.0);
	s->ff_gain = pf_pid_cfg_num(c, "ff_gain", 1.0);
	cJSON *at = c ? cJSON_GetObjectItem(c, "auto_tune") : NULL;
	s->auto_tune = at ? cJSON_IsTrue(at) : true;
	cJSON_Delete(c);
	recompute(s);
}

static void *create(const char *json, const pf_env *env)
{
	ad_t *s = calloc(1, sizeof *s);
	if (!s) return NULL;
	s->env = env;
	s->theta = THETA_DEFAULT; s->K = K_DEFAULT; s->tau = TAU_DEFAULT;
	load_learned(s);
	apply_config(s, json);
	if (env && env->log && s->l_valid) env->log(PF_LVL_INFO, "adaptive", "learned tuning restored: PB %.1f C, Ti %.0f s, Td %.0f s (%s)", s->l_PB_c, s->l_Ti, s->l_Td, s->l_src);
	return s;
}
static void destroy(void *self) { free(self); }

static void window_reset(ad_t *s, double now)
{
	s->win_start = now; s->win_abs_sum = 0; s->win_peak = 0; s->win_n = 0; s->win_changes = 0; s->win_sat = 0; s->last_sign = 0;
}

static void reset(void *self, const pf_ctrl_in *in)
{
	ad_t *s = self;
	bool sp_change = s->have_last && s->setpoint_c != in->setpoint_c;
	if (sp_change) { s->step_t = in->now_s; s->step_size = in->setpoint_c - s->setpoint_c; s->step_peak = 0; s->step_open = true; s->settled_n = 0; }
	s->setpoint_c = in->setpoint_c;
	use_band(s, in->setpoint_c);      /* the correction learned around this temperature, not the last one */
	s->last_t = in->now_s;
	s->last_pit = in->pit_c;
	s->last_err = in->pit_c - in->setpoint_c;
	s->have_last = true;
	window_reset(s, in->now_s);
	/* Start the model where the feed already has it, with no history behind it, so it predicts
	 * nothing until the feed moves. Started from zero instead -- which is where a fresh controller
	 * finds it -- it spends a whole time constant climbing to meet the duty, and reads that climb
	 * out as a rise on its way to the pit: the loop starves a fire that is doing nothing of the
	 * kind, and settles the pit ten degrees high. What the prediction says must depend on what the
	 * feed has done, never on when the controller happened to start. */
	s->mdl = clampd(in->u_prev_applied, 0, 1);
	s->ring_n = 0; s->ring_head = 0; s->surplus = 0;
	/* Integrator seed. Near the target (controller swap, software restart, small set-point nudge) it is
	 * bumpless: the integrator absorbs the gap between the last applied duty and ff + P, within the
	 * same +/-0.15 duty the band trim allows. Far from the target the last duty is meaningless (it was
	 * the smoke cycle or the u_min placeholder at Hold entry) and seeding from it would park a large
	 * negative integral that then holds the feed back for many minutes, so it starts at zero. */
	double ff = s->ff_gain * in->u_ff;
	double p = s->kp * s->last_err;
	s->inter = 0;
	if (s->ki != 0 && fabs(s->last_err) <= IBAND_C) {
		double seed = clampd(in->u_prev_applied - ff - p, -0.15, 0.15);
		s->inter = seed / s->ki;
	}
	s->in_band = fabs(s->last_err) <= IBAND_C;
}

/* Use the lesson learning settled on for this temperature range, so recompute() and the published
 * note both show the tuning actually in force. A lesson learned against an older tune is carried
 * onto the current one in proportion, not thrown away and not applied literally: "a fifth wider
 * than what was measured here" survives a new measurement, "128 degrees" does not. A range nothing
 * has been learned about runs the tune as measured. */
static void use_band(ad_t *s, double setpoint_c)
{
	int b = band_of(setpoint_c);
	double v = s->band_learned[b], a0 = s->band_anchor[b], a = anchor_PB(s);
	double want = 0;
	if (v > 0) want = a0 > 0 && a > 0 ? clampd(a * (v / a0), a * LEARN_MIN, a * LEARN_MAX) : v;
	s->band_PB_c = want;
	recompute(s);
}

/* What the autotune (or, failing that, what was typed) says the band should be. Learning refines
 * that answer; it does not get to invent its own. */
static double anchor_PB(const ad_t *s)
{
	if (s->sch_valid && s->sch_PB_c > 0) return s->sch_PB_c;
	if (s->auto_tune && s->l_valid && s->l_PB_c > 0) return s->l_PB_c;
	return s->cfg_PB_c > 0 ? s->cfg_PB_c : s->l_PB_c;
}

/* `tighter` above 1 makes the loop more aggressive, which means a narrower band. The result is
 * kept near the tuning it is refining: learning that may wander to half or double its anchor is
 * not refining a measurement, it is replacing it with a guess. */
static void adjust_band(ad_t *s, double tighter, const char *why)
{
	double base = s->PB_c > 0 ? s->PB_c : anchor_PB(s);
	if (!(base > 0) || !(tighter > 0)) return;
	double a = anchor_PB(s);
	double want = base / tighter;
	if (a > 0) want = clampd(want, a * LEARN_MIN, a * LEARN_MAX);
	if (fabs(want - s->PB_c) < 1e-6) return;
	int b = band_of(s->setpoint_c);
	/* The lesson belongs to the band, alongside the tune it refines; it does not overwrite the
	 * measurement, which is what a later export and a later tune both need to stay honest. */
	s->band_learned[b] = want;
	s->band_anchor[b] = a > 0 ? a : want;
	s->band_PB_c = want;
	recompute(s);
	save_learned(s);
	if (s->env && s->env->log)
		s->env->log(PF_LVL_INFO, "adaptive", "band %.1f C around %.0f C, refining %.1f C (%s)", want, s->setpoint_c, a, why);
}

/* rule-based self-correction from the last window of Hold behaviour */
static void monitor(ad_t *s, const pf_ctrl_in *in, double e)
{
	if (!s->auto_tune) return;
	/* A tuning run drives the loop on purpose. Every window through it would look like hunting,
	 * and the correction learned from it would be a correction for the test rather than for the
	 * grill -- clouding the very measurement it is standing next to. */
	if (in->tuning) { window_reset(s, in->now_s); return; }
	double a = fabs(e);
	s->win_abs_sum += a; s->win_n++;
	if (a > s->win_peak) s->win_peak = a;
	if (in->saturated) s->win_sat++;
	int sign = e > DEADBAND_C ? 1 : e < -DEADBAND_C ? -1 : 0;
	if (sign && s->last_sign && sign != s->last_sign) s->win_changes++;
	if (sign) s->last_sign = sign;
	/* overshoot after a set-point step: peak error in the direction of the step, once settled */
	if (s->step_open) {
		double over = s->step_size > 0 ? e : -e;
		if (over > s->step_peak) s->step_peak = over;
		if (a < 2.0) s->settled_n++; else s->settled_n = 0;
		bool timeout = in->now_s - s->step_t > 3600;
		if (s->settled_n >= 3 || timeout) {
			s->step_open = false;
			if (!timeout && s->step_peak > 5.0 && s->step_peak > 0.15 * fabs(s->step_size)) adjust_band(s, 0.9, "overshoot after a set-point change");
		}
	}
	if (in->now_s - s->win_start < WINDOW_S || s->win_n < 10) return;
	double mean_abs = s->win_abs_sum / s->win_n;
	bool mostly_free = s->win_sat < s->win_n / 4;
	bool step_recent = s->step_open || in->now_s - s->step_t < 1200;
	if (s->win_changes >= 3 && s->win_peak >= 3.0) adjust_band(s, 0.85, "sustained oscillation");
	else if (mean_abs > 3.0 && s->win_changes <= 1 && mostly_free && !step_recent) adjust_band(s, mean_abs > 6.0 ? 1.25 : 1.15, "slow to reach target");
	window_reset(s, in->now_s);
}


/* A Smith predictor, in the only form a pellet grill needs.
 *
 * The fault it fixes: fuel committed to the pot does not show up in the pit for a dead time, and
 * keeps arriving for a time constant after that. A controller that only ever sees the measurement
 * is therefore always a minute behind the fire, so on the way up it keeps feeding a grill that has
 * already been given enough, and the pit sails past the target. That is the overshoot on capture,
 * and no amount of tuning the three numbers removes it: on this grill it was 12.9 F with a 150 F
 * proportional band and 9.3 F with a 67 F one.
 *
 * Smith's answer (1957) is to run a model of the plant alongside it and feed the controller the
 * model's UNDELAYED output corrected by however wrong the model has turned out to be:
 *
 *     what the controller sees  =  pit  +  [ y_model(t) - y_model(t - theta) ]
 *
 * The bracket is the rise that is already committed and not yet measured. At steady state the two
 * model terms are equal, the bracket is zero, and the loop is exactly as it was -- so this cannot
 * introduce an offset. On a climb the bracket is positive and the controller backs off early, by
 * the amount the fire is about to deliver on its own.
 *
 * The model is deviation-from-ambient, so the ambient never enters: only the difference between two
 * points on the same curve is used, and any constant offset cancels.
 */
/* The grill as a first-order lag with a dead time, run alongside the real one and driven by the
 * same feed.
 *
 * It is kept in NORMALISED form: `mdl` is the lag's response to the duty, between 0 and 1, and the
 * temperature it stands for is K times that. Two things fall out of writing it that way. A new
 * plant model -- a fresh fit, or the library handing over the one measured at this set point --
 * changes K and tau without moving the state, so nothing jolts. And the model carries no absolute
 * temperature, so it cannot disagree with the pit about where the grill IS; it only ever says how
 * much of the feed's effect has yet to arrive, which is the one thing it is asked. */
static void model_step(ad_t *s, const pf_ctrl_in *in, double dt)
{
	if (!(s->tau > 0) || !(s->K > 0) || !(dt > 0)) { s->surplus = 0; return; }
	double drive = clampd(in->u_prev_applied, 0, 1);
	s->mdl += (drive - s->mdl) * (dt / s->tau);

	/* keep the last theta seconds of it */
	s->ring_t[s->ring_head] = in->now_s;
	s->ring_y[s->ring_head] = s->mdl;
	s->ring_head = (s->ring_head + 1) % PF_MDL_RING;
	if (s->ring_n < PF_MDL_RING) s->ring_n++;

	/* what the model said one dead time ago; linear between the two samples that straddle it */
	double want = in->now_s - clampd(s->theta, THETA_MIN, THETA_MAX);
	double then = s->mdl, prev_t = 0, prev_y = 0;
	bool have = false;
	for (int k = 0; k < s->ring_n; k++) {
		int i = (s->ring_head - 1 - k + 2 * PF_MDL_RING) % PF_MDL_RING;
		if (s->ring_t[i] <= want) {
			then = have && prev_t > s->ring_t[i]
			     ? s->ring_y[i] + (prev_y - s->ring_y[i]) * (want - s->ring_t[i]) / (prev_t - s->ring_t[i])
			     : s->ring_y[i];
			break;
		}
		prev_t = s->ring_t[i]; prev_y = s->ring_y[i]; have = true;
		/* ran out of history: the model has not been going for a whole dead time yet, so there is
		   nothing committed that we can vouch for */
		if (k == s->ring_n - 1) { then = s->mdl; }
	}
	s->surplus = clampd(s->K * (s->mdl - then), -SURPLUS_MAX, SURPLUS_MAX);
}

static double update(void *self, const pf_ctrl_in *in, pf_ctrl_dbg *dbg)
{
	ad_t *s = self;
	if (!s->have_last || s->setpoint_c != in->setpoint_c) reset(self, in);
	/* The daemon interpolates the tuning library for whatever set point is being held, so
	 * these change as the set point moves. Take them whenever they differ from what we are using. */
	bool sch = in->sched_PB_c > 0 && in->sched_Ti > 0;
	if (sch != s->sch_valid || (sch && (in->sched_PB_c != s->sch_PB_c || in->sched_Ti != s->sch_Ti || in->sched_Td != s->sch_Td))) {
		s->sch_valid = sch;
		s->sch_PB_c = in->sched_PB_c;
		s->sch_Ti = in->sched_Ti;
		s->sch_Td = in->sched_Td;
		/* A different entry from the tuning library is a different anchor, so the lesson for this
		 * band is re-applied against it rather than left pointing at the old one. */
		use_band(s, in->setpoint_c);
	}
	/* The plant the library measured at this set point, when it has one. It comes from the step
	 * into this temperature rather than from the last light, so the prediction is built on how the
	 * grill answers fuel HERE -- a low smoke set point and a hot sear are not the same plant, and
	 * one model for both over-predicts at one end and under-predicts at the other. */
	if (in->sched_K > 0 && in->sched_tau > 0) {
		s->K = clampd(in->sched_K, K_MIN, K_MAX);
		s->tau = clampd(in->sched_tau, TAU_MIN, TAU_MAX);
		if (in->sched_theta > 0) s->theta = clampd(in->sched_theta, THETA_MIN, THETA_MAX);
	}
	double dt = in->now_s - s->last_t;
	if (dt <= 0) dt = in->cycle_time_s > 0 ? in->cycle_time_s : 1;
	/* What the loop acts on is the pit plus what is already on its way to it. */
	model_step(s, in, dt);
	double e_true = in->pit_c - in->setpoint_c;
	double e = e_true + s->surplus;
	s->ff = s->ff_gain * in->u_ff;
	s->p = s->kp * e;
	/* integrate only near the target (and never while pushing into a clamp): the approach is handled by
	 * P + feed-forward + the coast look-ahead, so the integrator cannot wind up on the way there */
	bool sat_push = (in->saturated > 0 && e < 0) || (in->saturated < 0 && e > 0);
	bool in_band = fabs(e_true) <= IBAND_C;
	if (in_band && !s->in_band && s->ki != 0) {
		/* arriving at the target: whatever the integrator accumulated on the way is approach wind-up, not a
		 * steady-state correction. Keep at most +/-0.15 duty of it so the pit does not sag, drop the rest. */
		double lim = 0.15 / fabs(s->ki);
		if (s->inter > lim) s->inter = lim;
		if (s->inter < -lim) s->inter = -lim;
	}
	s->in_band = in_band;
	/* A probe that drops out for a moment hands the controller a reading that is not a number. One
	 * addition of it to the integrator poisons the integrator for ever, because every later
	 * comparison against it is false and nothing clears it, so the loop never recovers even after
	 * the probe comes back. Integrate only real numbers, and throw away an accumulator that has
	 * already gone bad. */
	if (!isfinite(s->inter)) s->inter = 0;
	/* The integrator is fed the PREDICTED error, like the rest of the law. This is the whole point
	 * of the Smith structure: the pit spends half an hour below its set point on the way in, and an
	 * integrator watching the measurement banks every second of that as a deficit to be repaid,
	 * which is precisely the wind-up that throws the pit past the target once it arrives. Fed the
	 * prediction, it stops accumulating as soon as enough fuel is committed to close the gap, which
	 * is the moment the deficit stops being real. The prediction is zero at steady state, so this
	 * costs no accuracy where the integrator actually earns its keep. */
	if (!sat_push && isfinite(e) && isfinite(dt)) s->inter += e * dt;
	/* The integral never opposes a large error: a negative integral while the pit is far below the
	 * target (or positive while far above) is left-over wind-down, not a steady-state correction.
	 * This too is about where the pit REALLY is -- a pit sitting on its set point with fuel still on
	 * its way reads as a large error to the prediction, and throwing the integral away there would
	 * discard a correction that is doing its job. */
	if (e_true < -IBAND_C && s->inter < 0) s->inter = 0;
	if (e_true > IBAND_C && s->inter > 0) s->inter = 0;
	s->i = s->ki * s->inter;
	double lim = 0.5;
	if (s->i > lim) { s->i = lim; s->inter = s->ki != 0 ? lim / s->ki : 0; }
	if (s->i < -lim) { s->i = -lim; s->inter = s->ki != 0 ? -lim / s->ki : 0; }
	double derv = (in->pit_c - s->last_pit) / dt;
	s->d = s->kd * derv;
	s->u = s->ff + s->p + s->i + s->d;
	/* The coast look-ahead that used to sit here -- cut back to the feed-forward once the pit,
	 * rising at its current rate, would reach the target within one dead time -- is gone. It was
	 * this same idea taken from the slope instead of from a model, and it could only ever cut back
	 * TO the steady-state feed, never below it, which is why it still left ten degrees of overshoot
	 * on the table. The prediction above does the whole job, and two mechanisms aiming at one thing
	 * fight each other. */
	s->last_t = in->now_s;
	s->last_pit = in->pit_c;
	s->last_err = e;
	/* The monitor asks how well the GRILL is holding, so it is shown the real error. Judging it on
	 * the predicted one would have it reacting to the model: a pit sitting exactly on the set point
	 * with fuel still on its way looks like an error to the prediction, and the band would be
	 * adjusted for something that has not happened. */
	monitor(s, in, e_true);
	if (dbg) {
		dbg->p = s->p; dbg->i = s->i; dbg->d = s->d; dbg->ff = s->ff; dbg->error = e; dbg->derivative = derv; dbg->integral = s->inter;
		/* PB, Ti and Td here are what the loop is running on, with nothing applied on top. */
		snprintf(dbg->note, sizeof dbg->note, "ff %.2f · PB %.0f Ti %.0f Td %.0f%s%s", s->ff, pf_delta_from_c(s->PB_c, s->units), s->Ti, s->Td, s->sch_valid ? " tuned" : s->auto_tune && s->l_valid ? " learned" : "", s->surplus > 0.5 ? " · holding back" : "");
	}
	return s->u;
}

static void configure(void *self, const char *json) { ad_t *s = self; apply_config(s, json); s->have_last = false; }

/* Throw away one half of what is held, or both, and write that down. Clearing the measurement
 * takes the grill back to the numbers that were typed; clearing the refinement leaves the
 * measurement standing and starts the learning again from it. */
static void forget(void *self, unsigned what)
{
	ad_t *s = self;
	if (what & PF_FORGET_TUNING) {
		s->l_PB_c = s->l_Ti = s->l_Td = 0;
		s->l_valid = false;
		s->l_ts = 0;
		s->l_src[0] = 0;
		s->theta = THETA_DEFAULT; s->K = K_DEFAULT; s->tau = TAU_DEFAULT;
	}
	if (what & PF_FORGET_REFINEMENT) {
		for (int i = 0; i < PF_SCALE_BANDS; i++) { s->band_learned[i] = 0; s->band_anchor[i] = 0; }
		s->band_PB_c = 0;
		window_reset(s, s->last_t);
	}
	recompute(s);
	save_learned(s);
	if (s->env && s->env->log)
		s->env->log(PF_LVL_INFO, "adaptive", "forgot %s; band now %.1f C",
		            (what & PF_FORGET_TUNING) && (what & PF_FORGET_REFINEMENT) ? "everything it had learned and been told"
		            : (what & PF_FORGET_TUNING) ? "the tuning it was given" : "what it had refined for itself", s->PB_c);
}

static int state_json(void *self, char *out, size_t n)
{
	ad_t *s = self;
	return snprintf(out, n, "{\"kp\":%.6g,\"ki\":%.6g,\"kd\":%.6g,\"ff\":%.4f,\"p\":%.4f,\"i\":%.4f,\"d\":%.4f,\"u\":%.4f,"
	                "\"PB_c\":%.2f,\"Ti\":%.1f,\"Td\":%.1f,\"learned\":%s,\"auto_tune\":%s,\"learned_ts\":%.0f,\"src\":\"%s\","
	                "\"surplus\":%.2f,\"mdl\":%.3f,\"K\":%.0f,\"tau\":%.0f,\"theta\":%.0f}",
	                s->kp, s->ki, s->kd, s->ff, s->p, s->i, s->d, s->u, s->PB_c, s->Ti, s->Td,
	                s->l_valid ? "true" : "false", s->auto_tune ? "true" : "false", s->l_ts, s->l_src,
	                s->surplus, s->mdl, s->K, s->tau, s->theta);
}

/* Ku/Pu from a relay autotune (Tyreus-Luyben), or K/tau/theta from the passive plant model (SIMC).
 * Both are softened because the feed-forward carries the load, blended with what was learned before,
 * and bounded to sane grill values. */
static void apply_tuning(void *self, double Ku, double Pu, double K, double tau, double theta)
{
	ad_t *s = self;
	double PB, Ti, Td;
	const char *src;
	/* Each measurement is designed from by the rule written for it. A relay test measured an
	 * ultimate gain and a period, and Tyreus-Luyben turns exactly those two numbers into a tuning.
	 * A startup rise was fitted to a model, and SIMC designs from a model. Routing the relay
	 * through the model as well meant borrowing a static gain it never saw and splitting its phase
	 * lag between a time constant and a dead time -- and the band that came out was proportional to
	 * that dead time, which moved by two thirds between two runs on the same grill. */
	bool relay = Ku > 0 && Pu > 0;
	src = relay ? "relay" : "model";
	if (theta > 0) s->theta = clampd(theta, THETA_MIN, THETA_MAX);
	/* the model the prediction runs on: whatever the grill last measured about itself */
	if (K > 0) s->K = clampd(K, K_MIN, K_MAX);
	if (tau > 0) s->tau = clampd(tau, TAU_MIN, TAU_MAX);
	if (relay) pf_tuning_from_relay(Ku, Pu, &PB, &Ti, &Td);
	else if (K > 0 && tau > 0 && theta > 0) pf_tuning_from_plant(K, tau, theta, &PB, &Ti, &Td);
	else return;
	if (!(PB > 0) || !(Ti > 0)) return;
	/* The configured PB, Ti and Td are where the grill starts, not a leash on what it learns. A
	 * relay test drives the plant deliberately and measures it, and the answer can be several
	 * times the starting guess: tying it to a band around that guess would throw away the
	 * measurement and hand back the guess. Only the crude passive fit, which is inferred from
	 * whatever the cook happened to do, is kept near the baseline. Absolute bounds still apply,
	 * because a number outside them is a fault rather than a grill. */
	if (!relay) {
		PB = clampd(PB, s->cfg_PB_c * 0.5, s->cfg_PB_c * 1.5);
		Ti = clampd(Ti, s->cfg_Ti * 0.5, s->cfg_Ti * 2.0);
		Td = clampd(Td, 0, s->cfg_Td * 2.0);
	}
	PB = clampd(PB, 15, 400); Ti = clampd(Ti, 60, 3600); Td = clampd(Td, 0, 240);
	/* Averaging a fresh measurement with the last one halves how much of it actually arrives, and
	 * after a profile run each set point is measured once, so blending would leave every entry
	 * halfway to the one before it. A measurement replaces; only the passive fit is smoothed. */
	if (s->l_valid && !relay) { PB = 0.5 * (PB + s->l_PB_c); Ti = 0.5 * (Ti + s->l_Ti); Td = 0.5 * (Td + s->l_Td); }
	s->l_PB_c = PB; s->l_Ti = Ti; s->l_Td = Td; s->l_valid = true; s->l_ts = (double)time(NULL);
	snprintf(s->l_src, sizeof s->l_src, "%s", src);
	/* A fresh measurement supersedes what the monitor had concluded -- but only where the
	 * measurement was taken.
	 *
	 * A tuning run holds one temperature and measures the grill there. What it says about 350 F
	 * has nothing to do with what the grill does at 250, and the corrections settled on around 250
	 * were hard won over whole cooks. Rescaling every band on every run meant a tune at one end of
	 * the range quietly rewrote the other end, which is how a good tuning gets worse by adding
	 * another one. Only the band this run belongs to is touched, and half of its correction is kept
	 * as a hint rather than carried over whole onto a number it was never measured against. */
	int band = band_of(s->setpoint_c);
	if (band >= 0 && band < PF_SCALE_BANDS) {
		double r = s->band_learned[band] > 0 && s->band_anchor[band] > 0 ? s->band_learned[band] / s->band_anchor[band] : 0;
		if (r > 0) {
			s->band_learned[band] = PB * clampd(1.0 + 0.5 * (r - 1.0), LEARN_MIN, LEARN_MAX);
			s->band_anchor[band] = PB;
		} else {
			s->band_learned[band] = 0;
			s->band_anchor[band] = 0;
		}
	}
	recompute(s);
	save_learned(s);
	if (s->env && s->env->log) s->env->log(PF_LVL_INFO, "adaptive", "tuning learned from %s: PB %.1f C, Ti %.0f s, Td %.0f s%s", src, PB, Ti, Td, s->auto_tune ? "" : " (auto-tune off: stored only)");
}

static const pf_controller_ops ops = {
	.abi = PF_CONTROLLER_ABI, .id = "adaptive", .name = "Adaptive (self-learning)",
	.description = "Learns the steady-state feed your grill needs for each set point and ambient temperature across cooks, derives its PID tuning from the plant model measured during every startup, and keeps adjusting the loop gain from how each cook behaves. Improves with every cook.",
	.author = "PiFire", .config_schema_json = schema, .recommend = { 20, 0.08, 0.9 },
	.create = create, .destroy = destroy, .reset = reset, .update = update, .configure = configure, .state_json = state_json, .apply_tuning = apply_tuning, .forget = forget,
};
const pf_controller_ops *pf_controller_adaptive(void) { return &ops; }
