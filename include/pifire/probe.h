#pragma once
/* PiFire probe device plugin ABI.
 *
 * A probe device exposes named ports. Each read() fills one sample per port. ADC-style devices
 * return millivolts (the daemon applies the resistor divider and Steinhart-Hart from the probe's
 * profile); digital and Bluetooth devices return Celsius directly.
 */
#include "pifire/common.h"

#define PF_PROBE_ABI 1
#define PF_MAX_PORTS 8

typedef enum { PF_SAMPLE_INVALID = 0, PF_SAMPLE_MV, PF_SAMPLE_CELSIUS, PF_SAMPLE_OHMS } pf_sample_kind;

typedef struct {
	pf_sample_kind kind;
	double value;
} pf_probe_sample;

typedef struct pf_probe_ops {
	uint32_t abi;               /* PF_PROBE_ABI */
	const char *id;             /* "ads1x15", "max31865", "ibbq", ... matches manifest filename */
	const char *name;
	bool transient;             /* may come and go (Bluetooth, DS18B20) */
	int  poll_ms;               /* suggested poll interval */

	/* device_json is the probe_devices[] entry from settings ("device","module","ports","config"). */
	void *(*create)(const char *device_json, const pf_env *env);
	void  (*destroy)(void *self);
	/* Port names in read() order. Returns count. */
	int   (*ports)(void *self, const char **names, int max);
	/* Fill out[0..nports-1]. Returns 0, or <0 if the device is unavailable (all samples invalid). */
	int   (*read)(void *self, pf_probe_sample *out, int nports);
	/* Device status for the UI: {"connected":..,"battery":..,"hardware_id":..} */
	int   (*status_json)(void *self, char *out, size_t n);
	/* Optional: units hint for devices that display locally (iBBQ). */
	void  (*set_units)(void *self, pf_units u);
	/* Optional, wireless devices: link quality. rssi_dbm 0 = unknown, battery_pct -1 = unknown.
	 * Returns 0, or <0 when the device is not wireless / not available. */
	int   (*link)(void *self, int *rssi_dbm, int *battery_pct);
} pf_probe_ops;

typedef const pf_probe_ops *(*pf_probe_export_fn)(void);
