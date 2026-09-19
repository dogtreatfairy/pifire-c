#include "platform/sim.h"
#include "core/log.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "sim"

/* Model constants (rough numbers for a mid-size pellet grill) */
#define AUGER_GPS        0.30    /* grams/s while auger runs */
#define BURN_MAX_GPS     0.45    /* firepot can burn at most this fast */
#define POT_TAU_S        60.0    /* pellets in the pot burn down with this time constant */
#define GAIN_C_PER_GPS   680.0   /* steady ΔT above ambient per g/s burned, still air */
#define PIT_TAU_S        240.0   /* first-order pit response (thermal mass of the barrel) */
#define DEAD_TIME_S      45.0
#define IGNITE_AFTER_S   60.0    /* igniter needs this long with pellets to light */
#define STARVE_OUT_S     45.0    /* empty pot this long -> fire out */
#define IGNITER_HEAT_C   6.0
#define LID_LOSS_FACTOR  0.55
#define FOOD_TAU_S       2400.0

static pf_sim_state *g_model;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

pf_sim_state *pf_sim_model(void) { return g_model; }

void pf_sim_reset(double ambient_c)
{
	pthread_mutex_lock(&g_mu);
	pf_sim_state *m = g_model;
	memset(m, 0, sizeof *m);
	m->ambient_c = ambient_c;
	m->time_scale = 1;
	m->pit_c = ambient_c;
	for (int i = 0; i < 3; i++) m->food_c[i] = ambient_c;
	for (int i = 0; i < 8; i++) m->delay[i] = ambient_c;
	pthread_mutex_unlock(&g_mu);
}

void pf_sim_step(double dt)
{
	if (!g_model || dt <= 0) return;
	pthread_mutex_lock(&g_mu);
	pf_sim_state *m = g_model;
	m->sim_time_s += dt;

	/* pellets in / out of the pot */
	if (m->out[PF_OUT_AUGER]) m->pot_pellets_g += AUGER_GPS * dt;
	double burn = 0;
	if (m->fire_lit) {
		double air = m->out[PF_OUT_FAN] ? (0.4 + 0.6 * (m->fan_pct > 0 ? m->fan_pct : 100) / 100.0) : 0.25;
		burn = fmin(BURN_MAX_GPS * air, m->pot_pellets_g / POT_TAU_S);
		m->pot_pellets_g = fmax(0, m->pot_pellets_g - burn * dt);
	} else {
		/* unlit pellets just accumulate (cap so a runaway auger doesn't grow unbounded) */
		if (m->pot_pellets_g > 400) m->pot_pellets_g = 400;
	}

	/* ignition and flame-out */
	if (m->out[PF_OUT_IGNITER] && m->pot_pellets_g > 2) m->igniter_time_s += dt; else m->igniter_time_s = 0;
	if (!m->fire_lit && m->igniter_time_s >= IGNITE_AFTER_S) { m->fire_lit = true; LOGD(TAG, "fire lit at t=%.0f", m->sim_time_s); }
	if (m->fire_lit && m->pot_pellets_g < 0.5) m->starved_time_s += dt; else m->starved_time_s = 0;
	if (m->fire_lit && m->starved_time_s > STARVE_OUT_S && !m->out[PF_OUT_IGNITER]) { m->fire_lit = false; LOGD(TAG, "fire out at t=%.0f", m->sim_time_s); }

	/* equilibrium pit temperature for this instant */
	double gain = GAIN_C_PER_GPS * (1.0 - 0.35 * m->wind) * (m->lid_open ? LID_LOSS_FACTOR : 1.0);
	double t_eq = m->ambient_c + gain * burn + (m->out[PF_OUT_IGNITER] ? IGNITER_HEAT_C : 0);

	/* dead time: push t_eq through a short delay line, then first-order lag */
	double slot = DEAD_TIME_S / 8.0;
	static double acc;
	acc += dt;
	while (acc >= slot) {
		memmove(&m->delay[1], &m->delay[0], sizeof(double) * 7);
		m->delay[0] = t_eq;
		acc -= slot;
	}
	double target = m->delay[7];
	double tau = m->lid_open ? PIT_TAU_S * 0.5 : PIT_TAU_S;
	m->pit_c += (target - m->pit_c) * (dt / tau);

	for (int i = 0; i < 3; i++) {
		double ftau = FOOD_TAU_S * (1.0 + 0.5 * i);
		m->food_c[i] += (m->pit_c - m->food_c[i]) * (dt / ftau);
	}
	pthread_mutex_unlock(&g_mu);
}

/* ---- platform ops ---- */

static void *create(const char *platform_json, const pf_env *env)
{
	(void)platform_json; (void)env;
	if (!g_model) g_model = calloc(1, sizeof *g_model);
	pf_sim_reset(18.0);
	LOGI(TAG, "simulated platform ready (ambient %.1f C)", g_model->ambient_c);
	return g_model;
}

static void destroy(void *self) { (void)self; }

static int set_output(void *self, pf_output o, bool on)
{
	pf_sim_state *m = self;
	pthread_mutex_lock(&g_mu);
	m->out[o] = on;
	if (o == PF_OUT_FAN && !on) m->fan_pct = 0;
	if (o == PF_OUT_FAN && on && m->fan_pct == 0) m->fan_pct = 100;
	pthread_mutex_unlock(&g_mu);
	return 0;
}

static int set_fan_pct(void *self, int pct)
{
	pf_sim_state *m = self;
	pthread_mutex_lock(&g_mu);
	m->fan_pct = pct;
	pthread_mutex_unlock(&g_mu);
	return 0;
}

static int set_pwm_frequency(void *self, int hz) { (void)self; (void)hz; return 0; }
static bool read_input(void *self, pf_input in) { (void)self; (void)in; return false; }

static void all_off(void *self)
{
	pf_sim_state *m = self;
	pthread_mutex_lock(&g_mu);
	memset(m->out, 0, sizeof m->out);
	m->fan_pct = 0;
	pthread_mutex_unlock(&g_mu);
}

static int status_json(void *self, char *out, size_t n)
{
	pf_sim_state *m = self;
	return snprintf(out, n, "{\"sim\":true,\"pit_c\":%.2f,\"ambient_c\":%.1f,\"pot_g\":%.1f,\"fire_lit\":%s,\"lid_open\":%s}",
	                m->pit_c, m->ambient_c, m->pot_pellets_g, m->fire_lit ? "true" : "false", m->lid_open ? "true" : "false");
}

static const pf_platform_ops ops = {
	.abi = PF_PLATFORM_ABI, .id = "sim", .name = "Simulator",
	.create = create, .destroy = destroy, .set_output = set_output, .set_fan_pct = set_fan_pct,
	.set_pwm_frequency = set_pwm_frequency, .read_input = read_input, .all_off = all_off, .status_json = status_json,
};

const pf_platform_ops *pf_platform_sim(void) { return &ops; }
