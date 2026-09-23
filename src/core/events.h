#pragma once
/* Event bus: alerts and notable state changes. Every event is logged, stored in the events table,
 * kept in a small ring for the UI, and handed to registered sinks (MQTT, webhook, push, ...).
 *
 * An event carries how loudly it should be announced (`crit`) and which sinks may take it
 * (`sinks`), so a notification rule can say "this one is critical, send it to Pushover only"
 * without every sink having to infer that from the event code. */
#include <cJSON.h>
#include <stddef.h>

/* How loudly to announce it. Sinks map this onto whatever their service supports. */
typedef enum { PF_CRIT_INFO = 0, PF_CRIT_NORMAL, PF_CRIT_HIGH, PF_CRIT_CRITICAL } pf_criticality;

/* Which sinks may deliver an event. */
#define PF_SINK_APP      (1u << 0)   /* the web app's banners and notification centre */
#define PF_SINK_PUSHOVER (1u << 1)
#define PF_SINK_NTFY     (1u << 2)
#define PF_SINK_MQTT     (1u << 3)
#define PF_SINK_WEBHOOK  (1u << 4)
/* The browser's own notifications, delivered by the push service even with the app closed. */
#define PF_SINK_WEBPUSH  (1u << 5)
#define PF_SINK_ALL      0xFFFFFFFFu

typedef struct {
	double ts;
	const char *code, *title, *body;
	int crit;          /* pf_criticality */
	unsigned sinks;    /* PF_SINK_* mask */
} pf_event;

typedef void (*pf_event_sink_fn)(const pf_event *e, void *ctx);

void pf_events_init(void);
/* Emit an alert. `title`/`body` are user-facing; code is a stable identifier. Criticality is
 * inferred from the code (E0x = critical, W0x = high) and every sink is offered the event. */
void pf_events_emit(const char *code, const char *title, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* Emit with an explicit criticality and sink mask, as the notification rules do. */
void pf_events_emit_ex(const char *code, int crit, unsigned sinks, const char *title, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
int  pf_events_add_sink(pf_event_sink_fn fn, void *ctx);
/* Ring of recent alerts (newest last) for the UI; also the generation counter for WS push. */
cJSON *pf_events_recent_json(int max);
unsigned pf_events_generation(void);
