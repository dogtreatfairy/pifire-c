#pragma once
/* Simulated grill: platform ops plus a firepot/pit thermal model driven by the daemon's outputs.
 * Probe devices (sim_probe.c) and tests read the model through pf_sim_model(). */
#include "pifire/platform.h"

typedef struct {
	/* environment (settable) */
	double ambient_c;
	double wind;          /* 0..1 extra heat loss */
	bool   lid_open;
	double time_scale;    /* model seconds per real second when driven by the thread (default 1) */
	/* outputs as last commanded */
	bool out[PF_OUT_COUNT];
	int  fan_pct;
	/* state */
	double pit_c;
	double food_c[3];
	double pot_pellets_g;
	bool   fire_lit;
	double igniter_time_s;   /* continuous igniter-on with pellets */
	double starved_time_s;   /* continuous seconds with an empty pot */
	double delay[8];         /* dead-time line for pit response */
	double sim_time_s;
} pf_sim_state;

const pf_platform_ops *pf_platform_sim(void);
pf_sim_state *pf_sim_model(void);          /* NULL until the sim platform is created */
void pf_sim_step(double dt_s);              /* advance the model */
void pf_sim_reset(double ambient_c);

/* The simulator's own dynamics -- the pit's time constant, its dead time and the pot's lag -- so a
 * test can work out the ultimate period the relay ought to find and hold the tuner to it. */
void pf_sim_plant(double *tau_s, double *theta_s, double *pot_tau_s);
/* Degrees of settled pit per unit duty at this ambient and duty; see the implementation. */
double pf_sim_small_signal_gain(double amb_c, double duty);
/* Set the pit's time constant and dead time (seconds) for the runs that follow; 0 keeps a value. */
void pf_sim_set_plant(double tau_s, double theta_s);
