#pragma once
/* BlueZ over the system D-Bus (sd-bus). One manager thread runs discovery, connections, GATT
 * notifications and polling for every registered device; drivers get callbacks on that thread. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pf_ble_dev pf_ble_dev;

typedef struct {
	const char *name_match;     /* case-insensitive substring of the advertised name, or NULL */
	const char *name_exclude;   /* skip names containing this (e.g. "MEATER+"), or NULL */
	const char *address;        /* "AA:BB:CC:DD:EE:FF" to pin a device, or NULL/"" for first match */
	const char *notify_uuids[8];/* characteristics to StartNotify after connecting (NULL-terminated) */
	const char *poll_uuids[4];  /* characteristics to ReadValue every poll_ms (NULL-terminated) */
	int poll_ms;
	/* Passive device: never connected; its manufacturer-specific advertisement data (company id
	 * manufacturer_id) is read every poll_ms while discovery runs and delivered to on_value with
	 * uuid "mfr". Matched by address, else by name_match, else by the manufacturer id alone. */
	bool passive;
	uint16_t manufacturer_id;
	void (*on_connected)(pf_ble_dev *d, void *ctx);
	void (*on_disconnected)(pf_ble_dev *d, void *ctx);
	/* notification or poll result */
	void (*on_value)(pf_ble_dev *d, const char *uuid, const uint8_t *data, size_t len, void *ctx);
	void *ctx;
} pf_ble_spec;

int  pf_ble_start(void);            /* start the manager thread (no-op if already running) */
void pf_ble_stop(void);
bool pf_ble_available(void);        /* adapter found and powered */

pf_ble_dev *pf_ble_register(const pf_ble_spec *spec);
void pf_ble_unregister(pf_ble_dev *d);
/* Queue a GATT write (executed on the manager thread, in order). */
int  pf_ble_write(pf_ble_dev *d, const char *uuid, const uint8_t *data, size_t len, bool with_response);
bool pf_ble_connected(const pf_ble_dev *d);
bool pf_ble_has_service(const pf_ble_dev *d, const char *uuid_prefix);   /* e.g. "a75cc7fc" */
int  pf_ble_battery(const pf_ble_dev *d);   /* 0..100 or -1 */
int  pf_ble_rssi(const pf_ble_dev *d);      /* dBm (negative), 0 = unknown; live for connected probes when CAP_NET_RAW is granted */
const char *pf_ble_address(const pf_ble_dev *d);
const char *pf_ble_name(const pf_ble_dev *d);

/* Discovery for the UI: runs discovery for ~seconds and returns [{name,address,rssi}]. */
struct cJSON *pf_ble_scan_json(int seconds);

/* Helpers for 16-bit UUIDs: "fff4" -> "0000fff4-0000-1000-8000-00805f9b34fb" */
const char *pf_ble_uuid16(const char *short4, char out[40]);
