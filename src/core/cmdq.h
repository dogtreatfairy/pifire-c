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
} pf_cmd_type;

typedef struct {
	pf_cmd_type type;
	pf_mode mode;
	double num;
	bool flag;
	char str[64];
} pf_cmd;

void pf_cmdq_init(void);
int  pf_cmdq_push(const pf_cmd *c);            /* 0 ok, -1 full */
bool pf_cmdq_pop(pf_cmd *out);                 /* non-blocking */
/* convenience */
int  pf_cmd_mode(pf_mode m, double setpoint_user);
int  pf_cmd_simple(pf_cmd_type t);
