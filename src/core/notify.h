#pragma once
/* Probe targets, limit alarms, the cook timer and ETA estimation. Runs inside the control thread
 * (pf_notify_tick from pf_control_step). Temperatures in Celsius. */
#include "pifire/common.h"
#include "probes/probes.h"
#include <cJSON.h>

#define PF_ETA_SAMPLES 400   /* 20 min at 3 s */

enum { PF_AFTER_NONE = 0, PF_AFTER_KEEPWARM = 1, PF_AFTER_SHUTDOWN = 2 };

/* A step in a cook: a temperature on the way to the target with a name on it -- "Flip" at 120,
 * "Wrap" at 165 -- which says something once, when it is crossed going up. It is what every probe
 * app worth using has: the target is where the meat comes off, and the steps are what you have to
 * be at the grill for before then. Held in settings so they survive a restart mid-cook. */
#define PF_MAX_STEPS 4
typedef struct {
	char name[24];
	double temp_c;
	bool fired;
} pf_notify_step;

typedef struct {
	char label[PF_LABEL_LEN];
	double target_c;        /* 0 = none */
	char meat[24], done[24]; /* what the target was chosen for -- Beef, Medium rare -- so the card can say so */
	double finish_c;         /* what it rests up to once off the heat, 0 = not said */
	/* A rest-to target: the temperature wanted after resting. The take-off point (target_c) is
	 * then worked out as the probe climbs -- the rest temperature plus a small margin, less the
	 * carry-over its rate of climb predicts -- rather than fixed. 0 = target_c is a take-off. */
	double rest_c;
	/* After a rest-to alert, the rest itself is watched: the temperature and climb at the alert,
	 * then the peak, so the carry-over that actually happened can correct the next estimate. */
	bool rest_watch; double pull_c, pull_rate, pull_predicted_c, rest_peak_c, rest_peak_t, rest_watch_t;
	pf_notify_step steps[PF_MAX_STEPS];
	int nsteps;
	int after;              /* PF_AFTER_* */
	double limit_high_c, limit_low_c;   /* 0 = disabled */
	bool high_tripped, low_tripped;
	double eta_s;           /* -1 unknown */
	/* The same estimate, aimed at the next step rather than at the target, so the grill can say
	 * "flip it in a minute" as well as "it is nearly done". -1 when there is no step left to reach
	 * or the probe is not climbing. */
	double eta_step_s;
	char next_step[24];
	bool reached;           /* the target has been met: latched so a rule sees target and temp together */
	bool eta_warned;        /* the "about N minutes to target" notice went out for this target */
	int eta_hits;           /* consecutive estimates under the warning threshold */
	double hist[PF_ETA_SAMPLES];
	int hist_len, hist_head;
	double last_sample_t;
} pf_notify_probe;

typedef struct {
	bool running, paused;
	double end_t;           /* monotonic */
	double remaining;       /* when paused */
	double duration;
	int after;
} pf_timer;

typedef struct {
	pf_notify_probe probes[PF_MAX_PROBES];
	int n;
	pf_timer timer;
	double last_eta_t;
	int pending_action;     /* PF_AFTER_* requested by a fired trigger; consumed by control */
	double cook_temp_c;     /* the grill's set point, when it has one, for probes with no ambient sensor of their own */
} pf_notify;

void pf_notify_init(pf_notify *n);
/* Keep probe entries aligned with the current sensor snapshot (by label). */
void pf_notify_sync(pf_notify *n, const pf_sensors *s);
/* Evaluate triggers. Sets n->pending_action when a fired trigger asks for keep-warm/shutdown. */
void pf_notify_tick(pf_notify *n, const pf_sensors *s, pf_mode mode, double now, pf_units units);
int  pf_notify_set_target(pf_notify *n, const char *label, double target_c, int after);
/* the words behind the target; either may be empty. Cleared when the target is. */
void pf_notify_set_target_note(pf_notify *n, const char *label, const char *meat, const char *done, double finish_c);
/* Aim at a rested temperature instead of a take-off one (see rest_c). Sets the target too. */
/* how far above the rest asked for a rest-to target aims, so the actual rest lands on it:
 * notify.rest_margin in the user's units, 2 F by default */
double pf_notify_rest_margin_c(void);
#define PF_REST_MARGIN_C pf_notify_rest_margin_c()
int  pf_notify_set_rest(pf_notify *n, const char *label, double rest_c, int after);
/* the take-off point for a rest-to target at this rate of climb */
double pf_notify_rest_pull_c(double rest_c, double rate_c_s, double centre_c, double cook_c, const char *meat);
int  pf_notify_set_limits(pf_notify *n, const char *label, double high_c, double low_c);
void pf_notify_timer_start(pf_notify *n, double seconds, int after, double now);
void pf_notify_timer_pause(pf_notify *n, double now);
void pf_notify_timer_resume(pf_notify *n, double now);
void pf_notify_timer_cancel(pf_notify *n);
const pf_notify_probe *pf_notify_find(const pf_notify *n, const char *label);
/* Python-compatible estimator: smoothed, exponentially weighted linear regression. */
double pf_notify_estimate_eta(const double *temps, int n, double target, double interval_s);
/* How fast a probe is climbing, C per second, 0 when it is not climbing or has too little history. */
double pf_notify_probe_rate(const pf_notify *n, const char *label);
/* How much further the centre will climb after it comes off the heat, in C, from that rate.
 *
 * The meat keeps cooking on the heat already in it, and the rate of climb at the moment it comes
 * off is the measure of how much there is. The rate decays roughly exponentially once the heat is
 * away, so the total is the rate times a time constant -- about seven minutes for a piece you
 * would put on a grill, which is the figure the usual carryover tables come to when you work
 * backwards from them. Capped, because an estimate built on a slope should not be trusted to
 * predict a big number. */
#define PF_CARRYOVER_TAU_S 420.0
#define PF_CARRYOVER_MAX_C 5.0
static inline double pf_carryover_c(double rate_c_s)
{
	double c = rate_c_s * PF_CARRYOVER_TAU_S;
	return c < 0 ? 0 : c > PF_CARRYOVER_MAX_C ? PF_CARRYOVER_MAX_C : c;
}
/* Carry-over from the cook itself: the centre temperature, its rate of climb, and the temperature
 * the meat is cooking in (the probe's own ambient sensor, or the grill's set point).
 *
 * The centre climbs towards the cooking temperature with a time constant that grows with the
 * square of the cut's thickness, and the ratio of the gap to the climb, (T_cook - T_centre) / rate,
 * is that time constant, measured: a flank steak on a hot grill shows about half an hour, a 2"
 * steak about three hours, a roast or a brisket three to four. The heat stored in the outer layers
 * at the pull goes with the gap, and how much of it reaches the centre rather than the air goes
 * with the time constant -- a thin cut loses its surface heat to the room faster than it can move
 * inward -- so the rise is
 *
 *     carry = g x (T_cook - T_centre),   g = tau / 300000 s, kept between 0.004 and 0.06
 *
 * g is fitted to the usual carry-over figures: a flank steak off a 450 F grill about 2 F, a 1"
 * steak about 7, a 2" steak about 10, a standing rib roast off 350 F about 10, a roast off 225 F
 * about 4, a brisket at 200 F under 2. Those figures are rules of thumb, which is why the per-meat
 * correction below still applies on top: the model says why two steaks differ, the grill's own
 * rests say by how much on this grill. Capped at 8 C. Falls back to the rate-only estimate when
 * there is no cooking temperature to go on. */
double pf_carryover_model_c(double rate_c_s, double centre_c, double cook_c);

/* The same estimate corrected by what this grill's rests of this meat have actually done.
 *
 * Carry-over is heat already inside the meat moving inward after it comes off: how much there
 * is depends on how steep the gradient is between the surface and the centre, and how long it
 * takes to even out goes with the thickness squared. The centre's rate of climb at the pull
 * carries both of those, which is why it is the base; what it cannot carry is the thickness on
 * its own -- a thick roast climbs slowly and still coasts a long way -- and nobody should have to
 * measure a steak to cook it. So each rest-to target that ends in a real rest (the probe peaks
 * and falls away within the half hour) is compared with what was predicted, and the ratio is
 * kept per meat, recency-weighted. The cap widens to 8 C (about 15 F) once a meat has shown it
 * carries more than the base allows, which is where thick roasts off a hot grill land. */
double pf_carryover_learned_c(double rate_c_s, double centre_c, double cook_c, const char *meat);
/* the learned correction for a meat, 1 when nothing has been learned; and one observation of it */
double pf_carryover_k(const char *meat);
void   pf_carryover_learn(const char *meat, double predicted_c, double actual_c);
