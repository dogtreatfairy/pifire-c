#include "core/status.h"
#include "features/tuner.h"
#include "features/alarms.h"
#include "features/update.h"
#include "features/backup.h"
#include "core/settings.h"
#include "core/util.h"
#include "features/learning.h"
#include "features/weather.h"
#include "net/netmgr.h"
#include "net/tailscale.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
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
	cJSON_AddBoolToObject(o, "lid_event", s->lid_event);
	{
		/* a restart nobody asked for from the app, for ten minutes: what the "Grill Restarted" rule reads */
		cJSON *r = cJSON_AddObjectToObject(o, "restarted");
		bool recent = s->restart_wall > 0 && pf_wall() - s->restart_wall < 600;
		cJSON_AddBoolToObject(r, "active", recent);
		cJSON_AddStringToObject(r, "reason", recent ? s->restart_reason : "");
		cJSON_AddStringToObject(r, "resuming", recent && s->restart_resumed ? " · Cook resumed" : "");
	}
	cJSON_AddNumberToObject(o, "lid_open_remaining", s->lid_open ? fmax(0, s->lid_open_until - s->t) : 0);
	cJSON_AddBoolToObject(o, "target_reached", s->target_reached);
	/* How long the grill has been working towards what it is aiming at now. A pit short of its
	 * target is ordinary while it climbs and only a fault once it has had time. */
	cJSON_AddNumberToObject(o, "aiming_s", round(s->aim_since > 0 ? fmax(0, s->t - s->aim_since) : 0));
	/* How long the pit should take to reach the set point it has just been given. It is a readout,
	 * not an alert: the question is asked the moment the set point changes, before there is any
	 * climb to measure, which is why it comes from what the grill has learned rather than from a
	 * line fitted through the last few minutes. -1 while it does not know enough to say. */
	cJSON_AddNumberToObject(o, "setpoint_eta_s", s->setpoint_eta_s >= 0 ? round(s->setpoint_eta_s) : -1);
	{
		char word[64] = "";
		pf_alarms_flash_word(word, sizeof word);
		/* A probe's flash is filled in now, with what it reads now: the action on the first line and
		 * the probe with its reading on the second -- "Flip" over "Brisket 165°". */
		if (word[0] == '\x01') {
			char label[24] = "", name[24] = "", action[24] = "";
			const char *a = word + 1, *b = strchr(a, '\x01');
			const char *c2 = b ? strchr(b + 1, '\x01') : NULL;
			if (b) {
				size_t ll = (size_t)(b - a); if (ll >= sizeof label) ll = sizeof label - 1;
				memcpy(label, a, ll);
				size_t nl = c2 ? (size_t)(c2 - b - 1) : strlen(b + 1); if (nl >= sizeof name) nl = sizeof name - 1;
				memcpy(name, b + 1, nl);
				if (c2) pf_strlcpy(action, c2 + 1, sizeof action);
			}
			double t = NAN;
			for (int i = 0; i < s->sensors.n; i++)
				if (!strcmp(s->sensors.p[i].label, label) && s->sensors.p[i].valid) t = conv(s->sensors.p[i].temp_c, units);
			char line[40];
			if (isfinite(t)) snprintf(line, sizeof line, "%.14s %.0f\xC2\xB0", name, t);
			else snprintf(line, sizeof line, "%.20s", name);
			if (action[0]) snprintf(word, sizeof word, "%s\n%s", action, line);
			else pf_strlcpy(word, line, sizeof word);
		}
		cJSON_AddStringToObject(o, "attention", word);   /* what the panel flashes until it is acknowledged */
	}
	cJSON_AddBoolToObject(o, "sim", s->sim);
	cJSON_AddStringToObject(o, "version", PF_VERSION);
	{
		/* the updater's stage, so any page can say Downloading, Updating, Rebooting */
		char st[16] = "idle"; double pr = 0;
		pf_update_stage(st, sizeof st, &pr);
		cJSON *u = cJSON_AddObjectToObject(o, "update");
		cJSON_AddStringToObject(u, "state", st);
		cJSON_AddNumberToObject(u, "progress", pr);
		/* what the notification rules ask about: a PiFire build waiting, and system packages */
		bool avail = false; char latest[32] = ""; int nsys = 0;
		pf_update_summary(&avail, latest, sizeof latest, &nsys);
		cJSON_AddBoolToObject(u, "available", avail);
		cJSON_AddStringToObject(u, "latest", latest[0] == 'v' ? latest + 1 : latest);
		cJSON_AddNumberToObject(u, "system", nsys);
	}
	{
		/* the last backup, for the rules: one that just finished, or a failure still standing */
		bool recent = false, failed = false;
		pf_backup_brief(&recent, &failed);
		cJSON *b = cJSON_AddObjectToObject(o, "backup");
		cJSON_AddBoolToObject(b, "done", recent);
		cJSON_AddBoolToObject(b, "failed", failed);
	}
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
			char what[12] = ""; double eta_s = -1; int cr = 0;
			if (pf_tuner_live(what, sizeof what, &eta_s, &cr)) {
				cJSON *e = cJSON_AddObjectToObject(tn, "eta");
				cJSON_AddStringToObject(e, "what", what);
				cJSON_AddNumberToObject(e, "s", eta_s >= 0 ? round(eta_s) : -1);
				cJSON_AddNumberToObject(tn, "crossings", cr);
			}
		}
	}

	cJSON *tm = cJSON_AddObjectToObject(o, "timers");
	cJSON_AddNumberToObject(tm, "shutdown_duration", s->shutdown_duration);
	cJSON_AddNumberToObject(tm, "prime_duration", s->prime_duration);
	cJSON_AddNumberToObject(tm, "prime_amount", s->prime_amount);
	/* seconds left: in Startup and Relight, of the Smart Start window running (the proof, then the
	 * exit rise); in Shutdown and Prime, of the mode; 0 otherwise */
	double left = 0;
	if (s->mode == PF_MODE_STARTUP || s->mode == PF_MODE_REIGNITE) left = s->ss_active ? s->ss_deadline - s->t : 0;
	else if (s->mode == PF_MODE_SHUTDOWN) left = s->shutdown_duration - (s->t - s->mode_start);
	else if (s->mode == PF_MODE_PRIME) left = s->prime_duration - (s->t - s->mode_start);
	cJSON_AddNumberToObject(tm, "mode_remaining", round(fmax(0, left)));

	cJSON *cs = cJSON_AddObjectToObject(o, "smartstart");
	cJSON_AddBoolToObject(cs, "active", s->ss_active);
	cJSON_AddBoolToObject(cs, "proven", s->ss_proven);
	add_num_or_null(cs, "baseline", s->ss_active ? r1(conv(s->ss_baseline_c, units)) : NAN);
	cJSON_AddNumberToObject(cs, "remaining", s->ss_active ? round(fmax(0, s->ss_deadline - s->t)) : 0);
	/* after startup, until the pit reaches its working temperature: what the "Heating" rule says */
	cJSON *ht = cJSON_AddObjectToObject(o, "heating");
	cJSON_AddBoolToObject(ht, "active", s->heating);
	char htxt[48] = "Heating";
	if (s->heating && s->mode == PF_MODE_HOLD && s->setpoint_c > 0)
		snprintf(htxt, sizeof htxt, "Heating to %.0f°%s", conv(s->setpoint_c, units), units == PF_UNITS_C ? "C" : "F");
	cJSON_AddStringToObject(ht, "text", htxt);

	cJSON *sf = cJSON_AddObjectToObject(o, "safety");
	cJSON_AddStringToObject(sf, "error_code", s->error_code);
	cJSON_AddStringToObject(sf, "error_msg", s->error_msg);
	cJSON_AddNumberToObject(sf, "reignite_retries_left", s->reignite_retries_left);
	cJSON_AddBoolToObject(sf, "proving", s->proving);

	cJSON *ct = cJSON_AddObjectToObject(o, "controller");
	cJSON_AddStringToObject(ct, "id", s->controller_id);
	cJSON_AddNumberToObject(ct, "p", s->ctrl_dbg.p);
	cJSON_AddNumberToObject(ct, "i", s->ctrl_dbg.i);
	cJSON_AddNumberToObject(ct, "d", s->ctrl_dbg.d);
	cJSON_AddNumberToObject(ct, "ff", s->ctrl_dbg.ff);
	cJSON_AddNumberToObject(ct, "error", r1(pf_delta_from_c(s->ctrl_dbg.error, units)));
	cJSON_AddStringToObject(ct, "note", s->ctrl_dbg.note);
	/* What is in force for the set point being asked for, whether or not the grill is holding. */
	if (s->tuning.valid) {
		cJSON *tn = cJSON_AddObjectToObject(ct, "tuning");
		cJSON_AddNumberToObject(tn, "PB", r1(pf_delta_from_c(s->tuning.PB_c, units)));
		cJSON_AddNumberToObject(tn, "Ti", round(s->tuning.Ti));
		cJSON_AddNumberToObject(tn, "Td", round(s->tuning.Td));
		cJSON_AddStringToObject(tn, "src", s->tuning.src);
	}

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
		/* the step as the cook counts them: lighting and shutting down are the ends of the
		 * timeline, not stages of it */
		cJSON_AddNumberToObject(rc, "stage", s->recipe.stage);
		cJSON_AddNumberToObject(rc, "stages", s->recipe.stages);
		cJSON_AddBoolToObject(rc, "waiting", s->recipe.waiting);
		cJSON_AddBoolToObject(rc, "needs_lid", s->recipe.needs_lid);
		cJSON_AddStringToObject(rc, "step_mode", pf_mode_name(s->recipe.step_mode));
		cJSON_AddNumberToObject(rc, "remaining_s", s->recipe.remaining_s);
		cJSON_AddNumberToObject(rc, "clock_s", s->recipe.clock_s);
		cJSON_AddBoolToObject(rc, "at_temp", s->recipe.at_temp);
		cJSON_AddNumberToObject(rc, "id", s->recipe.id);
		{
			cJSON *fl = cJSON_AddArrayToObject(rc, "flags");
			for (int i = 0; i < s->recipe.nsteps && i < 16; i++)
				cJSON_AddItemToArray(fl, cJSON_CreateString(s->recipe.flags[i] == 1 ? "hold" : s->recipe.flags[i] == 2 ? "skip" : s->recipe.flags[i] == 3 ? "auto" : ""));
		}
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
		if (s->notify[i].meat[0]) cJSON_AddStringToObject(po, "meat", s->notify[i].meat);
		if (s->notify[i].done[0]) cJSON_AddStringToObject(po, "done", s->notify[i].done);
		if (s->notify[i].finish_c > 0) cJSON_AddNumberToObject(po, "finish", r1(conv(s->notify[i].finish_c, units)));
		if (s->notify[i].rest_c > 0) cJSON_AddNumberToObject(po, "rest", r1(conv(s->notify[i].rest_c, units)));
		if (s->notify[i].resting) { cJSON_AddBoolToObject(po, "resting", true); cJSON_AddNumberToObject(po, "rest_peak", r1(conv(s->notify[i].rest_peak_c, units))); }
		cJSON_AddNumberToObject(po, "eta_s", s->notify[i].eta_s);
		cJSON_AddNumberToObject(po, "eta_step_s", s->notify[i].eta_step_s);
		if (s->notify[i].next_step[0]) cJSON_AddStringToObject(po, "next_step", s->notify[i].next_step);
		/* the steps, with what each has already said, so the app can tick them off */
		if (s->notify[i].nsteps > 0) {
			cJSON *steps = cJSON_AddArrayToObject(po, "steps");
			for (int k = 0; k < s->notify[i].nsteps; k++) {
				cJSON *st = cJSON_CreateObject();
				cJSON_AddStringToObject(st, "name", s->notify[i].steps[k].name);
				cJSON_AddNumberToObject(st, "temp", r1(conv(s->notify[i].steps[k].temp_c, units)));
				cJSON_AddBoolToObject(st, "done", s->notify[i].steps[k].fired);
				cJSON_AddItemToArray(steps, st);
			}
		}
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
