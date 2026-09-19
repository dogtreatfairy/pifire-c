#pragma once
/* Thin wrapper over NetworkManager's nmcli (fork/exec, no shell). In simulator mode every call
 * returns canned data so the UI can be exercised without Wi-Fi hardware. */
#include <cJSON.h>
#include <stdbool.h>

void pf_wifi_init(bool sim);
const char *pf_wifi_iface(void);                    /* e.g. "wlan0" */
/* Networks in range: [{"ssid","signal","security","active"}], strongest first, deduplicated. */
cJSON *pf_wifi_scan(bool rescan);
/* {"connected":bool,"ssid":..,"signal":..,"ip":..,"iface":..} */
cJSON *pf_wifi_status(void);
/* Blocking (up to ~40 s). 0 on success; on failure the auto-created profile is removed. */
int  pf_wifi_connect(const char *ssid, const char *psk, char *err, size_t errn);
int  pf_wifi_forget(const char *ssid);
cJSON *pf_wifi_saved(void);                          /* ["ssid", ...] */

int  pf_hotspot_start(const char *ssid, const char *psk);
int  pf_hotspot_stop(void);
bool pf_hotspot_is_up(void);
/* "PiFire-XXXX" from the Wi-Fi MAC. */
void pf_hotspot_default_ssid(char *out, size_t n);
