#pragma once
/* Pellet manager: brands/profiles, current load, hopper level from a distance sensor, usage
 * estimate from auger run time, low-pellet warnings. */
#include <cJSON.h>
#include <stdbool.h>

int  pf_pellets_init(bool sim);
void pf_pellets_shutdown(void);
/* Called ~1 Hz from the services thread. auger_on_total_s is the running total from control. */
void pf_pellets_tick(double now, double auger_on_total_s, bool cooking);
void pf_pellets_request_check(void);
int  pf_pellets_hopper_pct(void);   /* -1 unknown / no sensor */

/* {"current":{id,brand,wood,loaded_ts,est_usage_g},"hopper":{pct,cm,updated,enabled},"profiles":[...],"log":[...]} */
cJSON *pf_pellets_json(void);
int  pf_pellets_profile_save(const char *json, char *err, size_t errn);   /* {id?,brand,wood,rating,comments} */
int  pf_pellets_profile_delete(int id);
int  pf_pellets_load(int id);       /* mark profile as loaded, reset usage */
