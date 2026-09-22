#include "core/status.h"
#include "features/tuner.h"
#include "core/settings.h"
#include "core/util.h"
#include "features/weather.h"
#include "net/netmgr.h"
#include "net/tailscale.h"
#include <math.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pf_status g_status;
static unsigned g_gen;
/* The mode on its own, for readers that only want to know whether the grill is doing anything and
 * should not pay for a copy of the whole status to find out. */
static _Atomic int g_mode;

void pf_status_publish(const pf_status *s)
{
	pthread_mutex_lock(&g_mu);
	g_status = *s;
	g_gen++;
	pthread_mutex_unlock(&g_mu);
	__atomic_store_n(&g_mode, (int)s->mode, __ATOMIC_RELAXED);
}

pf_mode pf_status_mode(void) { return (pf_mode)__atomic_load_n(&g_mode, __ATOMIC_RELAXED); }

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
	cJSON_AddNumberToObject(o, "cook_elapsed", s->cook_start_wall > 0 ? round(s->wall - s->cook_start_wall) : 0);
	cJSON_AddNumberToObject(o, "setpoint", r1(conv(s->setpoint_c, units)));
	cJSON_AddBoolToObject(o, "s_plus", s->s_plus);
	cJSON_AddBoolToObject(o, "pwm_control", s->pwm_control);
	cJSON_AddNumberToObject(o, "duty_cycle", s->duty_cycle);
	cJSON_AddBoolToObject(o, "lid_open", s->lid_open);
	cJSON_AddNumberToObject(o, "lid_open_remaining", s->lid_open ? fmax(0, s->lid_open_until - s->t) : 0);
	cJSON_AddBoolToObject(o, "target_reached", s->target_reached);
	/* How long the grill has been working towards what it is aiming at now. A pit short of its
	 * target is ordinary while it climbs and only a fault once it has had time. */
	cJSON_AddNumberToObject(o, "aiming_s", round(s->aim_since > 0 ? fmax(0, s->t - s->aim_since) : 0));
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
	cJSON_AddNumberToObject(cy, "u_ff", round(s->u_ff * 1000) / 1000);
	cJSON *at = cJSON_AddObjectToObject(o, "autotune");
	cJSON_AddBoolToObject(at, "active", s->autotune_active);
	cJSON_AddNumberToObject(at, "crossings", s->autotune_crossings);
	/* A tuning run is more than the oscillation: it lights the grill, waits for it to settle,
	 * measures, moves to the next set point. Anyone looking at the grill while it is doing that
	 * needs to know why it started itself, so the whole run is flagged, not just the measurement. */
	{
		double sp = 0; int step = 0, steps = 0;
		bool on = pf_tuner_active(&sp, &step, &steps);
		cJSON *tn = cJSON_AddObjectToObject(o, "tuning");
		cJSON_AddBoolToObject(tn, "running", on);
		if (on) {
			cJSON_AddNumberToObject(tn, "setpoint", round(sp));
			cJSON_AddNumberToObject(tn, "step", step);
			cJSON_AddNumberToObject(tn, "steps", steps);
			cJSON_AddBoolToObject(tn, "measuring", s->autotune_active);
		}
	}

	cJSON *tm = cJSON_AddObjectToObject(o, "timers");
	cJSON_AddNumberToObject(tm, "startup_duration", s->startup_duration);
	cJSON_AddNumberToObject(tm, "shutdown_duration", s->shutdown_duration);
	cJSON_AddNumberToObject(tm, "prime_duration", s->prime_duration);
	cJSON_AddNumberToObject(tm, "prime_amount", s->prime_amount);
	cJSON_AddNumberToObject(tm, "startup_exit_temp", s->startup_exit_c > 0 ? r1(conv(s->startup_exit_c, units)) : 0);
	/* seconds left in the current timed mode (Startup/Reignite/Shutdown/Prime), 0 otherwise; cold-start may hold Startup past this */
	double left = 0;
	if (s->mode == PF_MODE_STARTUP || s->mode == PF_MODE_REIGNITE) left = s->startup_duration - (s->t - s->mode_start);
	else if (s->mode == PF_MODE_SHUTDOWN) left = s->shutdown_duration - (s->t - s->mode_start);
	else if (s->mode == PF_MODE_PRIME) left = s->prime_duration - (s->t - s->mode_start);
	cJSON_AddNumberToObject(tm, "mode_remaining", round(fmax(0, left)));

	cJSON *cs = cJSON_AddObjectToObject(o, "coldstart");
	cJSON_AddBoolToObject(cs, "active", s->coldstart_active);
	cJSON_AddBoolToObject(cs, "reached", s->coldstart_reached);
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

	cJSON *tmr = cJSON_AddObjectToObject(o, "timer");
	cJSON_AddBoolToObject(tmr, "running", s->timer.running);
	cJSON_AddBoolToObject(tmr, "paused", s->timer.paused);
	cJSON_AddNumberToObject(tmr, "remaining", round(s->timer.remaining));
	cJSON_AddNumberToObject(tmr, "duration", s->timer.duration);
	cJSON_AddNumberToObject(tmr, "after", s->timer.after);

	cJSON *rc = cJSON_AddObjectToObject(o, "recipe");
	cJSON_AddBoolToObject(rc, "active", s->recipe.active);
	if (s->recipe.active) {
		cJSON_AddStringToObject(rc, "name", s->recipe.name);
		cJSON_AddNumberToObject(rc, "step", s->recipe.step);
		cJSON_AddNumberToObject(rc, "nsteps", s->recipe.nsteps);
		cJSON_AddBoolToObject(rc, "waiting", s->recipe.waiting);
		cJSON_AddStringToObject(rc, "step_mode", pf_mode_name(s->recipe.step_mode));
		cJSON_AddNumberToObject(rc, "remaining_s", s->recipe.remaining_s);
		cJSON_AddStringToObject(rc, "message", s->recipe.message);
	}

	{
		pf_weather w;
		pf_weather_get(&w);
		cJSON *wo = cJSON_AddObjectToObject(o, "weather");
		cJSON_AddBoolToObject(wo, "valid", w.valid);
		if (w.valid) {
			cJSON_AddNumberToObject(wo, "temp", r1(conv(w.temp_c, units)));
			cJSON_AddNumberToObject(wo, "wind_kmh", round(w.wind_kmh));
			cJSON_AddNumberToObject(wo, "humidity", round(w.humidity_pct));
			cJSON_AddStringToObject(wo, "place", w.place);
		}
	}
	{
		/* how to reach the grill: the panel builds its QR code from this and the web header
		 * shows the Wi-Fi strength and the Tailscale state without polling anything extra */
		char ip[32], ssid[64], ts[128];
		int signal = 0;
		bool hotspot = false, ts_on = false, ts_up = false;
		pf_netmgr_brief(ip, sizeof ip, ssid, sizeof ssid, &signal, &hotspot);
		pf_tailscale_brief(&ts_on, &ts_up, ts, sizeof ts);
		cJSON *no = cJSON_AddObjectToObject(o, "net");
		cJSON_AddStringToObject(no, "ip", ip);
		cJSON_AddStringToObject(no, "ssid", ssid);
		cJSON_AddNumberToObject(no, "signal", signal);
		cJSON_AddBoolToObject(no, "hotspot", hotspot);
		cJSON_AddNumberToObject(no, "port", pf_set_int("web.port", 80));
		if (ts_on) {
			cJSON *to = cJSON_AddObjectToObject(no, "tailscale");
			cJSON_AddBoolToObject(to, "online", ts_up);
			cJSON_AddStringToObject(to, "name", ts);
		}
	}
	cJSON *probes = cJSON_AddArrayToObject(o, "probes");
	for (int i = 0; i < s->sensors.n; i++) {
		const pf_probe_reading *p = &s->sensors.p[i];
		cJSON *po = cJSON_CreateObject();
		cJSON_AddStringToObject(po, "label", p->label);
		cJSON_AddStringToObject(po, "name", p->name);
		cJSON_AddStringToObject(po, "role", p->role == PF_PROBE_PRIMARY ? "Primary" : p->role == PF_PROBE_AUX ? "Aux" : "Food");
		cJSON_AddBoolToObject(po, "enabled", p->enabled);
		cJSON_AddBoolToObject(po, "in_use", p->in_use);
		cJSON_AddBoolToObject(po, "home", p->home);
		cJSON_AddBoolToObject(po, "valid", p->valid);
		add_num_or_null(po, "temp", p->valid ? r1(conv(p->temp_c, units)) : NAN);
		cJSON_AddNumberToObject(po, "target", p->target_c > 0 ? r1(conv(p->target_c, units)) : 0);
		cJSON_AddNumberToObject(po, "after", s->notify[i].after);
		cJSON_AddNumberToObject(po, "eta_s", s->notify[i].eta_s);
		cJSON_AddNumberToObject(po, "limit_high", s->notify[i].limit_high_c > 0 ? r1(conv(s->notify[i].limit_high_c, units)) : 0);
		cJSON_AddNumberToObject(po, "limit_low", s->notify[i].limit_low_c > 0 ? r1(conv(s->notify[i].limit_low_c, units)) : 0);
		cJSON_AddNumberToObject(po, "ohms", round(p->ohms));
		cJSON_AddBoolToObject(po, "wireless", p->wireless);
		if (p->wireless) {
			cJSON_AddNumberToObject(po, "rssi", p->rssi);
			cJSON_AddNumberToObject(po, "signal", p->valid ? pf_signal_bars(p->rssi) : 0);
			cJSON_AddNumberToObject(po, "battery", p->battery);
		}
		if (p->is_companion) cJSON_AddBoolToObject(po, "companion", true);
		if (p->companion >= 0 && p->companion < s->sensors.n) {
			const pf_probe_reading *a = &s->sensors.p[p->companion];
			add_num_or_null(po, "ambient", a->valid ? r1(conv(a->temp_c, units)) : NAN);
			cJSON_AddStringToObject(po, "ambient_label", a->label);
		}
		cJSON_AddStringToObject(po, "device", p->device);
		cJSON_AddStringToObject(po, "port", p->port);
		cJSON_AddItemToArray(probes, po);
	}
	return o;
}
