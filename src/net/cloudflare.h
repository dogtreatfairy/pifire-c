#pragma once
/* Remote access through a Cloudflare Tunnel: cloudflared on the grill dials out to Cloudflare, and
 * the web app is reachable at a hostname on the user's own domain with Cloudflare's certificate.
 * Unlike a tailnet, that hostname is on the public internet, and the grill's API has no login of its
 * own -- so the grill does not trust the tunnel. Every request that arrives through Cloudflare must
 * carry a Cloudflare Access token that the grill verifies itself (signature, issuer, audience,
 * expiry); one without is refused, whatever the dashboard says. Everything privileged goes through
 * the pifire-cloudflare helper (sudoers). */
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>

/* {installed, state, version, token, enabled, active, online, connections, team, aud, hostname, verify,
 *  busy, last_action, last_ok, last_output} */
cJSON *pf_cloudflare_status_json(void);
/* Start an action on a worker thread: "install" | "start" | "stop" | "forget". `token` is only used
 * by "start" (NULL or "" reuses the stored one). Returns 0 when started, -1 with err when refused. */
int pf_cloudflare_action(const char *verb, const char *token, char *err, size_t n);
/* Cached summary for the status stream, refreshed in the background at most every 15 s. */
void pf_cloudflare_brief(bool *configured, bool *online, char *name, size_t n);

/* Whether a request that came through Cloudflare may proceed. `jwt` is its Cf-Access-Jwt-Assertion
 * header (may be NULL), `host` its Host header. Returns 0 when the token is valid for this grill's
 * Access application, -1 with a reason otherwise. */
int pf_cloudflare_check(const char *jwt, const char *host, char *why, size_t n);
/* For tests: verify `jwt` against the PEM certificate or public key `pem` instead of the team's
 * published keys, at time `now`. */
int pf_cloudflare_check_with_pem(const char *jwt, const char *pem, const char *team, const char *aud, double now, char *why, size_t n);
