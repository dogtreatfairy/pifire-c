/* Every built-in controller must bring the simulated grill to the set point and hold it. */
#include "core/cmdq.h"
#include "core/control.h"
#include "core/db.h"
#include "core/env.h"
#include "core/events.h"
#include "core/history.h"
#include "core/log.h"
#include "core/outputs.h"
#include "core/settings.h"
#include "controllers/registry.h"
#include "platform/sim.h"
#include "probes/probes.h"
#include "probes/registry.h"
#include "unity.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char cfg_path[256], db_path[256];
static pf_control ctrl;
static double now;

static void tick(double dt)
{
	int steps = (int)(dt / 0.1 + 0.5);
	for (int i = 0; i < steps; i++) { now += 0.1; pf_sim_step(0.1); pf_probes_poll(now); pf_control_step(&ctrl, now); }
}

void setUp(void)
{
	snprintf(cfg_path, sizeof cfg_path, "/tmp/pf_ctl_%d.json", (int)getpid());
	snprintf(db_path, sizeof db_path, "/tmp/pf_ctl_%d.db", (int)getpid());
	unlink(cfg_path); unlink(db_path);
	pf_settings_init(cfg_path);
	pf_settings_force_sim();
	pf_settings_patch("startup", "{\"smartstart\":{\"enabled\":false},\"startup_exit_temp\":0,\"start_to_mode\":{\"after_startup_mode\":\"Smoke\",\"primary_setpoint\":165}}", NULL, 0);
	pf_db_open(db_path);
	pf_controllers_init(NULL);
	pf_probe_drivers_init(NULL);
	pf_events_init();
	pf_env env; pf_env_init(&env, "platform");
	pf_outputs_init(pf_platform_sim(), pf_platform_sim()->create("{}", &env));
	pf_cmdq_init(); pf_history_init(); pf_probes_init();
	now = 1000;
}

void tearDown(void)
{
	pf_control_shutdown(&ctrl);
	pf_probes_shutdown();
	pf_outputs_shutdown();
	pf_db_close();
	pf_settings_shutdown();
	unlink(cfg_path); unlink(db_path);
	char p[300]; snprintf(p, sizeof p, "%s-wal", db_path); unlink(p); snprintf(p, sizeof p, "%s-shm", db_path); unlink(p);
}

static void run_with(const char *id)
{
	char patch[96];
	snprintf(patch, sizeof patch, "{\"selected\":\"%s\"}", id);
	TEST_ASSERT_EQUAL_INT(0, pf_settings_patch("controller", patch, NULL, 0));
	pf_control_init(&ctrl, true);
	TEST_ASSERT_EQUAL_STRING(id, ctrl.cops->id);
	pf_sim_reset(18.0);
	tick(3);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 45 * 60);
	double sp = pf_f_to_c(225);
	/* average error over the last 10 min */
	double sum = 0; int n = 0;
	for (int i = 0; i < 60; i++) { tick(10); sum += ctrl.pit_c - sp; n++; }
	printf("%-24s pit %.1f C (target %.1f) mean err %.2f  u=%.2f\n", id, ctrl.pit_c, sp, sum / n, ctrl.u_applied);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("", ctrl.safety.error_code);
	TEST_ASSERT_DOUBLE_WITHIN(10.0, sp, ctrl.pit_c);
	TEST_ASSERT_DOUBLE_WITHIN(6.0, 0.0, sum / n);
}

static void t_pid(void) { run_with("pid"); }
static void t_clamp(void) { run_with("pid_clamping"); }
static void t_clamp_pct(void) { run_with("pid_clamping_percent_pb"); }
static void t_ac(void) { run_with("pid_ac"); }
static void t_sp(void) { run_with("pid_sp"); }
static void t_par(void) { run_with("pid_parallel"); }

int main(void)
{
	pf_log_init(PF_LOG_ERROR);
	UNITY_BEGIN();
	RUN_TEST(t_pid);
	RUN_TEST(t_clamp);
	RUN_TEST(t_clamp_pct);
	RUN_TEST(t_ac);
	RUN_TEST(t_sp);
	RUN_TEST(t_par);
	return UNITY_END();
}
