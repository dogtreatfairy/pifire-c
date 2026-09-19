#include "probes/tempq.h"
#include <math.h>
#include <string.h>

void pf_tempq_init(pf_tempq *t, double stdev_max_c)
{
	memset(t, 0, sizeof *t);
	t->stdev_max_c = stdev_max_c > 0 ? stdev_max_c : 2.5;
	t->last_avg = NAN;
}

void pf_tempq_reset(pf_tempq *t)
{
	double s = t->stdev_max_c;
	pf_tempq_init(t, s);
}

double pf_tempq_push(pf_tempq *t, double c)
{
	if (isnan(c)) return t->have_avg ? t->last_avg : NAN;
	if (t->len == 0) {
		for (int i = 0; i < PF_TEMPQ_LEN; i++) t->q[i] = c;  /* pre-fill so the first reading is usable */
		t->len = PF_TEMPQ_LEN;
	} else {
		memmove(&t->q[1], &t->q[0], sizeof(double) * (PF_TEMPQ_LEN - 1));
		t->q[0] = c;
	}
	double mean = 0;
	for (int i = 0; i < PF_TEMPQ_LEN; i++) mean += t->q[i];
	mean /= PF_TEMPQ_LEN;
	double var = 0;
	for (int i = 0; i < PF_TEMPQ_LEN; i++) var += (t->q[i] - mean) * (t->q[i] - mean);
	double sd = sqrt(var / (PF_TEMPQ_LEN - 1));
	if (sd < t->stdev_max_c || !t->have_avg) {
		t->last_avg = mean;
		t->have_avg = true;
	}
	return t->last_avg;
}
