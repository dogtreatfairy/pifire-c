#pragma once
/* Guided tuning: one button that starts the grill, measures the loop at each anchor set point in
 * turn, stores what it finds as a gain schedule, and shuts the grill down again.
 *
 * Nothing here talks to the control loop directly. It watches the published status and pushes the
 * same commands the web app and the panel push, so a run is exactly what a patient person with a
 * stopwatch would do. */
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>

void pf_tuner_init(void);
/* Begin a run.
 *
 * `setpoints_json` may be an array of set points in the user's units, or NULL for the configured
 * full profile. `full_profile` says what the result means: a full profile is a new baseline for
 * the grill and clears the tuning library before it starts, while a single set point is added to
 * the library alongside whatever is already there.
 *
 * 0 on success, -1 with err when the grill is busy or a run is already going. */
int  pf_tuner_start(const cJSON *setpoints_json, bool full_profile, char *err, size_t n);
/* Stop early. The anchors already measured are kept. */
void pf_tuner_stop(const char *why);
/* Drive the run; called once a second from the services thread with the latest status. */
void pf_tuner_tick(const cJSON *status, double now);
/* Is a run going, and what is it aiming at? The relay only oscillates for part of a run, so this
 * is true through the startup and the settling too, which is when someone glancing at the grill
 * most needs to know why it lit itself. Any argument may be NULL. */
bool pf_tuner_active(double *setpoint_user, int *step, int *steps);

/* {running, phase, step, steps, setpoint, elapsed_s, message, anchors:[...]} */
cJSON *pf_tuner_json(void);
