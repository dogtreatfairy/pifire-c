#pragma once
/* Commands from the web/API/CLI into the control thread. */
#include "pifire/common.h"

typedef enum {
	PF_CMD_NONE = 0,
	PF_CMD_MODE,           /* mode, num = setpoint (user units, 0 = keep), str = optional next mode */
	PF_CMD_SETPOINT,       /* num = setpoint (user units) */
	PF_CMD_SMOKE_PLUS,     /* flag */
	PF_CMD_PWM_CONTROL,    /* flag */
	PF_CMD_DUTY_CYCLE,     /* num = fan % */
	PF_CMD_MANUAL_OUTPUT,  /* str = output name, flag = on, num = fan pct (for "pwm") */
	PF_CMD_LID_TOGGLE,
	PF_CMD_PRIME,          /* num = grams, str = next mode ("Startup" or "") */
	PF_CMD_SETTINGS_CHANGED,
	PF_CMD_CONTROLLER_CHANGED,
	PF_CMD_PROBES_CHANGED,
	PF_CMD_STOP,           /* e-stop: always honoured */
	PF_CMD_CLEAR_ERROR,
	PF_CMD_NOTIFY_TARGET,  /* str = probe label, num = target (user units, 0 clears), aux = after-action */
	PF_CMD_NOTIFY_LIMITS,  /* str = label, num = high, num2 = low (user units, 0 = off) */
	PF_CMD_TIMER_START,    /* num = seconds, aux = after-action */
	PF_CMD_TIMER_PAUSE,
	PF_CMD_TIMER_RESUME,
	PF_CMD_TIMER_CANCEL,
	PF_CMD_NOTIFY_TEST,
	PF_CMD_PROBES_IN_USE,  /* str = comma-separated labels of the probes that are in the food */
	PF_CMD_RECIPE_START,   /* num = recipe id */
	PF_CMD_RECIPE_NEXT,    /* continue past a paused step */
	PF_CMD_RECIPE_STOP,    /* abandon the program (grill keeps its current mode) */
	PF_CMD_AUTOTUNE_START,
	PF_CMD_AUTOTUNE_STOP,
	PF_CMD_TUNING_APPLY,   /* push the stored autotune / plant fit into the active controller */
	PF_CMD_FORGET_LEARNING,/* aux = pf_clear_what */
} pf_cmd_type;

/* What a clearing is for. Each one names a thing that can be wrong rather than a set of flags,
 * because which stored data and which half of the controller have to go is not obvious from the
 * flags and was got wrong once already. */
typedef enum {
	PF_CLEAR_LEARNING = 1,   /* what the grill worked out for itself; what was measured stays */
	PF_CLEAR_TUNING,         /* what was measured, so the typed values govern again */
	PF_CLEAR_FOR_BASELINE,   /* a baseline has just been measured: everything else goes, it stays */
} pf_clear_what;

typedef struct {
	pf_cmd_type type;
	pf_mode mode;
	double num, num2;
	int aux;
	bool flag;
	char str[256];         /* room for a list of probe labels */
} pf_cmd;

void pf_cmdq_init(void);
int  pf_cmdq_push(const pf_cmd *c);            /* 0 ok, -1 full */
bool pf_cmdq_pop(pf_cmd *out);                 /* non-blocking */
/* convenience */
int  pf_cmd_mode(pf_mode m, double setpoint_user);
int  pf_cmd_simple(pf_cmd_type t);
