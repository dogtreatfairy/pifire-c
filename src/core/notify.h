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
} pf_notify;

void pf_notify_init(pf_notify *n);
/* Keep probe entries aligned with the current sensor snapshot (by label). */
void pf_notify_sync(pf_notify *n, const pf_sensors *s);
/* Evaluate triggers. Sets n->pending_action when a fired trigger asks for keep-warm/shutdown. */
void pf_notify_tick(pf_notify *n, const pf_sensors *s, pf_mode mode, double now, pf_units units);
int  pf_notify_set_target(pf_notify *n, const char *label, double target_c, int after);
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
