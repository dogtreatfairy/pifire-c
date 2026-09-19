#pragma once
/* Cycle engine: converts a feed ratio u into auger on/off timing within a fixed cycle.
 * Pure logic, no I/O, unit-testable. */
#include <stdbool.h>

typedef struct {
	double cycle_s;    /* total cycle length */
	double u_min, u_max;
	double max_on_s;   /* absolute cap on continuous auger-on, from safety settings */
} pf_cycle_cfg;

typedef struct {
	double start;      /* monotonic time the current cycle began */
	double cycle_s;    /* length of the current cycle */
	double on_s;       /* auger-on seconds this cycle */
	double u_raw;      /* requested */
	double u_applied;  /* after clamps */
	int    saturated;  /* -1 at u_min, +1 at u_max, 0 free */
	bool   active;
} pf_cycle;

/* Start a new cycle at `now` with the requested ratio. Clamps and records saturation. */
void pf_cycle_begin(pf_cycle *c, const pf_cycle_cfg *cfg, double now, double u_raw);
/* Start a fixed on/off cycle (Smoke / Startup P-mode); only max_on_s from cfg applies. */
void pf_cycle_begin_fixed(pf_cycle *c, const pf_cycle_cfg *cfg, double now, double on_s, double off_s);
bool pf_cycle_auger_on(const pf_cycle *c, double now);
bool pf_cycle_done(const pf_cycle *c, double now);
void pf_cycle_stop(pf_cycle *c);
