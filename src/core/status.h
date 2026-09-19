#pragma once
/* Published status snapshot: what the UI, MQTT, display and CLI see. Written by the control
 * thread, copied by readers. All temperatures Celsius; pf_status_to_json converts to user units. */
#include "pifire/common.h"
#include "pifire/controller.h"
#include "probes/probes.h"
#include <cJSON.h>

typedef struct {
	double t;                 /* monotonic */
	double wall;              /* unix seconds */
	pf_mode mode, next_mode;
	double mode_start;        /* monotonic */
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
	bool coldstart_active;
	double coldstart_baseline_c, coldstart_deadline;
	char error_code[32];
	char error_msg[128];
	char controller_id[32];
	pf_ctrl_dbg ctrl_dbg;
	double ambient_c;
	int reignite_retries_left;
	pf_sensors sensors;
	struct { int after; double eta_s, limit_high_c, limit_low_c; } notify[PF_MAX_PROBES]; /* parallel to sensors.p */
	struct { bool running, paused; double remaining, duration; int after; } timer;
	struct { bool active, waiting; char name[64]; int step, nsteps; pf_mode step_mode; double remaining_s; char message[128]; } recipe;
	bool autotune_active; int autotune_crossings; double u_ff;
	int hopper_pct;           /* -1 unknown */
	bool sim;
} pf_status;

void pf_status_publish(const pf_status *s);
void pf_status_get(pf_status *out);
unsigned pf_status_generation(void);
/* Full JSON for /api/v1/status and the WebSocket stream (caller frees). */
cJSON *pf_status_to_json(const pf_status *s, pf_units units);
