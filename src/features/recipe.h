#pragma once
/* Recipes: multi-step programs. Storage/CRUD here; the runner lives in control.c.
 * Step JSON: {"mode":"Startup|Smoke|Hold|Shutdown","setpoint":225,"s_plus":false,
 *             "timer_min":0,"probe":"Probe1","probe_temp":0,"pause":false,"message":""} */
#include "pifire/common.h"
#include <cJSON.h>

#define PF_RECIPE_MAX_STEPS 16

typedef struct {
	pf_mode mode;
	double setpoint_c;
	bool s_plus;
	double timer_s;          /* 0 = none */
	char probe[PF_LABEL_LEN];
	double probe_temp_c;     /* 0 = none */
	bool pause;              /* wait for the user after the trigger fires */
	char message[128];
} pf_recipe_step;

typedef struct {
	int id;
	char name[64];
	int nsteps;
	pf_recipe_step steps[PF_RECIPE_MAX_STEPS];
} pf_recipe;

void pf_recipes_init(void);
cJSON *pf_recipes_list(void);                       /* [{id,name,description,steps:[...]}] */
int  pf_recipe_save(const char *json, char *err, size_t errn);   /* returns id or <0 */
int  pf_recipe_delete(int id);
/* Load into a runner-ready struct (temperatures converted to Celsius). 0 on success. */
int  pf_recipe_load(int id, pf_recipe *out);
