#pragma once
/* PiFire controller plugin ABI.
 *
 * A controller turns (pit temperature, setpoint) into a feed ratio u in [0,1]: the fraction of
 * each auger cycle the auger runs. The daemon clamps to [u_min,u_max], applies safety caps and
 * converts to on/off timing. All temperatures in this ABI are Celsius.
 *
 * Out-of-tree plugins: build a shared object exporting
 *     const pf_controller_ops *pf_controller_export(void);
 * and install it under /usr/lib/pifire/controllers/.
 */
#include "pifire/common.h"

#define PF_CONTROLLER_ABI 2

typedef struct {
	double t;          /* monotonic seconds */
	double pit_c;
	double setpoint_c;
	double u_raw;      /* controller output that cycle */
	double u_applied;  /* after clamping / safety */
	double ambient_c;
	int    fan_pct;    /* 0 when fan off */
	unsigned outputs;  /* bitmask of pf_output */
} pf_hist_pt;

/* Ring buffer of recent samples. Index 0 = oldest. */
typedef struct {
	const pf_hist_pt *pts;
	int cap, head, len; /* head = next write slot */
} pf_history;

static inline const pf_hist_pt *pf_history_at(const pf_history *h, int i)
{
	if (!h || i < 0 || i >= h->len) return (const pf_hist_pt *)0;
	return &h->pts[(h->head - h->len + i + h->cap) % h->cap];
}

typedef struct {
	double now_s;                        /* CLOCK_MONOTONIC */
	double pit_c, setpoint_c, ambient_c;
	double u_prev_raw, u_prev_applied;
	/* Tuning measured at this set point, interpolated from the tuning library autotune fills in;
	 * zero when the library is empty, in which case the controller uses its own. A pellet
	 * grill's process gain falls as it gets hotter, so one fixed band cannot suit every set point:
	 * this is how a single controller stays right from 180 F to 450 F. */
	double sched_PB_c, sched_Ti, sched_Td;
	double u_ff;                         /* learned steady-state feed for this set point/ambient (daemon) */
	int    saturated;                    /* -1 clamped at u_min, +1 at u_max, 0 free */
	double cycle_time_s, u_min, u_max;
	bool   target_reached;               /* pit has reached setpoint at least once this HOLD */
	bool   fan_on;
	int    fan_pct;
	const pf_history *hist;
} pf_ctrl_in;

typedef struct {
	double p, i, d, ff;
	double error, derivative, integral;
	char note[64];
} pf_ctrl_dbg;

typedef struct {
	double start_t, end_t;      /* monotonic */
	double setpoint_c, ambient_c;
	double u_mean;              /* mean applied u while at setpoint */
	double pit_rms_err_c;
	int    lid_events;
} pf_episode;

typedef struct pf_controller_ops {
	uint32_t abi;                        /* PF_CONTROLLER_ABI */
	const char *id;                      /* "pid", "pid_clamping", ... */
	const char *name;
	const char *description;
	const char *author;
	const char *config_schema_json;      /* JSON array of option descriptors (same shape as the hardware manifest) */
	struct { double cycle_time, u_min, u_max; } recommend;

	void  *(*create)(const char *config_json, const pf_env *env);
	void   (*destroy)(void *self);
	/* Called on HOLD entry, controller switch and fault clear. CONTRACT (bumpless): initialise
	 * internal state so the first update() would return approximately in->u_prev_applied. */
	void   (*reset)(void *self, const pf_ctrl_in *in);
	/* Called once per cycle in HOLD (never during lid-open, manual override or autotune). */
	double (*update)(void *self, const pf_ctrl_in *in, pf_ctrl_dbg *dbg);
	void   (*configure)(void *self, const char *config_json);
	int    (*state_json)(void *self, char *out, size_t n);

	/* optional (may be NULL) */
	void   (*apply_tuning)(void *self, double Ku, double Pu, double K, double tau, double theta);
	void   (*episode_end)(void *self, const pf_episode *ep);
} pf_controller_ops;

typedef const pf_controller_ops *(*pf_controller_export_fn)(void);
