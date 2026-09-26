#pragma once
/* Published status snapshot: what the UI, MQTT, display and CLI see. Written by the control
 * thread, copied by readers. All temperatures Celsius; pf_status_to_json converts to user units. */
#include "pifire/common.h"
#include "pifire/controller.h"
#include "probes/probes.h"
#include "core/notify.h"
#include <cJSON.h>

/* The tuning that governs the set point being asked for: the library entry if there is one, else
 * what the controller is carrying, else what was typed. */
typedef struct { double PB_c, Ti, Td; char src[8]; bool valid; } pf_ctrl_tuning;

typedef struct {
	double t;                 /* monotonic */
	double wall;              /* unix seconds */
	pf_mode mode, next_mode;
	double mode_start;        /* monotonic */
	double aim_since;         /* monotonic: last mode change or set-point change */
	double setpoint_c;
	bool s_plus, pwm_control;
	int duty_cycle;           /* configured DC fan % */
	unsigned outputs;         /* bitmask pf_output */
	int fan_pct;
	double u_raw, u_applied;
	int saturated;
	double cycle_s;
	bool lid_open;
	double lid_open_until;
	bool target_reached;
	double startup_duration, shutdown_duration, prime_duration, prime_amount;
	bool coldstart_active, coldstart_reached;
	double coldstart_baseline_c, coldstart_deadline;
	double startup_exit_c;      /* 0 = startup runs the full timer */
	int pmode;
	double cook_start_wall;     /* 0 when no cook is running */
	char error_code[32];
	char error_msg[128];
	char controller_id[32];
	pf_ctrl_dbg ctrl_dbg;
	/* The tuning that governs the set point being asked for, recomputed every publish rather than
	 * left over from the last cycle the controller ran. The note inside ctrl_dbg is only written
	 * while the grill is holding, so between cooks it showed whatever was in force during the last
	 * one -- which after a tuning run is the one moment it is most certainly wrong. */
	pf_ctrl_tuning tuning;
	double ambient_c;
	int reignite_retries_left;
	pf_sensors sensors;
	struct {
		int after; double eta_s, limit_high_c, limit_low_c;
		/* the same estimate aimed at the next step rather than the target, and which step that is */
		double eta_step_s; char next_step[24];
		/* the named temperatures on the way to the target, and what each has already said */
		int nsteps; struct { char name[24]; double temp_c; bool fired; } steps[PF_MAX_STEPS];
	} notify[PF_MAX_PROBES]; /* parallel to sensors.p */
	struct { bool running, paused; double remaining, duration; int after; } timer;
	struct { bool active, waiting, needs_lid; char name[64]; int step, nsteps, stage, stages; pf_mode step_mode; double remaining_s; double clock_s; bool at_temp; int id; unsigned char flags[16]; char message[128]; } recipe;
	bool autotune_active; int autotune_crossings; double u_ff;
	int hopper_pct;           /* -1 unknown */
	bool sim;
} pf_status;

void pf_status_publish(const pf_status *s);
void pf_status_get(pf_status *out);
unsigned pf_status_generation(void);
/* Just the mode, without copying the whole status. */
pf_mode  pf_status_mode(void);
/* Full JSON for /api/v1/status and the WebSocket stream (caller frees). */
cJSON *pf_status_to_json(const pf_status *s, pf_units units);
