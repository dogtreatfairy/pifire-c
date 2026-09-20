#pragma once
/* Remote access through Tailscale: the grill joins the user's tailnet and the web app is reachable
 * from anywhere at http(s)://<hostname>.<tailnet>.ts.net. Everything privileged goes through the
 * pifire-tailscale helper (sudoers). */
#include <cJSON.h>
#include <stddef.h>

/* {installed, state, auth_url, ips[], dns_name, hostname, online, version, https, busy, last_action, last_ok, last_output} */
cJSON *pf_tailscale_status_json(void);
/* Start an action on a worker thread: "install" | "up" | "down" | "logout" | "serve" | "unserve".
 * Returns 0 when started, -1 with err when refused (busy, unknown verb, simulator). */
int pf_tailscale_action(const char *verb, char *err, size_t n);
