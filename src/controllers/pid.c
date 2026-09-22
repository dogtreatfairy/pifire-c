/* Standard PiFire/PiSmoker PID on proportional band (port of controller/pid.py).
 *   u = center + kp*e + ki*∫e + kd*de/dt,  kp = -1/PB, ki = kp/Ti, kd = kp*Td
 * PB is configured in the user's units and converted to Celsius here. */
#include "pifire/controller.h"
#include "controllers/pid_common.h"
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const pf_env *env;
	double PB_c, Ti, Td, center;
	double kp, ki, kd;
	double inter, inter_max;
	double last_pit, last_t;
	bool have_last;
	double p, i, d, u;
} pid_t_;

static const char *schema =
"[{\"option_name\":\"PB\",\"option_friendly_name\":\"Proportional Band (PB)\","
 "\"option_description\":\"Temperature band centered on the set point in which the controller is proportional. Higher PB = gentler response. [Default 60]\","
 "\"option_type\":\"float\",\"option_default\":60.0,\"option_step\":0.1,\"units\":\"temp_delta\"},"
 "{\"option_name\":\"Td\",\"option_friendly_name\":\"Derivative Time (Td)\","
 "\"option_description\":\"Seconds to predict the future value. Higher Td reacts more to the rate of change. [Default 45]\","
 "\"option_type\":\"float\",\"option_default\":45.0,\"option_step\":0.1},"
 "{\"option_name\":\"Ti\",\"option_friendly_name\":\"Integral Time (Ti)\","
 "\"option_description\":\"Seconds to eliminate accumulated error. Higher Ti reacts less to accumulated error. [Default 180]\","
 "\"option_type\":\"float\",\"option_default\":180.0,\"option_step\":0.1},"
 "{\"option_name\":\"center\",\"option_friendly_name\":\"Center Ratio\","
 "\"option_description\":\"Feed ratio at zero error. Higher centers respond better at high set points; lower centers are more stable at low set points. [Default 0.5]\","
 "\"option_type\":\"float\",\"option_default\":0.5,\"option_step\":0.01}]";

static void calc_gains(pid_t_ *s)
{
	s->kp = s->PB_c == 0 ? 0 : -1.0 / s->PB_c;
	s->ki = s->Ti == 0 ? 0 : s->kp / s->Ti;
	s->kd = s->kp * s->Td;
	/* How far the integrator may wind. Bounding it by the centre is right when there is one, but a
	 * centre of zero used to leave the bound at zero and the clamp skipped altogether, so the
	 * integrator grew for ever and the loop could not be talked down once it ran away. With no
	 * centre, bound it so the integral term alone cannot exceed a full output. */
	s->inter_max = s->ki != 0 ? fabs((s->center != 0 ? s->center : 1.0) / s->ki) : 0;
}

static void apply_config(pid_t_ *s, const char *json)
{
	cJSON *c = json ? cJSON_Parse(json) : NULL;
	pf_units u = pf_pid_cfg_units(c);
	double pb = pf_pid_cfg_num(c, "PB", 60.0);
	s->PB_c = pf_delta_to_c(pb, u);
	s->Ti = pf_pid_cfg_num(c, "Ti", 180.0);
	s->Td = pf_pid_cfg_num(c, "Td", 45.0);
	s->center = pf_pid_cfg_num(c, "center", 0.5);
	cJSON_Delete(c);
	calc_gains(s);
}

static void *create(const char *config_json, const pf_env *env)
{
	pid_t_ *s = calloc(1, sizeof *s);
	s->env = env;
	apply_config(s, config_json);
	return s;
}

static void destroy(void *self) { free(self); }

static void reset(void *self, const pf_ctrl_in *in)
{
	pid_t_ *s = self;
	s->last_pit = in->pit_c;
	s->last_t = in->now_s;
	s->have_last = true;
	/* bumpless: choose the integrator so p + i == u_prev_applied */
	double e = in->pit_c - in->setpoint_c;
	double p = s->kp * e + s->center;
	double want_i = in->u_prev_applied - p;
	s->inter = s->ki != 0 ? want_i / s->ki : 0;
	if (s->inter_max > 0) s->inter = fmax(-s->inter_max, fmin(s->inter_max, s->inter));
}

static double update(void *self, const pf_ctrl_in *in, pf_ctrl_dbg *dbg)
{
	pid_t_ *s = self;
	if (!s->have_last) reset(self, in);
	double dt = in->now_s - s->last_t;
	if (dt <= 0) dt = in->cycle_time_s > 0 ? in->cycle_time_s : 1;

	double e = in->pit_c - in->setpoint_c;
	s->p = s->kp * e + s->center;

	/* A probe that drops out for a moment hands the controller a reading that is not a number. One
	 * addition of it to the integrator poisons the integrator for ever, because every later
	 * comparison against it is false and nothing clears it, so the loop never recovers even after
	 * the probe comes back. Integrate only real numbers, and throw away an accumulator that has
	 * already gone bad. */
	if (!isfinite(s->inter)) s->inter = 0;
	if (isfinite(e) && isfinite(dt)) s->inter += e * dt;
	if (s->center != 0) s->inter = fmax(-s->inter_max, fmin(s->inter_max, s->inter));
	s->i = s->ki * s->inter;

	double derv = (in->pit_c - s->last_pit) / dt;
	s->d = s->kd * derv;

	s->u = s->p + s->i + s->d;
	s->last_pit = in->pit_c;
	s->last_t = in->now_s;

	if (dbg) {
		dbg->p = s->p; dbg->i = s->i; dbg->d = s->d; dbg->ff = 0;
		dbg->error = e; dbg->derivative = derv; dbg->integral = s->inter;
		dbg->note[0] = 0;
	}
	return s->u;
}

static void configure(void *self, const char *config_json)
{
	pid_t_ *s = self;
	apply_config(s, config_json);
	s->inter = 0;
	s->have_last = false;
}

static int state_json(void *self, char *out, size_t n)
{
	pid_t_ *s = self;
	return snprintf(out, n,
		"{\"kp\":%.6g,\"ki\":%.6g,\"kd\":%.6g,\"p\":%.4f,\"i\":%.4f,\"d\":%.4f,\"u\":%.4f,\"inter\":%.3f,\"inter_max\":%.3f}",
		s->kp, s->ki, s->kd, s->p, s->i, s->d, s->u, s->inter, s->inter_max);
}

static const pf_controller_ops ops = {
	.abi = PF_CONTROLLER_ABI,
	.id = "pid",
	.name = "PID Standard",
	.description = "The standard PiFire PID (from DBorello's PiSmoker). Proportional band form with integrator clamp.",
	.author = "Dan Borello",
	.config_schema_json = NULL, /* set at registration */
	.recommend = { 25, 0.1, 0.9 },
	.create = create, .destroy = destroy, .reset = reset, .update = update,
	.configure = configure, .state_json = state_json,
};

const pf_controller_ops *pf_controller_pid(void)
{
	static pf_controller_ops o;
	o = ops;
	o.config_schema_json = schema;
	return &o;
}
