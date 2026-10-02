#pragma once
/* Screen layouts shared by the TFT driver and the render test: draw from the status JSON.
 *
 * The panel is a small navigation stack. Every screen is pushed onto it, so "Back" is always the
 * same operation and a long press can unwind to the main screen from anywhere. Menus name modes and
 * actions, never sentences. */
#include "display/gfx.h"
#include <cJSON.h>

typedef enum {
	PF_SCR_MAIN = 0,   /* the running grill screen */
	PF_SCR_LIST,       /* a menu: one row per action */
	PF_SCR_TEMP,       /* temperature selector: value, an action button, Back */
	PF_SCR_CONFIRM,    /* two buttons: cancel on the left, the action on the right */
	PF_SCR_NETINFO,    /* address QR code */
	PF_SCR_MESSAGE,    /* transient notice */
	PF_SCR_MANUAL,     /* Monitor control: switch the outputs by hand */
	PF_SCR_BTSCAN,     /* Bluetooth probes a scan found */
	PF_SCR_MARGINS,    /* screen margins, adjusted while looking at the screen */
} pf_screen;

/* Which list a PF_SCR_LIST screen is showing. */
typedef enum {
	PF_LIST_ROOT = 0,  /* the mode decides: Stopped, Active or Monitor */
	PF_LIST_STARTUP,   /* Startup To Hold / Startup To Smoke */
	PF_LIST_POWER,     /* Restart / Shut Down */
	PF_LIST_PROBE,     /* pick a probe to give a target */
	PF_LIST_BT,        /* Bluetooth probes: connect, edit, delete */
	PF_LIST_BTKIND,    /* pick a make of Bluetooth probe */
	PF_LIST_BTEDIT,    /* paired probes: switch one on or off */
	PF_LIST_BTDEL,     /* paired probes: remove one */
	PF_LIST_SETTINGS,  /* panel settings: what can sensibly be changed at the grill */
	PF_LIST_MODE,      /* from the banner: a new mode, or stop */
	PF_LIST_HOPPER,    /* from the hopper: mark it full or empty at its current level */
	PF_LIST_PROBE_ACT, /* a probe with a target: change it, a custom temperature, or clear it */
	PF_LIST_MEAT,      /* a probe without one: what is on it */
	PF_LIST_DONE,      /* ...and how done, which sets the target with its name */
	PF_LIST_TIMER,     /* from the banner's corner while a timer runs: change it or cancel it */
	PF_LIST_STOP,      /* one Stop, which asks: End Cook (graceful) or Emergency Stop */
} pf_list_id;

/* What a row does when it is pressed. */
typedef enum {
	PF_ACT_NONE = 0,
	PF_ACT_BACK,
	PF_ACT_LIST,          /* arg = pf_list_id */
	PF_ACT_STARTUP,       /* plain startup, no target chosen */
	PF_ACT_STARTUP_HOLD,  /* opens the temperature selector, applied after startup */
	PF_ACT_STARTUP_SMOKE,
	PF_ACT_HOLD,          /* opens the temperature selector, applied now */
	PF_ACT_SMOKE,         /* confirmed, then applied now */
	PF_ACT_MONITOR,
	PF_ACT_MANUAL,        /* Monitor control */
	PF_ACT_END_COOK,      /* graceful shutdown */
	PF_ACT_STOP,
	PF_ACT_ESTOP,
	PF_ACT_CLEAR_ERROR,
	PF_ACT_PROBE_TARGET,  /* arg = index into the status probe array */
	PF_ACT_BT_SCAN,       /* arg = index into BT_KINDS */
	PF_ACT_BT_ADD,        /* arg = index into ui->bt[] */
	PF_ACT_BT_TOGGLE,     /* arg = index into the paired device list */
	PF_ACT_BT_DELETE,     /* arg = index into the paired device list */
	PF_ACT_NETINFO,
	PF_ACT_RESTART,
	PF_ACT_POWEROFF,
	PF_ACT_MARGINS,       /* open the margin editor */
	PF_ACT_THEME,         /* switch the panel between dark and light */
	PF_ACT_COLOUR,        /* swap the panel's red/blue order, judged with the screen in front of you */
	PF_ACT_HOPPER_FULL,   /* the hopper is full at this reading */
	PF_ACT_HOPPER_EMPTY,  /* the hopper is empty at this reading */
	PF_ACT_PROBE_PICK,    /* open the meat list for ui->probe_idx */
	PF_ACT_PROBE_CUSTOM,  /* open the temperature selector for ui->probe_idx */
	PF_ACT_PROBE_CLEAR,   /* clear ui->probe_idx's target */
	PF_ACT_MEAT,          /* arg = index into PF_MEATS: open its doneness list */
	PF_ACT_DONE,          /* arg = index into that meat's doneness: set the target */
	PF_ACT_TIMER,         /* open the minutes selector; confirmed, starts the timer */
	PF_ACT_TIMER_CANCEL,
	PF_ACT_RECIPE_NEXT,   /* continue past a step that is waiting */
	PF_ACT_RECIPE_SKIP,   /* confirmed: end this step now */
	PF_ACT_RECIPE_BACK,   /* confirmed: begin the previous step again */
	PF_ACT_RECIPE_EXIT,   /* confirmed: leave the recipe, the grill keeps its mode */
	PF_ACT_STOP_GRILL,    /* confirmed: stop */
	PF_ACT_TIMER_CHANGE,  /* the minutes selector, loaded with what is left */
	PF_ACT_PMODE,         /* Smoke's P-mode, from the main screen: the value selector, P0..P9 */
} pf_action;

/* The marks a menu row wears: drawn from lines and discs at the panel's own resolution. */
typedef enum {
	PF_ICON_NONE = 0, PF_ICON_PLAY, PF_ICON_HOLD, PF_ICON_SMOKE, PF_ICON_STOP, PF_ICON_POWER, PF_ICON_TIMER,
	PF_ICON_PROBE, PF_ICON_GEAR, PF_ICON_WIFI, PF_ICON_BACK, PF_ICON_EYE, PF_ICON_SLIDERS, PF_ICON_BT,
	PF_ICON_NEXT, PF_ICON_PREV, PF_ICON_EXIT, PF_ICON_HOPPER, PF_ICON_CHECK,
	PF_ICON_FLAG,      /* the chequered flag: the graceful stop, everywhere */
	/* what is on the probe, in PF_MEATS order */
	PF_ICON_STEAK, PF_ICON_BRISKET, PF_ICON_PORK, PF_ICON_RIBS, PF_ICON_CHICKEN, PF_ICON_TURKEY, PF_ICON_FISH, PF_ICON_LAMB, PF_ICON_SAUSAGE,
} pf_icon;

typedef struct {
	pf_action act;
	int arg;
	pf_icon icon;
	char label[26];
	char right[12];    /* optional right-hand column, e.g. a probe's current reading */
	bool danger;
} pf_menu_item;

/* how far a margin may be pushed in: enough to clear any bezel, not enough to lose the screen */
#define PF_MARGIN_MAX 60

/* the four edges, in the order the editor walks them */
enum { PF_EDGE_TOP = 0, PF_EDGE_RIGHT, PF_EDGE_BOTTOM, PF_EDGE_LEFT };

#define PF_MENU_MAX 12   /* the meat list: nine meats, Custom, Back */
#define PF_NAV_MAX 5
#define PF_BT_MAX 6

/* makes of Bluetooth probe the panel can pair, matching the probe modules in the manifest */
typedef struct { const char *module, *label; } pf_bt_kind;
extern const pf_bt_kind PF_BT_KINDS[];
extern const int PF_BT_KIND_COUNT;

typedef struct { char name[24], addr[20]; int bars; bool mine; } pf_bt_found;

#define PF_BT_DEV_MAX 8
typedef struct { char device[32], name[32]; bool enabled; } pf_bt_device;
/* The paired Bluetooth probes, read out of the status (one row per physical probe). */
int pf_bt_devices(const cJSON *status, pf_bt_device *out, int max);

typedef struct { pf_screen screen; int list; int index; } pf_nav;

/* What is on the probe and how done, the same table the phone's picker offers. `to_f` is where the
 * meat is done and `carry_f` how far it climbs once it is off the heat; the target set is the
 * difference, so the alarm fires while there is still time to pull it. */
typedef struct { const char *name; int to_f, carry_f; const char *step[3]; int step_f[3]; } pf_doneness;   /* steps: flip, spritz, wrap... in F */
typedef struct { const char *name; const pf_doneness *d; int n; } pf_meat;
extern const pf_meat PF_MEATS[];
extern const int PF_MEAT_COUNT;

/* The main screen's focus: what a turn of the knob has picked out, and what a press acts on.
 * The banner's mode, its timer corner (while one runs), the set point, the hopper, then the food
 * probes left to right. */
enum { PF_FOCUS_NONE = -1, PF_FOCUS_MODE = 0, PF_FOCUS_TIMER, PF_FOCUS_SETPOINT, PF_FOCUS_HOPPER, PF_FOCUS_PROBE0 };
/* The next focus in that direction, skipping what the status has nothing for; from none, the set point. */
int pf_main_focus_step(const cJSON *status, int cur, int dir);
/* The status index of the food probe shown in card `card` (0..2), or -1. */
int pf_main_probe_index(const cJSON *status, int card);

typedef struct {
	pf_nav stack[PF_NAV_MAX];
	int depth;                 /* 0 = main screen; stack[depth - 1] is what is showing */

	/* temperature selector: three stops that clamp at the ends rather than wrapping */
	double temp_value;
	int temp_focus;            /* 0 = the value, 1 = the action button, 2 = Back */
	bool temp_editing;         /* a press on the value toggles this */
	pf_action temp_action;     /* what the action button does once pressed */
	int temp_kind;             /* 0 = a temperature, 1 = minutes (the timer), 2 = Smoke's P-mode */
	char temp_title[20], temp_button[12], temp_probe[32];

	/* confirmation */
	char confirm_text[44], confirm_yes[12];
	pf_action confirm_action;
	int confirm_focus;         /* 0 = cancel (left), 1 = confirm (right) */
	bool confirm_danger;

	int manual_focus;          /* Monitor control: 0 auger, 1 fan, 2 igniter, 3 exit */

	/* Margin editor. The values are the live ones, so turning the knob moves the picture under the
	 * bezel straight away and the setting is judged by eye rather than by number. */
	int margin[4];             /* top, right, bottom, left, matching PF_EDGE_* */
	int margin_focus;          /* 0..3 an edge, 4 = Save, 5 = Back */
	bool margin_editing;       /* a press on an edge toggles this */
	bool margin_dirty;

	/* Bluetooth pairing */
	char bt_kind[16], bt_label[24];
	pf_bt_found bt[PF_BT_MAX];
	int bt_n;
	bool bt_scanning;

	/* main screen: what the knob has picked out, and until when it stays picked out */
	int main_focus;
	double main_focus_until;
	/* the probe and meat a target is being chosen for (status index, PF_MEATS index) */
	int probe_idx, meat_idx;

	/* the word the daemon wants flashed until somebody acknowledges it: TIMER, a probe's name */
	char attention[64];   /* "ACTION\nProbe 165°", or one word */

	char message[64];
	double message_until;
	bool blink;                /* toggled each tick: drives the done / over-target flash */
	bool splash;               /* the start-up screen: name, version, how to connect */
} pf_ui_state;

/* Build the list for whatever PF_SCR_LIST screen is showing. Returns the row count. */
int pf_menu_build(const cJSON *status, const pf_ui_state *ui, pf_menu_item *out, int max);

/* Navigation. push() opens a screen, pop() goes back one, reset() returns to the main screen. */
void pf_nav_reset(pf_ui_state *ui);
void pf_nav_push(pf_ui_state *ui, pf_screen screen, int list);
void pf_nav_pop(pf_ui_state *ui);
pf_screen pf_nav_screen(const pf_ui_state *ui);
pf_nav *pf_nav_top(pf_ui_state *ui);

/* Render whatever the state says onto g using the latest status JSON (may be NULL). */
void pf_screens_render(pf_gfx *g, const cJSON *status, const pf_ui_state *ui);
