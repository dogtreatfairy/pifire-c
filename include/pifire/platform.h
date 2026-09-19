#pragma once
/* PiFire platform (relay/fan/input) plugin ABI. */
#include "pifire/common.h"

#define PF_PLATFORM_ABI 1

typedef struct pf_platform_ops {
	uint32_t abi;
	const char *id;   /* "rpi", "sim" */
	const char *name;

	/* platform_json = settings.platform (pins, triggerlevel, dc_fan, ...) */
	void *(*create)(const char *platform_json, const pf_env *env);
	void  (*destroy)(void *self);
	int   (*set_output)(void *self, pf_output o, bool on);
	/* DC fan speed 0..100 (%). Ignored by AC-fan platforms. */
	int   (*set_fan_pct)(void *self, int pct);
	int   (*set_pwm_frequency)(void *self, int hz);
	/* Physical inputs; returns false when not configured. */
	bool  (*read_input)(void *self, pf_input in);
	void  (*all_off)(void *self);
	int   (*status_json)(void *self, char *out, size_t n);
} pf_platform_ops;
