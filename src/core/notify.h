#pragma once
/* Probe targets, limit alarms, the cook timer and ETA estimation. Runs inside the control thread
 * (pf_notify_tick from pf_control_step). Temperatures in Celsius. */
#include "pifire/common.h"
#include "probes/probes.h"
#include <cJSON.h>

#define PF_ETA_SAMPLES 400   /* 20 min at 3 s */

enum { PF_AFTER_NONE = 0, PF_AFTER_KEEPWARM = 1, PF_AFTER_SHUTDOWN = 2 };

typedef struct {
	char label[PF_LABEL_LEN];
	double target_c;        /* 0 = none */
	int after;              /* PF_AFTER_* */
	double limit_high_c, limit_low_c;   /* 0 = disabled */
	bool high_tripped, low_tripped;
	double eta_s;           /* -1 unknown */
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
