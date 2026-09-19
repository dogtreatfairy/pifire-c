#pragma once
/* Local display / input plugin ABI (only "none" is implemented in v1). */
#include "pifire/common.h"

#define PF_DISPLAY_ABI 1

typedef enum { PF_KEY_NONE = 0, PF_KEY_UP, PF_KEY_DOWN, PF_KEY_ENTER, PF_KEY_LONG_ENTER } pf_key;

typedef struct pf_display_ops {
	uint32_t abi;
	const char *id;
	const char *name;
	void  *(*create)(const char *config_json, const pf_env *env);
	void   (*destroy)(void *self);
	/* Called ~2 Hz with the same JSON the web UI receives over WebSocket. */
	void   (*status)(void *self, const char *status_json);
	void   (*text)(void *self, const char *msg);
	pf_key (*poll_input)(void *self);
} pf_display_ops;
