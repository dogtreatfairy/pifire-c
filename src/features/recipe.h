#pragma once
/* Recipes: multi-step programs. Storage/CRUD here; the runner lives in control.c.
 * Step JSON: {"mode":"Startup|Smoke|Hold|Shutdown","setpoint":225,"s_plus":false,
 *             "timer_min":0,"probe":"Probe1","probe_temp":0,"probe_match":"any|all",
 *             "carryover":false,"wait":"none|confirm|lid","pause":false,"message":"",
 *             "lead_min":0,"lead_message":""} */
#include "pifire/common.h"
#include <cJSON.h>

#define PF_RECIPE_MAX_STEPS 16
/* A step aimed at "@food" watches every Food probe in the cook rather than one named probe, which
 * is what lets a recipe be written once and used with however many probes go in the meat. */
#define PF_RECIPE_ANY_FOOD "@food"

/* How a step ends when it is not a clock or a temperature that ends it.
 *
 * CONFIRM waits for the cook to say they have done the thing the message asked for. LID also
 * accepts the lid being opened: taking the ribs off to wrap them is a lid event, and asking
 * someone with both hands full to find their phone first is asking them to do it later. */
typedef enum { PF_RSTEP_WAIT_NONE = 0, PF_RSTEP_WAIT_CONFIRM, PF_RSTEP_WAIT_LID } pf_rstep_wait;

typedef struct {
	pf_mode mode;
	double setpoint_c;
	bool s_plus;
	double timer_s;          /* 0 = none */
	char probe[PF_LABEL_LEN];/* a probe label, or PF_RECIPE_ANY_FOOD */
	double probe_temp_c;     /* 0 = none */
	bool probe_all;          /* every food probe must reach it, not just the first one to */
	/* Pull early by the coast the meat will still do off the heat, so the number in the recipe is
	 * where it ends up rather than where it was when it came off. */
	bool carryover;
	pf_rstep_wait wait;
	bool pause;              /* wait for the user after the trigger fires */
	char message[128];
	/* "Ten minutes until you wrap": a warning before the step is due to end, so the cook can be
	 * at the grill when it does rather than being told at the moment it is already due. */
	double lead_s;           /* 0 = no warning */
	char lead_message[128];
} pf_recipe_step;

typedef struct {
	int id;
	char name[64];
	int nsteps;
	pf_recipe_step steps[PF_RECIPE_MAX_STEPS];
} pf_recipe;

void pf_recipes_init(void);
/* Add the recipes PiFire ships with, once, on a grill that has never had them. */
void pf_recipes_seed(void);
cJSON *pf_recipes_list(void);                       /* [{id,name,description,steps:[...]}] */
int  pf_recipe_save(const char *json, char *err, size_t errn);   /* returns id or <0 */
int  pf_recipe_delete(int id);
/* Load into a runner-ready struct (temperatures converted to Celsius). 0 on success. */
int  pf_recipe_load(int id, pf_recipe *out);
