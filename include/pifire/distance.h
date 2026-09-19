#pragma once
/* Hopper level distance sensor plugin ABI. */
#include "pifire/common.h"

#define PF_DISTANCE_ABI 1

typedef struct pf_distance_ops {
	uint32_t abi;
	const char *id;   /* "none", "hcsr04", "vl53l0x" */
	const char *name;
	void  *(*create)(const char *config_json, const pf_env *env);
	void   (*destroy)(void *self);
	/* Distance in cm, or <0 on failure. May block up to ~100 ms. */
	double (*read_cm)(void *self);
} pf_distance_ops;
