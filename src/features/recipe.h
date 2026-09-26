#pragma once
/* Recipes: multi-step programs. Storage/CRUD here; the runner lives in control.c.
 * Step JSON: {"mode":"Startup|Smoke|Hold|Shutdown","setpoint":225,"s_plus":false,
 *             "timer_min":0,"probe":"Probe1","probe_temp":0,"probe_match":"any|all",
 *             "carryover":false,"wait":"none|confirm|lid|lid_and","pause":false,"message":"",
 *             "lead_min":0,"lead_message":""} */
#include "pifire/common.h"
#include <cJSON.h>

#define PF_RECIPE_MAX_STEPS 16
/* A step aimed at "@food" watches every Food probe in the cook rather than one named probe, which
 * is what lets a recipe be written once and used with however many probes go in the meat. */
#define PF_RECIPE_ANY_FOOD "@food"

/* How a step ends when it is not a clock or a temperature that ends it.
 *
 * Two signals -- the cook saying so, and the lid being opened -- combined the way a condition is.
 *
 *   CONFIRM   the prompt alone.
 *   LID       the lid OR the prompt: either ends it. Taking the ribs off to wrap them is a lid
 *             event, and asking someone with both hands full to find their phone first is asking
 *             them to do it later.
 *   LID_AND   the lid AND the prompt: both have to have happened. For a step where the meat has to
 *             physically come off before the answer means anything.
 *
 * There is no lid-only: the prompt is always one of the signals, because a lid switch that does
 * not fire would otherwise strand a recipe with no way to carry on. */
typedef enum { PF_RSTEP_WAIT_NONE = 0, PF_RSTEP_WAIT_CONFIRM, PF_RSTEP_WAIT_LID, PF_RSTEP_WAIT_LID_AND } pf_rstep_wait;

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
/* What is wrong with the shape of a recipe's steps, as [{code,message}]: "no_startup" when it
 * cooks without lighting the grill first, "no_shutdown" when it finishes without putting it out.
 * Advisory -- see the note on the implementation for why neither is enforced. Caller frees. */
cJSON *pf_recipe_shape_warnings(const cJSON *steps);
/* Load into a runner-ready struct (temperatures converted to Celsius). 0 on success. */
int  pf_recipe_load(int id, pf_recipe *out);
