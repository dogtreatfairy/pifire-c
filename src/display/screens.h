#pragma once
/* Screen layouts shared by the TFT driver and the render test: draw from the status JSON. */
#include "display/gfx.h"
#include <cJSON.h>

typedef enum { PF_SCR_MAIN = 0, PF_SCR_MENU, PF_SCR_SETPOINT, PF_SCR_MESSAGE } pf_screen;

typedef struct {
	pf_screen screen;
	int menu_index;          /* highlighted menu entry */
	double edit_setpoint;    /* user units */
	char message[64];
	double message_until;
} pf_ui_state;

#define PF_MENU_COUNT 7
extern const char *const pf_menu_items[PF_MENU_COUNT];

/* Render whatever the state says onto g using the latest status JSON (may be NULL). */
void pf_screens_render(pf_gfx *g, const cJSON *status, const pf_ui_state *ui);
