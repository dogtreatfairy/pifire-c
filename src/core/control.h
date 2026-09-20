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
	bool req_pending; pf_mode req_mode; double req_setpoint_c; bool req_prime_then_startup;
	double setpoint_c;
	bool s_plus, pwm_control; int duty_cycle;
	/* cycle + controller */
	pf_cycle cycle; pf_cycle_cfg ccfg;
	const pf_controller_ops *cops; void *cinst; pf_env cenv; pf_ctrl_dbg dbg;
	double u_raw, u_applied; int saturated; bool ctrl_reset_needed;
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
		double rise_t0, rise_T0_c, rise_u_sum; int rise_n; double rise_t28, rise_t63; bool rise_active;
	} learn;
	/* relay autotune (core-owned; controller update() is bypassed while active) */
	struct {
		bool active; int phase; double u_center, h, hyst_c, start_t, last_cross_t;
		double peak_max, peak_min; int crossings; double periods[8]; double amps[8];
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
