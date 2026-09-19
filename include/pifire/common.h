#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PF_VERSION
#define PF_VERSION "0.0.0"
#endif

#define PF_MAX_PROBES   16
#define PF_MAX_DEVICES  8
#define PF_LABEL_LEN    32
#define PF_NAME_LEN     64

typedef enum { PF_UNITS_F = 0, PF_UNITS_C = 1 } pf_units;

static inline double pf_c_to_f(double c) { return c * 9.0 / 5.0 + 32.0; }
static inline double pf_f_to_c(double f) { return (f - 32.0) * 5.0 / 9.0; }
static inline double pf_to_c(double v, pf_units u) { return u == PF_UNITS_C ? v : pf_f_to_c(v); }
static inline double pf_from_c(double c, pf_units u) { return u == PF_UNITS_C ? c : pf_c_to_f(c); }
/* temperature *differences* (no 32 offset) */
static inline double pf_delta_to_c(double d, pf_units u) { return u == PF_UNITS_C ? d : d * 5.0 / 9.0; }
static inline double pf_delta_from_c(double d, pf_units u) { return u == PF_UNITS_C ? d : d * 9.0 / 5.0; }

typedef enum {
	PF_MODE_STOP = 0,
	PF_MODE_MONITOR,
	PF_MODE_PRIME,
	PF_MODE_STARTUP,
	PF_MODE_REIGNITE,
	PF_MODE_SMOKE,
	PF_MODE_HOLD,
	PF_MODE_SHUTDOWN,
	PF_MODE_MANUAL,
	PF_MODE_ERROR,
	PF_MODE_COUNT
} pf_mode;

const char *pf_mode_name(pf_mode m);
int pf_mode_from_name(const char *s); /* -1 if unknown */

typedef enum { PF_OUT_POWER = 0, PF_OUT_FAN, PF_OUT_AUGER, PF_OUT_IGNITER, PF_OUT_COUNT } pf_output;
const char *pf_output_name(pf_output o);

typedef enum { PF_PROBE_PRIMARY = 0, PF_PROBE_FOOD, PF_PROBE_AUX } pf_probe_role;

typedef enum { PF_IN_SELECTOR = 0, PF_IN_SHUTDOWN, PF_IN_COUNT } pf_input;

/* Services the daemon hands to every plugin. Plugins never include daemon headers. */
typedef struct pf_env pf_env;
struct pf_env {
	void (*log)(int level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
	/* Persistent key/value (JSON text), namespaced per plugin instance by the daemon.
	 * kv_get returns 0 found, 1 missing, <0 error. */
	int (*kv_get)(const pf_env *env, const char *key, char *json_out, size_t n);
	int (*kv_put)(const pf_env *env, const char *key, const char *json);
	const char *ns;   /* namespace, e.g. "controller.pid" */
	void *ctx;        /* daemon-private */
};

enum { PF_LVL_DEBUG = 0, PF_LVL_INFO, PF_LVL_WARN, PF_LVL_ERROR };
