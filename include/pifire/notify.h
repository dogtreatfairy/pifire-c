#pragma once
/* Notification sink plugin ABI (MQTT, webhook, ...). */
#include "pifire/common.h"

#define PF_NOTIFY_ABI 1

typedef struct pf_notify_ops {
	uint32_t abi;
	const char *id;
	const char *name;
	void *(*create)(const char *config_json, const pf_env *env);
	void  (*destroy)(void *self);
	/* Discrete event: code like "Probe_Temp_Achieved", human title/body, and the full status JSON. */
	int   (*event)(void *self, const char *code, const char *title, const char *body, const char *status_json);
	/* Periodic telemetry (every settings.notify.<id>.update_sec). */
	int   (*telemetry)(void *self, const char *status_json);
	/* Called from the services thread ~1 Hz for housekeeping (reconnects, retries). */
	void  (*tick)(void *self);
} pf_notify_ops;
