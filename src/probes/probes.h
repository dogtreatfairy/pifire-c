#pragma once
/* The "probe complex": instantiates probe devices from settings.probe_settings.probe_map, maps
 * ports to labelled probes, converts raw samples (Steinhart-Hart, resistor divider), filters, and
 * publishes a snapshot for the control thread and the UI. */
#include "pifire/common.h"
#include "pifire/probe.h"
#include <cJSON.h>

typedef struct {
	char label[PF_LABEL_LEN];
	char name[PF_NAME_LEN];
	char device[PF_LABEL_LEN];
	char port[16];
	pf_probe_role role;
	bool enabled;
	bool ambient;      /* Aux probe designated as the ambient reference */
	bool home;         /* show on the Home screen / panel (probe_info.show_on_home) */
	bool valid;        /* last reading usable */
	double temp_c;     /* filtered, NAN if invalid */
	double raw_c;      /* unfiltered */
	double ohms;       /* 0 for non-thermistor */
	double last_valid_t; /* monotonic; 0 = never */
	double target_c;   /* notify target, 0 = none (filled by notify engine) */
	bool wireless;     /* Bluetooth device */
	int rssi;          /* dBm, 0 unknown (wireless only) */
	int battery;       /* %, -1 unknown (wireless only) */
	int companion;     /* index of this probe's ambient sibling on the same wireless device, or -1 */
	bool is_companion; /* this reading is shown inside its sibling's card, not as its own */
	/* Is this probe part of what is being cooked?
	 *
	 * A grill can have nine probes configured and two in the meat. The other seven are switched on,
	 * sitting in a drawer, reading nothing, and there is no sense telling anyone that they are
	 * offline or that they have reached a target of zero. A probe counts as in use once it has been
	 * given a target or has produced a reading while the grill was cooking, and stays in use until
	 * the cook ends, so going quiet part way through is still worth reporting. */
	bool in_use;
} pf_probe_reading;

/* 0..4 bars from an RSSI in dBm (0 = unknown / no link) */
static inline int pf_signal_bars(int rssi_dbm)
{
	if (rssi_dbm == 0) return 0;
	return rssi_dbm >= -60 ? 4 : rssi_dbm >= -70 ? 3 : rssi_dbm >= -80 ? 2 : 1;
}

typedef struct {
	double t;                 /* monotonic time of snapshot */
	int n;
	int primary;              /* index into p[] or -1 */
	pf_probe_reading p[PF_MAX_PROBES];
} pf_sensors;

int  pf_probes_init(void);        /* (re)build from current settings */
void pf_probes_shutdown(void);
/* Poll every device once (blocking on I/O), update the snapshot. Called from the sensor thread. */
void pf_probes_poll(double now);
/* Tell the probe layer whether a cook is under way, so it can work out which probes are part of it.
 * Passing false ends the cook and clears every probe's in-use flag. */
void pf_probes_set_cooking(bool cooking);
/* The cook said which probes are in the food. From here until the cook ends only those count as in
 * use (plus any probe later given a target, which is a deliberate act); a probe merely reading
 * while the grill is lit no longer does. An empty list means none of them. */
void pf_probes_set_in_use(const char *labels_csv);
/* The labels currently in use, comma separated, for the warm-restart snapshot. */
void pf_probes_in_use_csv(char *out, size_t n);
void pf_probes_snapshot(pf_sensors *out);
/* Per-device status for the UI: [{"device":..,"module":..,"status":{...}}] */
cJSON *pf_probes_device_status(void);
/* Index by label, or -1. */
int  pf_probes_find(const pf_sensors *s, const char *label);
