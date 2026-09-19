#pragma once
/* Learning across cooks (all Celsius):
 *   - steady-state observations (setpoint, ambient, feed ratio) -> ridge-regularised, recency
 *     weighted fit  u_ff = a + b * (setpoint - ambient)   used as feed-forward
 *   - passive FOPDT (K, tau, theta) identification from each startup rise
 *   - relay autotune results (Ku, Pu -> PB/Ti/Td) */
#include <cJSON.h>
#include <stdbool.h>

typedef struct { double a, b; int n; double rms; } pf_ff_fit;
typedef struct { double K, tau, theta; double ts; bool valid; } pf_fopdt;
typedef struct { double Ku, Pu, PB_c, Ti, Td, amplitude_c; double ts; bool valid; } pf_autotune_result;

void pf_learning_init(void);
bool pf_learning_enabled(void);
/* Record one steady-state observation. */
void pf_learning_observe(const char *controller, double setpoint_c, double ambient_c, double u_mean, double pit_stdev_c, const char *pellet);
/* Feed-forward for a set point / ambient; returns clamped u_ff (or fallback when n < 3). */
double pf_learning_uff(double setpoint_c, double ambient_c, double u_min, double u_max, int *n_out);
pf_ff_fit pf_learning_fit(void);
void pf_learning_store_fopdt(double K, double tau, double theta);
pf_fopdt pf_learning_fopdt(void);
void pf_learning_store_autotune(const pf_autotune_result *r);
pf_autotune_result pf_learning_autotune(void);
void pf_learning_reset(void);
cJSON *pf_learning_json(void);   /* everything above, temperatures in user units */
