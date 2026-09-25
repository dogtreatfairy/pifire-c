#pragma once
/* Mode state machine + cycle engine + safety. Driven by pf_control_step(now) from the control
 * thread (real time) or from tests (fake time). All temperatures Celsius. */
#include "core/cycle.h"
#include "core/notify.h"
#include "features/recipe.h"
#include "pifire/common.h"
#include "pifire/controller.h"
#include "probes/probes.h"

#define PF_SS_MAX 8

typedef struct {
	pf_units units;
	double hold_cycle_s, smoke_on_s, smoke_off_s;
	int pmode;
	double u_min, u_max;
	bool lid_detect; double lid_threshold_pct, lid_pause_s;
	bool fan_pid;
	/* safety */
	double min_startup_c, max_startup_c, max_temp_c, restart_hot_c;
	int reignite_retries;
	bool startup_check, allow_manual;
	double manual_override_s, igniter_max_on_s, auger_max_on_s, probe_fault_s, error_cooldown_fan_s;
	/* Dynamic flame-out assist: how far below the set point counts as the fire failing, and how
	 * much recovery from the lowest point counts as it having caught again. */
	bool   use_library;         /* let measured anchors override the typed-in PB/Ti/Td */
	bool   relight_enabled;
	double relight_drop_c, relight_recover_c, relight_recover_step_c, relight_timeout_s;
	bool coldstart; double coldstart_delta_c, coldstart_timeout_s, coldstart_window_s; bool coldstart_exit_on_rise;
	/* startup / shutdown */
	double startup_duration_s, prime_on_startup_g, startup_exit_c, startup_exit_rise_c;
	pf_mode after_startup_mode; double after_startup_setpoint_c;
	bool smartstart; double ss_exit_c; int ss_n; double ss_ranges_c[PF_SS_MAX];
	struct { double startuptime, augerontime; int p_mode; } ss_prof[PF_SS_MAX + 1];
	int startup_pwm_duty;
	double shutdown_s; bool auto_power_off;
	/* smoke plus */
	bool splus_default; double splus_min_c, splus_max_c, splus_on_s, splus_off_s; int splus_duty; bool splus_ramp;
	/* pwm fan */
	bool dc_fan, pwm_control_default; double pwm_update_s; int pwm_hz, pwm_min_duty, pwm_max_duty;
	int pwm_n; double pwm_ranges_c[PF_SS_MAX]; int pwm_profiles[PF_SS_MAX + 1];
	double augerrate; bool prime_ignition;
	double keepwarm_c; bool keepwarm_splus;
	double history_sample_s; bool clear_history_on_startup;
	char controller_id[32];
} pf_cfg;

typedef struct {
	double floor_c;             /* SMOKE/HOLD flame-out floor */
	bool   floor_set;
	bool   relight_active;      /* the igniter is on because the pit fell away from the set point */
	double relight_low_c;       /* the lowest the pit has been since that began */
	double relight_below_since; /* when the pit first fell away, and did not come back */
	bool   stepdown_armed;      /* the set point was lowered a long way; watch for the pit crossing it */
	bool   relight_from_step;   /* this run began at a coast-down crossing, not at a fire falling away */
	double last_sp_c;           /* the set point on the previous tick, to notice it being changed */
	double baseline_c;          /* cold-start: running minimum during the baseline window */
	double baseline_window_end;
	bool   coldstart_active, coldstart_reached;
	double coldstart_deadline;
	double filt_c;              /* 30 s filtered pit for cold-start decisions */
	int    above_count;
	int    reignite_retries_left;
	pf_mode reignite_last;
	double primary_invalid_since;
	double igniter_on_since;
	bool   igniter_locked_out;
	int    ctrl_fault_count;
	double error_fan_until;
	char   error_code[32];
	char   error_msg[128];
} pf_safety;

typedef struct {
	pf_cfg cfg;
	bool sim;
	/* mode */
	pf_mode mode, next_mode;
	double mode_start;
	/* When the grill was last given something new to aim at: a mode change or a new set point.
	 * A pit below its target is only worth reporting once it has had time to climb, and this is
	 * what "had time" is measured from. */
	double aim_since;
	bool req_pending; pf_mode req_mode; double req_setpoint_c; bool req_prime_then_startup;
	double setpoint_c;
	bool s_plus, pwm_control; int duty_cycle;
	/* cycle + controller */
	pf_cycle cycle; pf_cycle_cfg ccfg;
	const pf_controller_ops *cops; void *cinst; pf_env cenv; pf_ctrl_dbg dbg;
	double u_raw, u_applied; int saturated; bool ctrl_reset_needed;
	/* The starting Proportional Band, Integral Time and Derivative Time as last seen, so a change
	 * typed on the controller page can be told apart from the daemon writing back a tuning. */
	double typed_gains[3];
	bool target_reached;
	bool fan_pid_active;
	/* lid / fan */
	bool lid_open; double lid_open_until;
	double fan_toggle_t, fan_update_t; bool fan_ramping; double ramp_end_t;
	/* manual */
	double manual_until[PF_OUT_COUNT];
	/* prime / startup / shutdown */
	double prime_duration_s, prime_amount_g;
	double startup_duration_s, raw_startup_c, startup_exit_c; int ss_profile;
	double startup_base_c;      /* pit when the current Startup/Reignite began (exit_rise reference), NAN if unknown */
	double pit_rate_c_min, pit_rate_last_c, pit_rate_last_t;   /* filtered pit slope, C per minute */
	pf_safety safety;
	pf_notify notify;
	struct {
		bool active, triggered, waiting;   /* waiting = paused for the user after a trigger */
		int step;
		double step_start;
		pf_recipe r;
	} recipe;
	/* learning: steady-state observation window and startup-rise identification */
	struct {
		double steady_since, last_obs_t, last_disturb_t;
		double u_sum, pit_sum, pit_sq; int n;
		double u_ff;
		double rise_t0, rise_T0_c, rise_u_sum; int rise_n; double rise_t28, rise_t63, rise_arrived_t, rise_sp_c;
		double sp_seen_c, sp_since; bool sp_reached, rise_active, rise_from_step;
	} learn;
	/* relay autotune (core-owned; controller update() is bypassed while active) */
/* Half-cycles kept. The conditioning can spend eight of these -- two centrings and two resizes,
 * two crossings each -- and the settling test needs two consecutive cycles that agree, so a budget
 * of fourteen left a well-behaved run barely three cycles to settle in. It is the time limit that
 * is meant to end a hopeless run, not the crossing count ending a healthy one early. */
#define PF_AT_MAX 24
#define PF_AT_MIN_CROSS 5   /* the fewest crossings that can produce a result: two full cycles after the centring */
	struct {
		bool active; int phase; double u_center, h, hyst_c, start_t, last_cross_t;
		/* A ceiling on the swing, set once the relay has shown that the swing it was given
		 * produces far more amplitude than the measurement needs, so that re-centring cannot put
		 * the oversized swing back. Zero until then. */
		double h_cap;
		/* and the other way: a swing deliberately grown because the oscillation was too thin to
		 * read must not be undone by the next re-centring, which sizes the swing from the room
		 * around the centre and knows nothing about why it was widened. */
		double h_floor;
		int resizes;            /* swing resizes this run, kept apart from the centring budget */

		/* `halves` holds the time between successive crossings. A full oscillation is one half
		 * plus the next, which is not the same as twice either one: a grill heats far faster
		 * than it cools, so its limit cycle is lopsided. */
		/* Per half-cycle: how long it lasted and the highest and lowest the pit reached in it.
		 * A full oscillation is two halves, and its amplitude spans both, so the peaks are kept
		 * separately rather than collapsed into one span. */
		double peak_max, peak_min; int crossings;
		double halves[PF_AT_MAX]; double hi_peak[PF_AT_MAX]; double lo_peak[PF_AT_MAX];
		int recentres;          /* times the swing has been re-centred after a stalled half-cycle */
		double err_at_move;     /* pit error when the centre last moved, to notice the pit walking away */
		double last_recentre_t; /* the grill needs time to answer a new centre before it is judged again */
		/* The duty actually delivered on each half of the swing. The cycle engine clamps to
		 * [u_min, u_max], so on a grill that holds a low set point on very little fuel the low
		 * half arrives at the minimum feed rather than where it was aimed. The ultimate gain is
		 * computed from the size of the swing, so it has to be the delivered size, not the
		 * requested one. */
		double hi_sum, lo_sum; int hi_n, lo_n;
		/* The feed delivered over the full cycle just finished. At a limit cycle the average of a
		 * relay's output over one period is the load the plant actually needs, so this is the
		 * grill's own answer to "what does it take to hold this?" and where the centre belongs. */
		double cyc_sum; int cyc_n;
		double worst_split;     /* the most lopsided full cycle seen, as time-high / time-low */
		int adjusts;            /* times the relay has been re-conditioned (centre or swing) */
		int adjust_at_cross;    /* the crossing it was last conditioned at: what follows is the measurement */
		/* Where the swing actually sat, over the cycles the result is taken from. A limit cycle
		 * that averages off the set point is measuring the grill somewhere other than where it is
		 * being asked to hold, and every number that comes out of it inherits the error. */
		double meas_err_sum; int meas_err_n;
		double last_load;       /* the average feed the last complete cycle delivered */
		char note[64];
	} autotune;
	/* sensors */
	pf_sensors sensors; double pit_c; bool pit_valid; double ambient_c; bool ambient_from_probe;
	/* bookkeeping */
	double auger_on_since, auger_total_on_s, cook_start_wall, cook_max_pit_c;
	int hopper_pct;
	double last_step;
	bool power_off_requested;
} pf_control;

void pf_control_init(pf_control *c, bool sim);
/* Size the relay's swing to the room around its centre, honouring the cap and floor a run has
 * learned. Exposed so the tests can check that a deliberate widening survives a re-centring. */
void pf_control_autotune_size(pf_control *c);
/* Have the last two oscillations agreed closely enough to call this a limit cycle? A run that
 * cannot say yes has measured a transient, and files nothing. Exposed for the tests. */
bool pf_control_autotune_settled(const pf_control *c);
void pf_control_shutdown(pf_control *c);
void pf_control_reload_settings(pf_control *c);
/* One tick: consume commands, read sensors, run mode logic + safety, drive outputs, publish status. */
void pf_control_step(pf_control *c, double now);
/* Request a mode change (from the queue handler or tests). setpoint_c <= 0 keeps the current one. */
void pf_control_request(pf_control *c, pf_mode mode, double setpoint_c);
/* Called once after the first sensor poll: handle unclean-restart recovery. */
void pf_control_boot_check(pf_control *c, bool unclean_restart, double now);
/* Warm restart (software update while cooking): snapshot the running cook as JSON (NULL when nothing
 * is worth resuming), and restore it in the new process. Resume returns true when a mode was re-entered. */
char *pf_control_resume_json(const pf_control *c, double now);
bool  pf_control_resume(pf_control *c, const char *json, double now);

/* safety.c */
void pf_safety_reset(pf_control *c);
void pf_safety_on_startup_enter(pf_control *c, double now);
void pf_safety_on_startup_exit(pf_control *c, double now);
/* Evaluate every tick. Returns 0, or a requested mode (PF_MODE_REIGNITE / PF_MODE_ERROR) with
 * error_code set for ERROR. May also force individual outputs off via pf_outputs. */
int  pf_safety_tick(pf_control *c, double now);
bool pf_safety_startup_can_finish(pf_control *c, double now);
void pf_safety_set_error(pf_control *c, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
