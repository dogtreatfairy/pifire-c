#pragma once
/* Phone push notifications: Pushover and ntfy sinks fed by the event bus. Each sink has its own
 * enable switch, credentials and event categories under settings.notify.{pushover,ntfy}. */
#include <stddef.h>

void pf_push_init(void);
void pf_push_shutdown(void);
/* Send a test message through one sink now ("pushover" | "ntfy"); 0 on success, else err filled. */
int  pf_push_test(const char *sink, char *err, size_t n);
/* Which switch in the notification settings decides whether this event is sent. Exposed so the
 * mapping can be tested: an event filed under the wrong one silently never leaves the grill. */
const char *pf_push_category(const char *code);
