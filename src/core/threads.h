#pragma once
#include "core/control.h"

typedef struct {
	bool sim;
	double sim_time_scale;   /* model seconds per real second */
} pf_threads_opts;

int  pf_threads_start(pf_control *ctrl, const pf_threads_opts *opts);
void pf_threads_stop(void);
/* seconds since the control thread last completed a tick */
double pf_threads_control_age(void);
bool pf_threads_power_off_requested(void);
