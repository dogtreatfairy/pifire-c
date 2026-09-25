#include "core/notify.h"
#include "core/events.h"
#include "core/settings.h"
#include "core/log.h"
#include "core/util.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define ETA_INTERVAL_S 3.0
#define ETA_RECALC_S   20.0

void pf_notify_init(pf_notify *n)
{
	memset(n, 0, sizeof *n);
	for (int i = 0; i < PF_MAX_PROBES; i++) { n->probes[i].eta_s = -1; n->probes[i].eta_step_s = -1; }
}

void pf_notify_sync(pf_notify *n, const pf_sensors *s)
{
	pf_notify_probe fresh[PF_MAX_PROBES];
	int k = 0;
	for (int i = 0; i < s->n && k < PF_MAX_PROBES; i++) {
		const pf_notify_probe *old = pf_notify_find(n, s->p[i].label);
		if (old) fresh[k] = *old;
		else { memset(&fresh[k], 0, sizeof fresh[k]); fresh[k].eta_s = -1; fresh[k].eta_step_s = -1; strcpy(fresh[k].label, s->p[i].label); }
		k++;
	}
	memcpy(n->probes, fresh, sizeof(pf_notify_probe) * (size_t)k);
	n->n = k;
}

const pf_notify_probe *pf_notify_find(const pf_notify *n, const char *label)
{
	for (int i = 0; i < n->n; i++) if (!strcmp(n->probes[i].label, label)) return &n->probes[i];
	return NULL;
}

static pf_notify_probe *find_mut(pf_notify *n, const char *label)
{
	for (int i = 0; i < n->n; i++) if (!strcmp(n->probes[i].label, label)) return &n->probes[i];
	return NULL;
}

int pf_notify_set_target(pf_notify *n, const char *label, double target_c, int after)
{
	pf_notify_probe *p = find_mut(n, label);
	if (!p) return -1;
	p->reached = false;
	p->eta_warned = false;
	p->eta_hits = 0;
	p->target_c = target_c > 0 ? target_c : 0;
	p->after = after;
	p->eta_s = -1;
	p->eta_step_s = -1;
	return 0;
}

int pf_notify_set_limits(pf_notify *n, const char *label, double high_c, double low_c)
{
	pf_notify_probe *p = find_mut(n, label);
	if (!p) return -1;
	p->limit_high_c = high_c > 0 ? high_c : 0;
	p->limit_low_c = low_c > 0 ? low_c : 0;
	p->high_tripped = p->low_tripped = false;
	return 0;
}

void pf_notify_timer_start(pf_notify *n, double seconds, int after, double now)
{
	n->timer.running = true;
	n->timer.paused = false;
	n->timer.duration = seconds;
	n->timer.end_t = now + seconds;
	n->timer.after = after;
}
void pf_notify_timer_pause(pf_notify *n, double now)
{
	if (!n->timer.running || n->timer.paused) return;
	n->timer.paused = true;
	n->timer.remaining = n->timer.end_t - now;
}
void pf_notify_timer_resume(pf_notify *n, double now)
{
	if (!n->timer.running || !n->timer.paused) return;
	n->timer.paused = false;
	n->timer.end_t = now + n->timer.remaining;
}
void pf_notify_timer_cancel(pf_notify *n) { memset(&n->timer, 0, sizeof n->timer); }

/* ---- ETA (port of notifications.py _estimate_eta) ---- */

double pf_notify_estimate_eta(const double *temps, int n, double target, double interval_s)
{
	if (n < 20) return -1;
	double maxv = temps[0];
	for (int i = 1; i < n; i++) if (temps[i] > maxv) maxv = temps[i];
	if (target <= maxv) return -1;
	/* centered moving average, window 5, edges unchanged */
	double sm[PF_ETA_SAMPLES];
	for (int i = 0; i < n; i++) {
		if (i < 2 || i >= n - 2) { sm[i] = temps[i]; continue; }
		sm[i] = (temps[i - 2] + temps[i - 1] + temps[i] + temps[i + 1] + temps[i + 2]) / 5.0;
	}
	double wsum = 0, xbar = 0, ybar = 0;
	double w[PF_ETA_SAMPLES];
	for (int i = 0; i < n; i++) { w[i] = exp((double)i / 10.0); wsum += w[i]; }
	for (int i = 0; i < n; i++) { w[i] /= wsum; xbar += w[i] * i; ybar += w[i] * sm[i]; }
	double num = 0, den = 0;
	for (int i = 0; i < n; i++) { num += w[i] * (i - xbar) * (sm[i] - ybar); den += w[i] * (i - xbar) * (i - xbar); }
	if (den == 0) return -1;
	double slope = num / den;
	if (slope <= 0) return -1;
	double eta = (target - sm[n - 1]) / slope * interval_s;
	return (eta < 0 || isinf(eta) || isnan(eta)) ? -1 : eta;
}

/* How fast a probe is climbing, in C per second, from the same recency-weighted fit the ETA uses.
 *
 * This is what carryover is worked out from: meat coasts on the heat already inside it, and the
 * rate it is climbing at when it comes off is the measure of how much of that there is. A probe
 * that is not climbing coasts nowhere, which is why a stall returns zero rather than an error. */
double pf_notify_probe_rate(const pf_notify *n, const char *label)
{
	const pf_notify_probe *p = pf_notify_find(n, label);
	if (!p || p->hist_len < 20) return 0;
	double lin[PF_ETA_SAMPLES];
	int cnt = p->hist_len;
	for (int i = 0; i < cnt; i++) lin[i] = p->hist[(p->hist_head - cnt + i + PF_ETA_SAMPLES) % PF_ETA_SAMPLES];
	double sm[PF_ETA_SAMPLES];
	for (int i = 0; i < cnt; i++) {
		if (i < 2 || i >= cnt - 2) { sm[i] = lin[i]; continue; }
		sm[i] = (lin[i - 2] + lin[i - 1] + lin[i] + lin[i + 1] + lin[i + 2]) / 5.0;
	}
	double w[PF_ETA_SAMPLES], wsum = 0, xbar = 0, ybar = 0;
	for (int i = 0; i < cnt; i++) { w[i] = exp((double)i / 10.0); wsum += w[i]; }
	for (int i = 0; i < cnt; i++) { w[i] /= wsum; xbar += w[i] * i; ybar += w[i] * sm[i]; }
	double num = 0, den = 0;
	for (int i = 0; i < cnt; i++) { num += w[i] * (i - xbar) * (sm[i] - ybar); den += w[i] * (i - xbar) * (i - xbar); }
	if (den <= 0) return 0;
	double slope = num / den / ETA_INTERVAL_S;
	return slope > 0 && !isnan(slope) && !isinf(slope) ? slope : 0;
}

static void push_sample(pf_notify_probe *p, double c)
{
	p->hist[p->hist_head] = c;
	p->hist_head = (p->hist_head + 1) % PF_ETA_SAMPLES;
	if (p->hist_len < PF_ETA_SAMPLES) p->hist_len++;
}

static void recalc_eta(pf_notify_probe *p)
{
	double lin[PF_ETA_SAMPLES];
	int n = p->hist_len;
	for (int i = 0; i < n; i++) lin[i] = p->hist[(p->hist_head - n + i + PF_ETA_SAMPLES) % PF_ETA_SAMPLES];

	/* The next step the meat has not reached yet, and how long until it does. The estimator does
	 * not care what it is aimed at, so a step is the same question as the target asked of a nearer
	 * number -- which is what lets the grill say "get ready to flip" a minute before the flip
	 * rather than at the moment it is already due. */
	p->eta_step_s = -1;
	p->next_step[0] = 0;
	const pf_notify_step *next = NULL;
	for (int k = 0; k < p->nsteps; k++)
		if (!p->steps[k].fired && (!next || p->steps[k].temp_c < next->temp_c)) next = &p->steps[k];
	if (next) {
		double es = pf_notify_estimate_eta(lin, n, next->temp_c, ETA_INTERVAL_S);
		if (es >= 0) { p->eta_step_s = es; snprintf(p->next_step, sizeof p->next_step, "%s", next->name); }
		else snprintf(p->next_step, sizeof p->next_step, "%s", next->name);
	}

	double e = pf_notify_estimate_eta(lin, n, p->target_c, ETA_INTERVAL_S);
	/* blend with the previous estimate (minus the time that passed) so the readout counts down
	 * smoothly instead of jumping with every re-fit; a stall (no slope) clears it */
	if (e < 0) { p->eta_s = -1; return; }
	double prev = p->eta_s > 0 ? p->eta_s - ETA_RECALC_S : -1;
	p->eta_s = prev > 0 ? 0.6 * e + 0.4 * prev : e;
}

/* ---- evaluation ---- */

static const char *after_text(int after)
{
	return after == PF_AFTER_SHUTDOWN ? " Shutting down." : after == PF_AFTER_KEEPWARM ? " Switching to keep-warm." : "";
}

/* Steps live in settings (`notify.probe_steps.<label>`), not in the notify state, so they survive a
 * restart part way through a cook -- which is exactly when losing them would matter. What does not
 * survive is whether one has already spoken: that is a latch here, re-seeded whenever the list
 * changes, so editing a step arms it again and a restart mid-cook does not shout twice about a
 * temperature already passed. The daemon holds Celsius; the settings file holds the user's units,
 * which is how every other temperature in it is stored. */
static void load_steps(pf_notify_probe *p, pf_units units)
{
	char path[128];
	snprintf(path, sizeof path, "notify.probe_steps.%s", p->label);
	cJSON *arr = pf_set_dup(path);
	pf_notify_step keep[PF_MAX_STEPS];
	int nkeep = p->nsteps;
	memcpy(keep, p->steps, sizeof keep);
	p->nsteps = 0;
	cJSON *it;
	cJSON_ArrayForEach(it, arr) {
		if (p->nsteps >= PF_MAX_STEPS) break;
		double t = pf_json_num(it, "temp", 0);
		if (!(t > 0)) continue;
		pf_notify_step *st = &p->steps[p->nsteps++];
		pf_strlcpy(st->name, pf_json_str(it, "name", "Step"), sizeof st->name);
		st->temp_c = pf_to_c(t, units);
		/* an unchanged step keeps whatever it had already said */
		st->fired = false;
		for (int k = 0; k < nkeep; k++)
			if (fabs(keep[k].temp_c - st->temp_c) < 0.01 && !strcmp(keep[k].name, st->name)) { st->fired = keep[k].fired; break; }
	}
	cJSON_Delete(arr);
}

void pf_notify_tick(pf_notify *n, const pf_sensors *s, pf_mode mode, double now, pf_units units)
{
	bool cooking = mode == PF_MODE_STARTUP || mode == PF_MODE_REIGNITE || mode == PF_MODE_SMOKE || mode == PF_MODE_HOLD;
	bool do_eta = now - n->last_eta_t >= ETA_RECALC_S;
	if (do_eta) n->last_eta_t = now;
	const char *u = units == PF_UNITS_C ? "°C" : "°F";

	for (int i = 0; i < n->n; i++) {
		pf_notify_probe *p = &n->probes[i];
		int si = pf_probes_find(s, p->label);
		if (si < 0 || !s->p[si].valid) continue;
		double t = s->p[si].temp_c;
		const char *name = s->p[si].name;

		if (now - p->last_sample_t >= ETA_INTERVAL_S) { p->last_sample_t = now; push_sample(p, t); }

		/* The target is not cleared once it is met: the notification rules compare temp against
		 * target, so both have to stay visible. A latch keeps this from running twice, and the
		 * after-action (keep warm, shutdown) still fires exactly once. The messages themselves now
		 * come from the rules in features/rules.c. */
		if (p->target_c > 0 && !p->reached) {
			if (t >= p->target_c) {
				p->reached = true;
				p->eta_s = -1;
				if (p->after != PF_AFTER_NONE) { n->pending_action = p->after; p->after = PF_AFTER_NONE; }
			} else if (do_eta) recalc_eta(p);
		}
		/* A step speaks once, going up, and only while there is a cook to speak about. It resets
		 * when the probe falls back below it by a couple of degrees, so a probe pulled out and put
		 * back does not re-announce, but a genuinely new cook does. */
		load_steps(p, units);
		for (int k = 0; k < p->nsteps; k++) {
			pf_notify_step *st = &p->steps[k];
			if (!st->fired && cooking && t >= st->temp_c) {
				st->fired = true;
				pf_events_emit("Probe_Step", st->name, "%s reached %.0f%s for %s (%.0f%s).",
				               name, pf_from_c(st->temp_c, units), u, st->name, pf_from_c(t, units), u);
			} else if (st->fired && t < st->temp_c - pf_delta_to_c(2, units)) st->fired = false;
		}
		if (p->limit_high_c > 0) {
			if (t > p->limit_high_c && !p->high_tripped) { p->high_tripped = true; pf_events_emit("Probe_Temp_Limit_Alarm", "High temperature alarm", "%s is above %.0f%s (%.0f%s).", name, pf_from_c(p->limit_high_c, units), u, pf_from_c(t, units), u); }
			else if (t <= p->limit_high_c - pf_delta_to_c(2, units)) p->high_tripped = false;
		}
		if (p->limit_low_c > 0 && cooking) {
			if (t < p->limit_low_c && !p->low_tripped) { p->low_tripped = true; pf_events_emit("Probe_Temp_Limit_Alarm", "Low temperature alarm", "%s is below %.0f%s (%.0f%s).", name, pf_from_c(p->limit_low_c, units), u, pf_from_c(t, units), u); }
			else if (t >= p->limit_low_c + pf_delta_to_c(2, units)) p->low_tripped = false;
		}
	}

	if (n->timer.running && !n->timer.paused && now >= n->timer.end_t) {
		int after = n->timer.after;
		int mins = (int)(n->timer.duration / 60), secs = (int)n->timer.duration % 60;
		pf_events_emit("Timer_Expired", "Timer finished", "The %d:%02d timer is done.%s", mins, secs, after_text(after));
		pf_notify_timer_cancel(n);
		if (after != PF_AFTER_NONE) n->pending_action = after;
	}
}
