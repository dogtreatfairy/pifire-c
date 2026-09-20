#pragma once
/* Screen layouts shared by the TFT driver and the render test: draw from the status JSON. */
#include "display/gfx.h"
#include <cJSON.h>

typedef enum { PF_SCR_MAIN = 0, PF_SCR_MENU, PF_SCR_SETPOINT, PF_SCR_MESSAGE } pf_screen;

/* menu entries depend on the running mode (same sets as the original PiFire display) */
typedef enum {
	PF_MI_STARTUP, PF_MI_HOLD, PF_MI_PRIME, PF_MI_MONITOR, PF_MI_SMOKE, PF_MI_SMOKE_PLUS,
	PF_MI_SHUTDOWN, PF_MI_STOP, PF_MI_CLEAR, PF_MI_BACK
} pf_menu_id;

typedef struct { pf_menu_id id; char label[28]; } pf_menu_item;
#define PF_MENU_MAX 8

typedef struct {
	pf_screen screen;
	int menu_index;          /* highlighted menu entry */
	double edit_setpoint;    /* user units */
	bool edit_is_change;     /* true: change the running set point, false: start Hold at it */
	char message[64];
	double message_until;
	bool blink;              /* toggled by the driver each tick: drives the done / over-target flash */
} pf_ui_state;

/* Build the menu for the current mode. Returns the item count. */
int pf_menu_build(const cJSON *status, pf_menu_item *out, int max);

/* Render whatever the state says onto g using the latest status JSON (may be NULL). */
void pf_screens_render(pf_gfx *g, const cJSON *status, const pf_ui_state *ui);
