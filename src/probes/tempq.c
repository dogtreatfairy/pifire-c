#include "probes/tempq.h"
#include <math.h>
#include <string.h>

void pf_tempq_init(pf_tempq *t)
{
	memset(t, 0, sizeof *t);
	t->last_avg = NAN;
}

void pf_tempq_reset(pf_tempq *t)
{
	pf_tempq_init(t);
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

double pf_tempq_push(pf_tempq *t, double c)
{
	if (isnan(c)) return t->have_avg ? t->last_avg : NAN;

	if (t->len == 0) {
		/* Pre-fill both stages so the very first reading is usable: a grill that has just been
		 * switched on should show its temperature at once, not once a window has filled. */
		for (int i = 0; i < PF_TEMPQ_MED; i++) t->med[i] = c;
		for (int i = 0; i < PF_TEMPQ_LEN; i++) t->q[i] = c;
		t->len = PF_TEMPQ_LEN;
		t->last_avg = c;
		t->have_avg = true;
		return c;
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
	return t->last_avg;
}
