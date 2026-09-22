#define _GNU_SOURCE
#include "core/threads.h"
#include "core/db.h"
#include "core/history.h"
#include "core/log.h"
#include "core/outputs.h"
#include "core/sdnotify.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/util.h"
#include "display/registry.h"
#include "features/mqtt.h"
#include "features/pellets.h"
#include "features/cookfile.h"
#include "features/rules.h"
#include "features/tuner.h"
#include "features/update.h"
#include "platform/sim.h"
#include "probes/probes.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TAG "threads"
#define CONTROL_TICK_MS 100
#define STALL_S 2.0

static pf_control *g_ctrl;
static pf_threads_opts g_opts;
static atomic_bool g_run;
static _Atomic double g_last_tick;
static atomic_bool g_power_off;
static pthread_t g_tid[4];

static void *control_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-control");
	/* In simulator mode the control loop runs on a virtual clock (real time x speed) so timers,
	 * cycles and the thermal model all accelerate together. */
	double scale = (g_opts.sim && g_opts.sim_time_scale > 0) ? g_opts.sim_time_scale : 1.0;
	double base = pf_now(), last = base;
	unsigned gen = pf_settings_generation();
	while (atomic_load(&g_run)) {
		double real = pf_now();
		double now = base + (real - base) * scale;
		if (g_opts.sim) pf_sim_step(now - last);
		last = now;
		unsigned g = pf_settings_generation();
		if (g != gen) { gen = g; pf_control_reload_settings(g_ctrl); }
		pf_control_step(g_ctrl, now);
		if (g_ctrl->power_off_requested) { g_ctrl->power_off_requested = false; atomic_store(&g_power_off, true); }
		atomic_store(&g_last_tick, pf_now());
		double spent = pf_now() - real;
		if (spent < CONTROL_TICK_MS / 1000.0) pf_sleep_ms((unsigned)((CONTROL_TICK_MS / 1000.0 - spent) * 1000));
	}
	return NULL;
}

static void *sensor_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-sensors");
	while (atomic_load(&g_run)) {
		pf_probes_poll(pf_now());
		pf_sleep_ms(50);
	}
	return NULL;
}

static void *services_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-services");
	double last_flush = pf_now(), last_prune = pf_now();
	while (atomic_load(&g_run)) {
		double now = pf_now();
		pf_status st;
		pf_status_get(&st);
		bool cooking = st.mode == PF_MODE_STARTUP || st.mode == PF_MODE_REIGNITE || st.mode == PF_MODE_SMOKE || st.mode == PF_MODE_HOLD;
		pf_pellets_tick(now, g_ctrl->auger_total_on_s, cooking);
		pf_mqtt_tick(now);
		pf_update_tick(now);
		{
			cJSON *j = pf_status_to_json(&st, pf_settings_units());
			pf_rules_tick(j, now);
			/* The tuner measures the grill, so it runs on the control loop's clock. In an
			 * accelerated simulation that clock moves faster than the wall clock, and the run
			 * has to accelerate with the grill rather than sit out real minutes. */
			pf_tuner_tick(j, st.t);
			char *txt = cJSON_PrintUnformatted(j);
			cJSON_Delete(j);
			if (txt) { pf_display_tick(txt); free(txt); }
		}
		if (now - last_flush > 15) { last_flush = now; pf_history_flush(); }
		pf_cookfile_pending_run();   /* the cook file, off the control thread where it belongs */
		if (now - last_prune > 600) {
			last_prune = now;
			double hours = pf_set_num("history.retention_hours", 48);
			pf_db_history_prune(pf_wall() - hours * 3600);
		}
		pf_sleep_ms(1000);
	}
	pf_history_flush();
	return NULL;
}

static void *watchdog_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-watchdog");
	double last_pet = 0;
	while (atomic_load(&g_run)) {
		double now = pf_now();
		double age = now - atomic_load(&g_last_tick);
		if (age > STALL_S) {
			LOGE(TAG, "control thread stalled for %.1f s - forcing outputs off and aborting", age);
			pf_outputs_emergency_off(200);
			abort();
		}
		if (now - last_pet > 5) { pf_sd_notify("WATCHDOG=1"); last_pet = now; }
		pf_sleep_ms(500);
	}
	return NULL;
}

int pf_threads_start(pf_control *ctrl, const pf_threads_opts *opts)
{
	g_ctrl = ctrl;
	g_opts = *opts;
	atomic_store(&g_run, true);
	atomic_store(&g_last_tick, pf_now());
	if (pthread_create(&g_tid[0], NULL, control_thread, NULL) ||
	    pthread_create(&g_tid[1], NULL, sensor_thread, NULL) ||
	    pthread_create(&g_tid[2], NULL, services_thread, NULL) ||
	    pthread_create(&g_tid[3], NULL, watchdog_thread, NULL)) {
		LOGE(TAG, "failed to start threads");
		return -1;
	}
	return 0;
}

void pf_threads_stop(void)
{
	atomic_store(&g_run, false);
	for (int i = 0; i < 4; i++) pthread_join(g_tid[i], NULL);
}

double pf_threads_control_age(void) { return pf_now() - atomic_load(&g_last_tick); }
bool pf_threads_power_off_requested(void) { return atomic_load(&g_power_off); }
