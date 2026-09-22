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
/* Bumped on every stored result, so a caller can tell a fresh measurement from the previous one
 * without relying on the clock (two results can land in the same second). */
unsigned pf_learning_autotune_gen(void);

/* Gain schedule: one controller, tuning that follows the set point.
 * A pellet grill loses more heat the hotter it runs, so its process gain falls as the set point
 * rises and a single proportional band cannot suit 180 F and 450 F at once. Autotune measures
 * the loop at a temperature and stores one entry per temperature; the controller interpolates
 * between them. This is the tuning library the app shows. */
#define PF_TUNE_ANCHORS 8
/* `ambient_c` and `wind` are the conditions the measurement was taken in. A grill behaves
 * differently on a still 80 F afternoon than in a 20 F wind, so an anchor is only fully meaningful
 * alongside the weather it was measured in, and the app shows both. */
typedef struct { double setpoint_c, Ku, Pu, PB_c, Ti, Td, ts, ambient_c, wind; int runs; bool valid; } pf_tune_anchor;

/* Store a measurement. A set point already in the library is REFINED rather than replaced: a relay
 * test measures the grill on one afternoon, with that day's wind and that hopper's pellets, and a
 * single run carries that day's noise with it. Successive runs average the noise out. */
void pf_learning_store_anchor(double setpoint_c, const pf_autotune_result *r, double ambient_c, double wind);
/* Restore an anchor exactly as given, runs count and all -- for a backup, which is not new evidence
 * about the grill and must not be averaged into anything. */
void pf_learning_put_anchor(const pf_tune_anchor *a);
/* Gains for this set point, interpolated between anchors and clamped outside their range.
 * False when the schedule is empty, in which case the controller keeps its own tuning. */
bool pf_learning_gains(double setpoint_c, double *PB_c, double *Ti, double *Td);
int  pf_learning_anchor_list(pf_tune_anchor *out, int max);
void pf_learning_clear_anchors(void);
void pf_learning_reset(void);
cJSON *pf_learning_json(void);   /* everything above, temperatures in user units */

/* Back up and restore everything the grill has learned about itself.
 *
 * The tuning library is hours of the grill's own time and a hopper of pellets, and it lives in a
 * database on an SD card. An export is canonical: temperatures in Celsius as the daemon holds them,
 * so a backup taken in Fahrenheit still restores correctly on a grill set to Celsius. It carries
 * the whole picture rather than the anchors alone -- the plant model, the feed-forward fit, and the
 * controller and gains actually in force -- because a restored library that lands on a different
 * controller is not the tuning that was measured. */
cJSON *pf_learning_export(void);
/* Returns the number of anchors restored, or -1 if the document is not one of ours. */
int pf_learning_import(const cJSON *doc, char *err, size_t n);
