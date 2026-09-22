#pragma once
#include "pifire/common.h"
#include <cJSON.h>
#include <sqlite3.h>

int pf_db_open(const char *path);
void pf_db_close(void);
/* Raw handle for feature modules (already opened in serialized/FULLMUTEX mode). */
sqlite3 *pf_db_handle(void);
int pf_db_exec(const char *sql);

/* Key/value store (JSON text) namespaced by component, e.g. ("controller.pid", "tuning"). */
int pf_db_kv_get(const char *ns, const char *key, char *out, size_t n); /* 0 found, 1 missing, <0 error */
int pf_db_kv_put(const char *ns, const char *key, const char *json);
int pf_db_kv_delete_ns(const char *ns);

/* Event log */
int pf_db_event(int level, const char *code, const char *message);
cJSON *pf_db_events_recent(int limit); /* array, newest first */

/* History. One sample row plus one row per probe. Batch inside a transaction. */
typedef struct {
	char label[PF_LABEL_LEN];
	double temp;    /* NAN when invalid */
	double target;  /* 0 = none */
	double raw;     /* the reading before the filter, NAN when invalid */
	double ohms;    /* the resistance it was computed from, 0 when not a resistive probe */
} pf_hist_probe;
typedef struct {
	double ts;      /* wall clock seconds */
	int mode;
	double setpoint, u_raw, u_applied;
	int fan_pct;
	unsigned outputs; /* bitmask of pf_output */
	/* controller detail for cook-log analysis */
	double u_ff, p, i, d, ff, ambient, cycle_s;
	unsigned flags;   /* 1 lid_open, 2 s_plus, 4 pwm_control, 8 target_reached, 16 coldstart_active, 32 sat_low, 64 sat_high */
	int pmode;
	int nprobes;
	pf_hist_probe probes[PF_MAX_PROBES];
} pf_hist_sample;

int pf_db_history_write(const pf_hist_sample *samples, int n);
/* Rows between from..to (wall secs) at ~res_s seconds resolution (0 = raw). Returns cJSON object:
 * { "t":[...], "mode":[...], "setpoint":[...], "u":[...], "probes": { "<label>": {"temp":[...],"target":[...]} } } */
cJSON *pf_db_history_query(double from, double to, int res_s);
int pf_db_history_clear(void);
int pf_db_history_prune(double older_than_ts);
