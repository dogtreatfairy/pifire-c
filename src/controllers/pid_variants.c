/* Ports of the PiFire PID variants:
 *   pid_clamping            classic de/dt derivative + integrator clamping (Mark Alston)
 *   pid_clamping_percent_pb same, with PB expressed as % of the set point
 *   pid_ac                  auto-center PID with overshoot handling (Ryan Steel / James Weber)
 *   pid_sp                  auto-center + Smith predictor
 *   pid_parallel            parallel-form Kp/Ki/Kd with optional clamping
 * All internal math is in Celsius; PB/stable-window are converted from the user's units. The
 * auto-center formula uses the set point in °F as the original does. */
#include "controllers/pid_common.h"
#include "pifire/controller.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { V_CLAMP, V_CLAMP_PCT, V_AC, V_SP, V_PARALLEL } variant_t;

typedef struct {
	variant_t v;
	const pf_env *env;
	pf_units units;
	double PB, Ti, Td;             /* PB in °C for absolute variants; % for CLAMP_PCT */
	double Kp_cfg, Ki_cfg, Kd_cfg; /* parallel */
	bool clamping;
	double center_factor, stable_window_c, tau, theta;
	double kp, ki, kd, pb_c, center;
	double inter, derv, p, i, d, u, err_last, last_pit, last_t, last_set_t, start_change_c, setpoint_c;
	bool new_target, have_last;
} pv_t;

static void gains_pb(pv_t *s, double pb_c)
{
	s->pb_c = pb_c;
	s->kp = pb_c == 0 ? 0 : -1.0 / pb_c;
	s->ki = s->Ti == 0 ? 0 : s->kp / s->Ti;
	s->kd = s->kp * s->Td;
}

static void apply_config(pv_t *s, const char *json)
{
	cJSON *c = json ? cJSON_Parse(json) : NULL;
	s->units = pf_pid_cfg_units(c);
	switch (s->v) {
	case V_PARALLEL:
		s->Kp_cfg = pf_pid_cfg_num(c, "Kp", 0.01);
		s->Ki_cfg = pf_pid_cfg_num(c, "Ki", 0.000055);
		s->Kd_cfg = pf_pid_cfg_num(c, "Kd", 0.45);
		s->clamping = pf_pid_cfg_bool(c, "Clamping", true);
		/* gains are per degree of the user's units; scale to °C */
		double f = s->units == PF_UNITS_C ? 1.0 : 9.0 / 5.0;
		s->kp = -s->Kp_cfg * f; s->ki = -s->Ki_cfg * f; s->kd = -s->Kd_cfg * f;
		break;
	case V_CLAMP_PCT:
		s->PB = pf_pid_cfg_num(c, "PB", 42.0);
		s->Ti = pf_pid_cfg_num(c, "Ti", 180.0);
		s->Td = pf_pid_cfg_num(c, "Td", 45.0);
		gains_pb(s, s->pb_c > 0 ? s->pb_c : 30);
		break;
	default:
		s->PB = pf_delta_to_c(pf_pid_cfg_num(c, "PB", s->v == V_CLAMP ? 100.0 : 60.0), s->units);
		s->Ti = pf_pid_cfg_num(c, "Ti", 180.0);
		s->Td = pf_pid_cfg_num(c, "Td", 45.0);
		s->center_factor = pf_pid_cfg_num(c, "center_factor", 0.0010);
		s->stable_window_c = pf_delta_to_c(pf_pid_cfg_num(c, "stable_window", 12), s->units);
		s->tau = pf_pid_cfg_num(c, "tau", 115);
		s->theta = pf_pid_cfg_num(c, "theta", 65);
		gains_pb(s, s->PB);
		break;
	}
	cJSON_Delete(c);
}

static void *create_v(variant_t v, const char *json, const pf_env *env)
{
	pv_t *s = calloc(1, sizeof *s);
	if (!s) return NULL;
	s->v = v;
	s->env = env;
	s->center = 0.5;
	apply_config(s, json);
	return s;
}
static void destroy(void *self) { free(self); }

/* set_target semantics from the originals */
static void reset(void *self, const pf_ctrl_in *in)
{
	pv_t *s = self;
	s->setpoint_c = in->setpoint_c;
	s->err_last = 0;
	s->inter = 0;
	s->derv = 0;
	s->last_t = in->now_s;
	s->last_set_t = in->now_s;
	s->start_change_c = s->have_last ? s->last_pit : in->pit_c;
	s->last_pit = in->pit_c;
	s->have_last = true;
	s->new_target = true;
	if (s->v == V_AC || s->v == V_SP) {
		double sp_f = pf_c_to_f(in->setpoint_c);
		s->center = sp_f <= 240 ? sp_f * s->center_factor : sp_f * s->center_factor * 1.2;
	}
	if (s->v == V_CLAMP_PCT) {
		double sp_user = pf_from_c(in->setpoint_c, s->units);
		gains_pb(s, pf_delta_to_c(sp_user * s->PB / 100.0, s->units));
	}
	/* bumpless where the structure allows it: seed the integrator so u starts near the last duty */
	if (s->ki != 0 && (s->v == V_CLAMP || s->v == V_CLAMP_PCT || s->v == V_PARALLEL)) {
		double e = in->pit_c - in->setpoint_c;
		s->inter = (in->u_prev_applied - s->kp * e) / s->ki;
	}
}

static double update(void *self, const pf_ctrl_in *in, pf_ctrl_dbg *dbg)
{
	pv_t *s = self;
	if (!s->have_last || s->setpoint_c != in->setpoint_c) reset(self, in);
	double now = in->now_s, dt = now - s->last_t;
	if (dt <= 0) dt = in->cycle_time_s > 0 ? in->cycle_time_s : 1;
	double cur = in->pit_c, error = cur - s->setpoint_c;

	if (s->v == V_CLAMP || s->v == V_CLAMP_PCT || s->v == V_PARALLEL) {
		s->p = s->kp * error;
		if (!isfinite(s->inter)) s->inter = 0;
		if (isfinite(error) && isfinite(dt)) s->inter += error * dt;
		s->i = s->ki * s->inter;
		s->derv = (error - s->err_last) / dt;
		s->d = s->kd * s->derv;
		s->u = s->p + s->i + s->d;
		bool clamp = s->v == V_PARALLEL ? s->clamping : true;
		if (clamp && fabs(s->u) >= 1 && s->i * s->u > 0) s->inter -= error * dt; /* anti-windup */
		s->err_last = error;
	} else {
		/* auto-center (+ Smith predictor) */
		double pred = cur, pred_err = error;
		if (s->v == V_SP) {
			/* A plant model with no time constant has no prediction to offer, and dividing by it
			 * puts a NaN through the rest of the loop. Fall back to the reading itself. */
			double roc = dt > 0 ? (cur - s->last_pit) / dt : 0;
			/* How much of one dead time's worth of coasting has already happened. With no time
			 * constant the plant answers at once, so the whole of it has: that is what dividing by
			 * zero used to arrive at by accident, and it is worth arriving at on purpose. */
			double settled = s->tau > 0 ? 1.0 - exp(-dt / s->tau) : 1.0;
			pred = cur + roc * s->theta * settled;
			pred_err = pred - s->setpoint_c;
		}
		if (pred_err < -s->pb_c) s->u = 1.0;
		else if (pred_err > s->stable_window_c) s->u = 0.0;
		else {
			if (s->new_target && fabs(error) <= pf_delta_to_c(3, s->units)) s->new_target = false;
			if (fabs(error) > s->stable_window_c ||
			    (s->new_target && now - s->last_set_t >= in->cycle_time_s * 3 && fabs(error) <= fabs(s->start_change_c - s->setpoint_c) / 2))
				s->inter = 0;
			s->p = s->kp * pred_err + s->center;
			if (!isfinite(s->inter)) s->inter = 0;
			if (isfinite(pred_err) && isfinite(dt)) s->inter += pred_err * dt;
			s->i = fmax(-s->center, fmin(s->center, s->ki * s->inter));
			/* The derivative here brakes the climb, so it must stay live through the large error
			 * that a climb is. A set-point step cannot kick it: reset() re-seeds last_pit and
			 * last_t on the step, so the first sample after one has nothing to differentiate. */
			s->derv = (pred - s->last_pit) / dt;
			if (!isfinite(s->derv)) s->derv = 0;
			s->d = s->kd * s->derv;
			s->u = s->p + s->i + s->d;
		}
	}
	s->last_pit = cur;
	s->last_t = now;
	if (dbg) { dbg->p = s->p; dbg->i = s->i; dbg->d = s->d; dbg->ff = 0; dbg->error = error; dbg->derivative = s->derv; dbg->integral = s->inter; dbg->note[0] = 0; }
	return s->u;
}

static void configure(void *self, const char *json) { pv_t *s = self; apply_config(s, json); s->inter = 0; s->derv = 0; s->have_last = false; }
static int state_json(void *self, char *out, size_t n)
{
	pv_t *s = self;
	return snprintf(out, n, "{\"kp\":%.6g,\"ki\":%.6g,\"kd\":%.6g,\"p\":%.4f,\"i\":%.4f,\"d\":%.4f,\"u\":%.4f,\"inter\":%.3f,\"center\":%.4f}",
	                s->kp, s->ki, s->kd, s->p, s->i, s->d, s->u, s->inter, s->center);
}
static void apply_tuning(void *self, double Ku, double Pu, double K, double tau, double theta)
{
	pv_t *s = self;
	(void)K; (void)Ku; (void)Pu;
	if (s->v == V_SP && tau > 0 && theta > 0) { s->tau = tau; s->theta = theta; }
}

#define OPT(name, fname, desc, dflt, step) "{\"option_name\":\"" name "\",\"option_friendly_name\":\"" fname "\",\"option_description\":\"" desc "\",\"option_type\":\"float\",\"option_default\":" dflt ",\"option_step\":" step "}"
#define OPT_T(name, fname, desc, dflt, step) "{\"option_name\":\"" name "\",\"option_friendly_name\":\"" fname "\",\"option_description\":\"" desc "\",\"option_type\":\"float\",\"option_default\":" dflt ",\"option_step\":" step ",\"units\":\"temp_delta\"}"
#define OPT_B(name, fname, desc, dflt) "{\"option_name\":\"" name "\",\"option_friendly_name\":\"" fname "\",\"option_description\":\"" desc "\",\"option_type\":\"bool\",\"option_default\":" dflt "}"

static const char schema_clamp[] = "[" OPT_T("PB", "Proportional Band (PB)", "Range of the output between 0% and 100%. Increase if you overshoot. [Default 100]", "100.0", "0.1") ","
	OPT("Ti", "Integral Time Constant (Ti)", "Higher Ti reacts less to accumulated error. Lower it if you cannot reach the set point. [Default 180]", "180.0", "0.1") ","
	OPT("Td", "Derivative Time Constant (Td)", "Damper on the response. Start low and increase slowly. [Default 45]", "45.0", "0.1") "]";
static const char schema_clamp_pct[] = "[" OPT("PB", "Proportional Band as % of set point", "Increase if you overshoot. [Default 42]", "42.0", "0.1") ","
	OPT("Ti", "Integral Time Constant (Ti)", "Higher Ti reacts less to accumulated error. [Default 180]", "180.0", "0.1") ","
	OPT("Td", "Derivative Time Constant (Td)", "Damper on the response. [Default 45]", "45.0", "0.1") "]";
static const char schema_ac[] = "[" OPT_T("PB", "Proportional Band (PB)", "Band around the set point where the controller is proportional. [Default 60]", "60.0", "0.1") ","
	OPT("Td", "Derivative Time (Td)", "Seconds to predict the future value. [Default 45]", "45.0", "0.1") ","
	OPT("Ti", "Integral Time (Ti)", "Seconds to eliminate accumulated error. [Default 180]", "180.0", "0.1") ","
	OPT_T("stable_window", "Stable Window", "Window above the set point that triggers the overshoot response (output to minimum). [Default 12]", "12", "1") ","
	OPT("center_factor", "Center Factor", "Multiplied by the set point (in F) to derive the center ratio. [Default 0.0010]", "0.0010", "0.0001") "]";
static const char schema_sp[] = "[" OPT_T("PB", "Proportional Band (PB)", "Band around the set point where the controller is proportional. [Default 60]", "60.0", "0.1") ","
	OPT("Td", "Derivative Time (Td)", "Seconds to predict the future value. [Default 45]", "45.0", "0.1") ","
	OPT("Ti", "Integral Time (Ti)", "Seconds to eliminate accumulated error. [Default 180]", "180.0", "0.1") ","
	OPT_T("stable_window", "Stable Window", "Window above the set point that triggers the overshoot response. [Default 12]", "12", "1") ","
	OPT("center_factor", "Center Factor", "Multiplied by the set point (in F) to derive the center ratio. [Default 0.0010]", "0.0010", "0.0001") ","
	OPT("tau", "Tau (s)", "Time to reach 2/3 of the final value after a set point change. [Default 115]", "115", "1") ","
	OPT("theta", "Theta (s)", "Dead time from a command to the first temperature rise. [Default 65]", "65", "1") "]";
static const char schema_par[] = "[" OPT("Kp", "Proportional Gain", "Kp = 1/PB. [Default 0.01]", "0.01", "0.0001") ","
	OPT("Ki", "Integral Gain", "Ki = Kp/Ti. Increase if you cannot reach the set point. [Default 0.000055]", "0.000055", "0.000001") ","
	OPT("Kd", "Derivative Gain", "Kd = Kp*Td. Start low. [Default 0.45]", "0.45", "0.001") ","
	OPT_B("Clamping", "Integral Windup Protection", "Stop integrating while the output is saturated. [Default on]", "true") "]";

static void *c_clamp(const char *j, const pf_env *e) { return create_v(V_CLAMP, j, e); }
static void *c_clamp_pct(const char *j, const pf_env *e) { return create_v(V_CLAMP_PCT, j, e); }
static void *c_ac(const char *j, const pf_env *e) { return create_v(V_AC, j, e); }
static void *c_sp(const char *j, const pf_env *e) { return create_v(V_SP, j, e); }
static void *c_par(const char *j, const pf_env *e) { return create_v(V_PARALLEL, j, e); }

#define OPS(sym, ident, nm, desc, auth, schema, cyc, umin, umax, ctor) \
	static const pf_controller_ops sym = { .abi = PF_CONTROLLER_ABI, .id = #ident, .name = nm, .description = desc, .author = auth, \
		.config_schema_json = schema, .recommend = { cyc, umin, umax }, .create = ctor, .destroy = destroy, .reset = reset, \
		.update = update, .configure = configure, .state_json = state_json, .apply_tuning = apply_tuning }; \
	const pf_controller_ops *pf_controller_##ident(void) { return &sym; }

OPS(ops_clamp, pid_clamping, "PID w/ Integrator Clamping", "Classic de/dt derivative with integral anti-windup by clamping.", "Mark Alston", schema_clamp, 15, 0.05, 0.9, c_clamp)
OPS(ops_clamp_pct, pid_clamping_percent_pb, "PID w/ Clamping, PB as %", "Integrator clamping with the proportional band defined as a percentage of the set point.", "Mark Alston", schema_clamp_pct, 15, 0.05, 0.9, c_clamp_pct)
OPS(ops_ac, pid_ac, "PID Auto Center", "Standard PID with auto-center calculation and overshoot reduction.", "Dan Borello / Ryan Steel", schema_ac, 15, 0.05, 0.9, c_ac)
OPS(ops_sp, pid_sp, "PID Smith Predictor", "Auto-center PID with a Smith predictor for dead-time compensation.", "Dan Borello / Ryan Steel", schema_sp, 15, 0.05, 0.9, c_sp)
OPS(ops_par, pid_parallel, "Parallel PID", "Parallel-form Kp/Ki/Kd with optional integral windup protection. For tuning-guide users.", "Mark Alston", schema_par, 10, 0.05, 0.99, c_par)
