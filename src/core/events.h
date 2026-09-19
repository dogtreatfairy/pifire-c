#pragma once
/* Event bus: alerts and notable state changes. Every event is logged, stored in the events table,
 * kept in a small ring for the UI, and handed to registered sinks (MQTT, webhook, ...). */
#include <cJSON.h>
#include <stddef.h>

typedef void (*pf_event_sink_fn)(const char *code, const char *title, const char *body, void *ctx);

void pf_events_init(void);
/* Emit an alert. `title`/`body` are user-facing; code is a stable identifier. */
void pf_events_emit(const char *code, const char *title, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  pf_events_add_sink(pf_event_sink_fn fn, void *ctx);
/* Ring of recent alerts (newest last) for the UI; also the generation counter for WS push. */
cJSON *pf_events_recent_json(int max);
unsigned pf_events_generation(void);
