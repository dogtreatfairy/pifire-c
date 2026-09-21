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
/* Begin a run. `setpoints_json` may be an array of set points in user units, or NULL for the
 * configured defaults. 0 on success, -1 with err when the grill is busy or already tuning. */
int  pf_tuner_start(const cJSON *setpoints_json, char *err, size_t n);
/* Stop early. The anchors already measured are kept. */
void pf_tuner_stop(const char *why);
/* Drive the run; called once a second from the services thread with the latest status. */
void pf_tuner_tick(const cJSON *status, double now);
/* {running, phase, step, steps, setpoint, elapsed_s, message, anchors:[...]} */
cJSON *pf_tuner_json(void);
