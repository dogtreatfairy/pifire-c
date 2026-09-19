/* Adaptive controller: learned feed-forward plus a conservative PID on the error.
 *   u = ff_gain * u_ff(setpoint, ambient)  +  Kp*e + Ki*∫e + Kd*de/dt
 * u_ff comes from the daemon (features/learning.c) via pf_ctrl_in.u_ff; the PID only has to
 * correct what the feed-forward gets wrong, so it can be gentle (large PB, long Ti). Integration is
 * conditional (paused while the output is saturated) and the integrator is seeded for bumpless
 * transfer. When no learning data exists yet the daemon's u_ff falls back to a physics prior. */
#include "controllers/pid_common.h"
#include "pifire/controller.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const pf_env *env;
	pf_units units;
	double PB_c, Ti, Td, ff_gain;
	double kp, ki, kd;
	double inter, last_err, last_t, last_pit;
	double p, i, d, ff, u;
	bool have_last;
	double setpoint_c;
} ad_t;

static const char schema[] =
"[{\"option_name\":\"PB\",\"option_friendly_name\":\"Proportional Band (PB)\",\"option_description\":\"Correction band around the set point. With good feed-forward this can be wide. [Default 80]\",\"option_type\":\"float\",\"option_default\":80.0,\"option_step\":1,\"units\":\"temp_delta\"},"
 "{\"option_name\":\"Ti\",\"option_friendly_name\":\"Integral Time (Ti)\",\"option_description\":\"Seconds to correct residual error. Long values keep the loop calm. [Default 400]\",\"option_type\":\"float\",\"option_default\":400.0,\"option_step\":1},"
 "{\"option_name\":\"Td\",\"option_friendly_name\":\"Derivative Time (Td)\",\"option_description\":\"Damping against fast swings (lid, wind). [Default 30]\",\"option_type\":\"float\",\"option_default\":30.0,\"option_step\":1},"
 "{\"option_name\":\"ff_gain\",\"option_friendly_name\":\"Feed-forward gain\",\"option_description\":\"Scale on the learned steady-state feed (1.0 = trust the model fully). [Default 1.0]\",\"option_type\":\"float\",\"option_default\":1.0,\"option_step\":0.05}]";

static void apply_config(ad_t *s, const char *json)
{
	cJSON *c = json ? cJSON_Parse(json) : NULL;
	s->units = pf_pid_cfg_units(c);
	s->PB_c = pf_delta_to_c(pf_pid_cfg_num(c, "PB", 80.0), s->units);
	s->Ti = pf_pid_cfg_num(c, "Ti", 400.0);
	s->Td = pf_pid_cfg_num(c, "Td", 30.0);
	s->ff_gain = pf_pid_cfg_num(c, "ff_gain", 1.0);
	cJSON_Delete(c);
	s->kp = s->PB_c > 0 ? -1.0 / s->PB_c : 0;
	s->ki = s->Ti > 0 ? s->kp / s->Ti : 0;
	s->kd = s->kp * s->Td;
}

static void *create(const char *json, const pf_env *env)
{
	ad_t *s = calloc(1, sizeof *s);
	s->env = env;
	apply_config(s, json);
	return s;
}
static void destroy(void *self) { free(self); }

static void reset(void *self, const pf_ctrl_in *in)
{
	ad_t *s = self;
	s->setpoint_c = in->setpoint_c;
	s->last_t = in->now_s;
	s->last_pit = in->pit_c;
	s->last_err = in->pit_c - in->setpoint_c;
	s->have_last = true;
	/* bumpless: integrator absorbs the gap between last applied duty and ff + P */
	double ff = s->ff_gain * in->u_ff;
	double p = s->kp * s->last_err;
	s->inter = s->ki != 0 ? (in->u_prev_applied - ff - p) / s->ki : 0;
}

static double update(void *self, const pf_ctrl_in *in, pf_ctrl_dbg *dbg)
{
	ad_t *s = self;
	if (!s->have_last || s->setpoint_c != in->setpoint_c) reset(self, in);
	double dt = in->now_s - s->last_t;
	if (dt <= 0) dt = in->cycle_time_s > 0 ? in->cycle_time_s : 1;
	double e = in->pit_c - in->setpoint_c;
	s->ff = s->ff_gain * in->u_ff;
	s->p = s->kp * e;
	/* conditional integration: don't wind up while saturated in the same direction */
	bool sat_push = (in->saturated > 0 && e < 0) || (in->saturated < 0 && e > 0);
	if (!sat_push) s->inter += e * dt;
	s->i = s->ki * s->inter;
	double lim = 0.5;
	if (s->i > lim) { s->i = lim; s->inter = s->ki != 0 ? lim / s->ki : 0; }
	if (s->i < -lim) { s->i = -lim; s->inter = s->ki != 0 ? -lim / s->ki : 0; }
	double derv = (in->pit_c - s->last_pit) / dt;
	s->d = s->kd * derv;
	s->u = s->ff + s->p + s->i + s->d;
	s->last_t = in->now_s;
	s->last_pit = in->pit_c;
	s->last_err = e;
	if (dbg) { dbg->p = s->p; dbg->i = s->i; dbg->d = s->d; dbg->ff = s->ff; dbg->error = e; dbg->derivative = derv; dbg->integral = s->inter; snprintf(dbg->note, sizeof dbg->note, "ff %.2f", s->ff); }
	return s->u;
}

static void configure(void *self, const char *json) { ad_t *s = self; apply_config(s, json); s->have_last = false; }
static int state_json(void *self, char *out, size_t n)
{
	ad_t *s = self;
	return snprintf(out, n, "{\"kp\":%.6g,\"ki\":%.6g,\"kd\":%.6g,\"ff\":%.4f,\"p\":%.4f,\"i\":%.4f,\"d\":%.4f,\"u\":%.4f}", s->kp, s->ki, s->kd, s->ff, s->p, s->i, s->d, s->u);
}
static void apply_tuning(void *self, double Ku, double Pu, double K, double tau, double theta)
{
	(void)K; (void)tau; (void)theta;
	ad_t *s = self;
	if (Ku <= 0 || Pu <= 0) return;
	/* Tyreus-Luyben, then soften: the feed-forward carries the load */
	double kc = Ku / 3.2;
	s->PB_c = 1.0 / kc * 1.5;
	s->Ti = 2.2 * Pu;
	s->Td = Pu / 6.3;
	s->kp = -1.0 / s->PB_c; s->ki = s->kp / s->Ti; s->kd = s->kp * s->Td;
	s->env->log(PF_LVL_INFO, "adaptive", "tuning applied: PB %.1f C, Ti %.0f s, Td %.0f s", s->PB_c, s->Ti, s->Td);
}

static const pf_controller_ops ops = {
	.abi = PF_CONTROLLER_ABI, .id = "adaptive", .name = "Adaptive (learning feed-forward + PID)",
	.description = "Learns the steady-state feed your grill needs for each set point and ambient temperature across cooks, and uses a gentle PID only to correct the remainder. Improves with every cook; pairs with autotune.",
	.author = "PiFire", .config_schema_json = schema, .recommend = { 20, 0.08, 0.9 },
	.create = create, .destroy = destroy, .reset = reset, .update = update, .configure = configure, .state_json = state_json, .apply_tuning = apply_tuning,
};
const pf_controller_ops *pf_controller_adaptive(void) { return &ops; }
