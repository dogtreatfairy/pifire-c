#define _GNU_SOURCE
#include "core/cmdq.h"
#include "core/control.h"
#include "core/db.h"
#include "core/env.h"
#include "core/events.h"
#include "core/history.h"
#include "display/registry.h"
#include "features/cookfile.h"
#include "features/update.h"
#include "features/learning.h"
#include "features/mqtt.h"
#include "features/pellets.h"
#include "features/recipe.h"
#include "features/webhook.h"
#include "core/log.h"
#include "core/outputs.h"
#include "core/sdnotify.h"
#include "core/settings.h"
#include "core/threads.h"
#include "core/util.h"
#include "controllers/registry.h"
#include "pifire/common.h"
#include "platform/rpi.h"
#include "platform/sim.h"
#include "probes/probes.h"
#include "probes/registry.h"
#include "net/netmgr.h"
#include "web/server.h"
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "main"
#define PLUGIN_DIR "/usr/lib/pifire"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
	        "usage: %s [options]\n"
	        "  -c, --config PATH   settings file (default %s)\n"
	        "  -d, --data DIR      data directory (default %s)\n"
	        "  -s, --sim           simulator mode: no hardware, dev paths under ./run/\n"
	        "  -x, --speed N       simulator time scale (default 1)\n"
	        "  -p, --port N        override web port\n"
	        "  -P, --plugins DIR   plugin root (default " PLUGIN_DIR ")\n"
	        "  -l, --log LEVEL     debug|info|warn|error\n"
	        "  -v, --version\n",
	        argv0, PF_DEFAULT_CONFIG, PF_DEFAULT_DATA_DIR);
}

int main(int argc, char **argv)
{
	const char *config = PF_DEFAULT_CONFIG;
	const char *data_dir = PF_DEFAULT_DATA_DIR;
	const char *plugin_dir = PLUGIN_DIR;
	bool sim = false;
	double speed = 1;
	int port_override = 0;
	pf_log_level level = PF_LOG_INFO;

	static const struct option opts[] = {
		{ "config", required_argument, NULL, 'c' }, { "data", required_argument, NULL, 'd' },
		{ "sim", no_argument, NULL, 's' },           { "speed", required_argument, NULL, 'x' },
		{ "port", required_argument, NULL, 'p' },    { "log", required_argument, NULL, 'l' },
		{ "plugins", required_argument, NULL, 'P' },
		{ "version", no_argument, NULL, 'v' },       { "help", no_argument, NULL, 'h' },  { 0, 0, 0, 0 }
	};
	int c;
	while ((c = getopt_long(argc, argv, "c:d:sx:p:l:P:vh", opts, NULL)) != -1) {
		switch (c) {
		case 'c': config = optarg; break;
		case 'd': data_dir = optarg; break;
		case 'P': plugin_dir = optarg; break;
		case 's': sim = true; break;
		case 'x': speed = atof(optarg); break;
		case 'p': port_override = atoi(optarg); break;
		case 'l': { int l = pf_log_level_from_name(optarg); if (l < 0) { usage(argv[0]); return 2; } level = (pf_log_level)l; break; }
		case 'v': printf("pifired %s\n", PF_VERSION); return 0;
		default: usage(argv[0]); return c == 'h' ? 0 : 2;
		}
	}
	if (sim) {
		if (!strcmp(config, PF_DEFAULT_CONFIG)) config = "run/settings.json";
		if (!strcmp(data_dir, PF_DEFAULT_DATA_DIR)) data_dir = "run";
	}

	pf_log_init(level);
	LOGI(TAG, "pifired %s starting (%s)", PF_VERSION, sim ? "simulator" : "hardware");
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (pf_mkdir_p(data_dir)) { LOGE(TAG, "cannot create data dir %s", data_dir); return 1; }
	if (pf_settings_init(config)) return 1;
	if (pf_set_bool("globals.debug_mode", false) && level > PF_LOG_DEBUG) pf_log_set_level(PF_LOG_DEBUG);
	if (sim) pf_settings_force_sim();
	if (port_override) pf_set_put_num("web.port", port_override);

	char dbpath[600], marker[600];
	snprintf(dbpath, sizeof dbpath, "%s/pifire.db", data_dir);
	snprintf(marker, sizeof marker, "%s/.running", data_dir);
	if (pf_db_open(dbpath)) return 1;
	bool unclean = pf_file_exists(marker);
	pf_write_file_atomic(marker, "1", 1);
	pf_db_event(PF_LVL_INFO, "SYS_START", sim ? "pifired started (simulator)" : "pifired started");

	/* plugins and hardware */
	char pdir[600];
	snprintf(pdir, sizeof pdir, "%s/controllers", plugin_dir);
	pf_controllers_init(pdir);
	snprintf(pdir, sizeof pdir, "%s/probes", plugin_dir);
	pf_probe_drivers_init(pdir);

	char sys_type[16];
	pf_set_str("platform.system_type", sys_type, sizeof sys_type, "sim");
	const pf_platform_ops *pops = (sim || !strcmp(sys_type, "sim")) ? pf_platform_sim() : pf_platform_rpi();
	cJSON *pj = pf_set_dup("platform");
	char *pjs = cJSON_PrintUnformatted(pj);
	cJSON_Delete(pj);
	pf_env penv;
	pf_env_init(&penv, "platform");
	void *pinst = pops->create(pjs, &penv);
	free(pjs);
	if (!pinst) { LOGE(TAG, "platform '%s' failed to initialise", pops->id); return 1; }
	pf_outputs_init(pops, pinst);

	pf_cmdq_init();
	pf_events_init();
	pf_history_init();
	pf_cookfile_init(data_dir);
	pf_update_init(data_dir, sim);
	pf_pellets_init(sim);
	pf_recipes_init();
	pf_learning_init();
	pf_display_init();
	pf_probes_init();
	static pf_control ctrl;
	pf_control_init(&ctrl, sim);

	/* first sensor pass before deciding on hot-restart recovery */
	for (int i = 0; i < 12; i++) { pf_probes_poll(pf_now()); pf_sleep_ms(50); }
	pf_control_step(&ctrl, pf_now());
	pf_control_boot_check(&ctrl, unclean, pf_now());

	pf_threads_opts topts = { .sim = sim, .sim_time_scale = speed };
	if (pf_threads_start(&ctrl, &topts)) return 1;

	char bind[64];
	pf_set_str("web.bind", bind, sizeof bind, "0.0.0.0");
	if (pf_web_start(bind, pf_set_int("web.port", 80))) return 1;
	pf_netmgr_start(sim);
	pf_mqtt_init();
	pf_webhook_init();

	pf_sd_notify("READY=1\nSTATUS=running");
	LOGI(TAG, "ready (units=%s, controller=%s)", pf_settings_units() == PF_UNITS_C ? "C" : "F", ctrl.cfg.controller_id);

	while (!g_stop) {
		pf_sleep_ms(200);
		if (pf_threads_power_off_requested()) { LOGI(TAG, "auto power off requested"); break; }
	}

	pf_sd_notify("STOPPING=1");
	LOGI(TAG, "shutting down");
	pf_webhook_shutdown();
	pf_mqtt_shutdown();
	pf_netmgr_stop();
	pf_web_stop();
	pf_threads_stop();
	pf_control_shutdown(&ctrl);
	pf_probes_shutdown();
	pf_pellets_shutdown();
	pf_display_shutdown();
	pf_outputs_shutdown();
	pf_db_event(PF_LVL_INFO, "SYS_STOP", "pifired stopped");
	pf_db_close();
	pf_settings_shutdown();
	unlink(marker);
	if (pf_threads_power_off_requested() && !sim) execlp("systemctl", "systemctl", "poweroff", (char *)NULL);
	return 0;
}
