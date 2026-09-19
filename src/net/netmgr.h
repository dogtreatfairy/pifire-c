#pragma once
/* Network state machine (own thread): boot wait -> online, or hotspot for first-time setup;
 * try-then-fallback Wi-Fi joins requested from the UI. */
#include <cJSON.h>
#include <stdbool.h>

typedef enum { PF_NET_BOOT = 0, PF_NET_ONLINE, PF_NET_HOTSPOT, PF_NET_CONNECTING, PF_NET_OFFLINE } pf_net_state;

int  pf_netmgr_start(bool sim);
void pf_netmgr_stop(void);
/* {"state","ssid","ip","signal","hotspot":{"ssid","password","active"},"last_error"} */
cJSON *pf_netmgr_status(void);
/* Queue a join; result is visible in status().last_error / state. 0 queued, -1 busy. */
int  pf_netmgr_connect(const char *ssid, const char *psk);
int  pf_netmgr_hotspot(bool on);
