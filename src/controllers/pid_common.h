#pragma once
/* Helpers shared by the built-in controllers. The config JSON handed to a controller is its
 * settings.controller.config.<id> object plus a "_units" key ("F"/"C") injected by the daemon so
 * temperature-valued options can be converted to Celsius. */
#include "pifire/common.h"
#include "pifire/controller.h"
#include <cJSON.h>

static inline double pf_pid_cfg_num(const cJSON *c, const char *key, double dflt)
{
	const cJSON *n = c ? cJSON_GetObjectItemCaseSensitive(c, key) : NULL;
	return cJSON_IsNumber(n) ? n->valuedouble : dflt;
}

static inline bool pf_pid_cfg_bool(const cJSON *c, const char *key, bool dflt)
{
	const cJSON *n = c ? cJSON_GetObjectItemCaseSensitive(c, key) : NULL;
	return cJSON_IsBool(n) ? cJSON_IsTrue(n) : dflt;
}

static inline pf_units pf_pid_cfg_units(const cJSON *c)
{
	const cJSON *n = c ? cJSON_GetObjectItemCaseSensitive(c, "_units") : NULL;
	return (cJSON_IsString(n) && n->valuestring[0] == 'C') ? PF_UNITS_C : PF_UNITS_F;
}

/* Built-in controller constructors. */
const pf_controller_ops *pf_controller_pid(void);
const pf_controller_ops *pf_controller_pid_clamping(void);
const pf_controller_ops *pf_controller_pid_clamping_percent_pb(void);
const pf_controller_ops *pf_controller_pid_ac(void);
const pf_controller_ops *pf_controller_pid_sp(void);
const pf_controller_ops *pf_controller_pid_parallel(void);
const pf_controller_ops *pf_controller_adaptive(void);
