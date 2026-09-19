#include "core/status.h"
#include "core/util.h"
#include <math.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pf_status g_status;
static unsigned g_gen;

void pf_status_publish(const pf_status *s)
{
	pthread_mutex_lock(&g_mu);
	g_status = *s;
	g_gen++;
	pthread_mutex_unlock(&g_mu);
}

void pf_status_get(pf_status *out)
{
	pthread_mutex_lock(&g_mu);
	*out = g_status;
	pthread_mutex_unlock(&g_mu);
}

unsigned pf_status_generation(void) { return __atomic_load_n(&g_gen, __ATOMIC_RELAXED); }

static double conv(double c, pf_units u) { return isnan(c) ? c : pf_from_c(c, u); }
static double r1(double v) { return isnan(v) ? v : round(v * 10.0) / 10.0; }

static void add_num_or_null(cJSON *o, const char *key, double v)
{
	if (isnan(v)) cJSON_AddNullToObject(o, key);
	else cJSON_AddNumberToObject(o, key, v);
}

cJSON *pf_status_to_json(const pf_status *s, pf_units units)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "ts", s->wall);
	cJSON_AddStringToObject(o, "units", units == PF_UNITS_C ? "C" : "F");
	cJSON_AddStringToObject(o, "mode", pf_mode_name(s->mode));
	cJSON_AddStringToObject(o, "next_mode", pf_mode_name(s->next_mode));
	cJSON_AddNumberToObject(o, "mode_elapsed", s->t - s->mode_start);
	cJSON_AddNumberToObject(o, "setpoint", r1(conv(s->setpoint_c, units)));
	cJSON_AddBoolToObject(o, "s_plus", s->s_plus);
	cJSON_AddBoolToObject(o, "pwm_control", s->pwm_control);
	cJSON_AddNumberToObject(o, "duty_cycle", s->duty_cycle);
	cJSON_AddBoolToObject(o, "lid_open", s->lid_open);
	cJSON_AddNumberToObject(o, "lid_open_remaining", s->lid_open ? fmax(0, s->lid_open_until - s->t) : 0);
	cJSON_AddBoolToObject(o, "target_reached", s->target_reached);
	cJSON_AddBoolToObject(o, "sim", s->sim);
	cJSON_AddNumberToObject(o, "hopper_pct", s->hopper_pct);
	add_num_or_null(o, "ambient", r1(conv(s->ambient_c, units)));

	cJSON *out = cJSON_AddObjectToObject(o, "outputs");
	for (int i = 0; i < PF_OUT_COUNT; i++) cJSON_AddBoolToObject(out, pf_output_name((pf_output)i), (s->outputs >> i) & 1);
	cJSON_AddNumberToObject(out, "fan_pct", s->fan_pct);

	cJSON *cy = cJSON_AddObjectToObject(o, "cycle");
	cJSON_AddNumberToObject(cy, "u_raw", round(s->u_raw * 1000) / 1000);
	cJSON_AddNumberToObject(cy, "u_applied", round(s->u_applied * 1000) / 1000);
	cJSON_AddNumberToObject(cy, "saturated", s->saturated);
	cJSON_AddNumberToObject(cy, "cycle_s", s->cycle_s);

	cJSON *tm = cJSON_AddObjectToObject(o, "timers");
	cJSON_AddNumberToObject(tm, "startup_duration", s->startup_duration);
	cJSON_AddNumberToObject(tm, "shutdown_duration", s->shutdown_duration);
	cJSON_AddNumberToObject(tm, "prime_duration", s->prime_duration);
	cJSON_AddNumberToObject(tm, "prime_amount", s->prime_amount);

	cJSON *cs = cJSON_AddObjectToObject(o, "coldstart");
	cJSON_AddBoolToObject(cs, "active", s->coldstart_active);
	add_num_or_null(cs, "baseline", s->coldstart_active ? r1(conv(s->coldstart_baseline_c, units)) : NAN);
	cJSON_AddNumberToObject(cs, "remaining", s->coldstart_active ? fmax(0, s->coldstart_deadline - s->t) : 0);

	cJSON *sf = cJSON_AddObjectToObject(o, "safety");
	cJSON_AddStringToObject(sf, "error_code", s->error_code);
	cJSON_AddStringToObject(sf, "error_msg", s->error_msg);
	cJSON_AddNumberToObject(sf, "reignite_retries_left", s->reignite_retries_left);

	cJSON *ct = cJSON_AddObjectToObject(o, "controller");
	cJSON_AddStringToObject(ct, "id", s->controller_id);
	cJSON_AddNumberToObject(ct, "p", s->ctrl_dbg.p);
	cJSON_AddNumberToObject(ct, "i", s->ctrl_dbg.i);
	cJSON_AddNumberToObject(ct, "d", s->ctrl_dbg.d);
	cJSON_AddNumberToObject(ct, "ff", s->ctrl_dbg.ff);
	cJSON_AddNumberToObject(ct, "error", r1(pf_delta_from_c(s->ctrl_dbg.error, units)));
	cJSON_AddStringToObject(ct, "note", s->ctrl_dbg.note);

	cJSON *probes = cJSON_AddArrayToObject(o, "probes");
	for (int i = 0; i < s->sensors.n; i++) {
		const pf_probe_reading *p = &s->sensors.p[i];
		cJSON *po = cJSON_CreateObject();
		cJSON_AddStringToObject(po, "label", p->label);
		cJSON_AddStringToObject(po, "name", p->name);
		cJSON_AddStringToObject(po, "role", p->role == PF_PROBE_PRIMARY ? "Primary" : p->role == PF_PROBE_AUX ? "Aux" : "Food");
		cJSON_AddBoolToObject(po, "enabled", p->enabled);
		cJSON_AddBoolToObject(po, "valid", p->valid);
		add_num_or_null(po, "temp", p->valid ? r1(conv(p->temp_c, units)) : NAN);
		cJSON_AddNumberToObject(po, "target", p->target_c > 0 ? r1(conv(p->target_c, units)) : 0);
		cJSON_AddNumberToObject(po, "ohms", round(p->ohms));
		cJSON_AddStringToObject(po, "device", p->device);
		cJSON_AddStringToObject(po, "port", p->port);
		cJSON_AddItemToArray(probes, po);
	}
	return o;
}
