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
} pf_probe_reading;

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
void pf_probes_snapshot(pf_sensors *out);
/* Per-device status for the UI: [{"device":..,"module":..,"status":{...}}] */
cJSON *pf_probes_device_status(void);
/* Index by label, or -1. */
int  pf_probes_find(const pf_sensors *s, const char *label);
