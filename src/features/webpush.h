#pragma once
/* Web Push (RFC 8030 / 8291 / 8292): notifications that reach a phone with the app closed.
 *
 * Everything else the daemon can do to reach a phone depends on something of ours running. A
 * banner needs the page open; showNotification() from the page needs the app alive, which on iOS
 * means a few seconds after you switch away and no longer. Web Push is the only path that does
 * not: the daemon posts an encrypted message to the push service named in the subscription --
 * Apple's for an iPhone -- and the phone wakes the service worker to show it, whether or not
 * PiFire has been opened for a week.
 *
 * Only outbound HTTPS is needed. The grill is never contacted from outside; it is the one doing
 * the contacting, so this works from behind a home router with nothing forwarded. Serving the app
 * over https is still required, because a browser will not hand out a push subscription to an
 * insecure page, which Tailscale already provides.
 *
 * The payload is encrypted end to end for the subscription that will receive it (RFC 8291): the
 * push service forwards bytes it cannot read. The VAPID key pair (RFC 8292) identifies this grill
 * to the push service so nobody else can send to a subscription obtained from it. */
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>

void pf_webpush_init(void);
void pf_webpush_shutdown(void);

/* True when the daemon was built with the crypto it needs. Everything below is safe to call
 * either way; without it they do nothing and say so. */
bool pf_webpush_available(void);

/* The VAPID public key, base64url, as the browser wants it for applicationServerKey. Generated
 * and stored on first use, and stable thereafter: changing it invalidates every subscription. */
const char *pf_webpush_public_key(void);

/* Record a subscription from a browser. `sub` is the PushSubscription as JSON: endpoint, and
 * keys.p256dh / keys.auth. Re-subscribing with the same endpoint replaces the old record rather
 * than collecting duplicates, which is what a browser does when its subscription is refreshed. */
int pf_webpush_subscribe(const cJSON *sub, char *err, size_t n);
int pf_webpush_unsubscribe(const char *endpoint);
int pf_webpush_count(void);
cJSON *pf_webpush_json(void);   /* what the app shows: how many devices, and this grill's key */

/* Send to every subscription. A push service that answers 404 or 410 is telling us the
 * subscription is dead, and it is dropped rather than retried for ever. */
void pf_webpush_send(const char *title, const char *body, const char *code, int crit);

/* Is this a contact a push service will accept: a mailto: with a real domain, or an https: URL.
 * Apple refuses anything else with 403 and says nothing about which claim it disliked. */
bool pf_webpush_contact_ok(const char *contact);

/* Seal a message with keys and a salt supplied rather than generated, so the encryption can be
 * checked against RFC 8291's published example instead of being taken on trust. Test use only. */
int pf_webpush_seal_for_test(const char *p256dh_b64, const char *auth_b64, const char *as_priv_b64,
                             const char *salt_b64, const char *plaintext, char *out_b64, size_t cap);
