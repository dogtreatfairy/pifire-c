#include "probes/tempq.h"
#include <math.h>
#include <string.h>

void pf_tempq_init(pf_tempq *t)
{
	memset(t, 0, sizeof *t);
	t->last_avg = NAN;
	t->out_c = NAN;
}

void pf_tempq_reset(pf_tempq *t)
{
	double slew = t->slew_c_s, hold = t->hold_s;
	pf_tempq_init(t);
	t->slew_c_s = slew; t->hold_s = hold;
}

void pf_tempq_limits(pf_tempq *t, double slew_c_s, double hold_s)
{
	t->slew_c_s = slew_c_s > 0 ? slew_c_s : 0;
	t->hold_s = hold_s > 0 ? hold_s : 0;
}

/* median of a small array, by insertion sort into a copy; PF_TEMPQ_MED is five */
static double median(const double *v, int n)
{
	double s[PF_TEMPQ_MED];
	for (int i = 0; i < n; i++) {
		double x = v[i];
		int j = i;
		while (j > 0 && s[j - 1] > x) { s[j] = s[j - 1]; j--; }
		s[j] = x;
	}
	return n & 1 ? s[n / 2] : 0.5 * (s[n / 2 - 1] + s[n / 2]);
}

/* the gate: where the filter says the probe is, against what was last reported */
static double gate(pf_tempq *t, double f, double now)
{
	if (!(t->slew_c_s > 0) || isnan(t->out_c)) { t->out_c = f; t->out_t = now; t->hold_since = 0; t->unstable = false; return f; }
	double dt = now - t->out_t;
	if (!(dt > 0)) dt = 0.25;
	if (dt > 5) dt = 5;
	double allowed = t->slew_c_s * dt, d = f - t->out_c;
	if (fabs(d) <= allowed) { t->out_c = f; t->hold_since = 0; t->unstable = false; }
	else if (t->hold_since > 0 && now - t->hold_since >= t->hold_s) { t->out_c = f; t->hold_since = 0; t->unstable = false; }   /* it lasted: believed */
	else {
		if (!(t->hold_since > 0)) t->hold_since = now;
		t->out_c += d > 0 ? allowed : -allowed;
		t->unstable = true;
	}
	t->out_t = now;
	return t->out_c;
}

double pf_tempq_push(pf_tempq *t, double c, double now)
{
	if (isnan(c)) return t->have_avg ? (isnan(t->out_c) ? t->last_avg : t->out_c) : NAN;

	if (t->len == 0) {
		/* Pre-fill both stages so the very first reading is usable: a grill that has just been
		 * switched on should show its temperature at once, not once a window has filled. */
		for (int i = 0; i < PF_TEMPQ_MED; i++) t->med[i] = c;
		for (int i = 0; i < PF_TEMPQ_LEN; i++) t->q[i] = c;
		t->len = PF_TEMPQ_LEN;
		t->last_avg = c;
		t->have_avg = true;
		return gate(t, c, now);
	}

	/* Stage one: the median throws away a wrong sample instead of averaging it in. */
	memmove(&t->med[1], &t->med[0], sizeof(double) * (PF_TEMPQ_MED - 1));
	t->med[0] = c;
	double m = median(t->med, PF_TEMPQ_MED);

	/* Stage two: a running mean of what survived, to take off the remaining jitter. */
	memmove(&t->q[1], &t->q[0], sizeof(double) * (PF_TEMPQ_LEN - 1));
	t->q[0] = m;
	double mean = 0;
	for (int i = 0; i < PF_TEMPQ_LEN; i++) mean += t->q[i];
	t->last_avg = mean / PF_TEMPQ_LEN;
	t->have_avg = true;

	/* Stage three: the gate. */
	return gate(t, t->last_avg, now);
}
