#pragma once
/* Per-probe smoothing/outlier filter (port of probes/temp_queue.py), values in Celsius. */
#include <stdbool.h>

#define PF_TEMPQ_LEN 10

typedef struct {
	double q[PF_TEMPQ_LEN];
	int len;
	double last_avg;
	bool have_avg;
	double stdev_max_c;   /* window rejected when stdev exceeds this (default 2.5 C) */
} pf_tempq;

void   pf_tempq_init(pf_tempq *t, double stdev_max_c);
void   pf_tempq_reset(pf_tempq *t);
/* Push a sample and return the filtered value (NAN until the window is full and never valid). */
double pf_tempq_push(pf_tempq *t, double c);
