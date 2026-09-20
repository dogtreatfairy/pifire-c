/* Learning across cooks: observations, feed-forward fit across ambients, repeat-cook improvement,
 * relay autotune on the simulated grill. */
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
#include "features/learning.h"
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
	snprintf(cfg_path, sizeof cfg_path, "/tmp/pf_learn_%d.json", (int)getpid());
	snprintf(db_path, sizeof db_path, "/tmp/pf_learn_%d.db", (int)getpid());
	unlink(cfg_path); unlink(db_path);
	pf_settings_init(cfg_path);
	pf_db_open(db_path);
	pf_controllers_init(NULL);
	pf_probe_drivers_init(NULL);
	pf_events_init();
	pf_learning_init();
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

/* one cook: returns integrated |error| (C*s) over the first `score_min` minutes of HOLD */
static double cook(const char *controller, double ambient_c, double minutes, double score_min)
{
	char patch[96];
	snprintf(patch, sizeof patch, "{\"selected\":\"%s\"}", controller);
	pf_settings_patch("controller", patch, NULL, 0);
	if (ctrl.cinst) pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	pf_sim_reset(ambient_c);
	tick(5);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	while (ctrl.mode != PF_MODE_HOLD) tick(1);
	double sp = pf_f_to_c(225), iae = 0;
	for (double t = 0; t < minutes * 60; t += 5) {
		tick(5);
		if (t < score_min * 60) iae += fabs(ctrl.pit_c - sp) * 5;
	}
	printf("  cook end: pit %.1f C u=%.2f tuning [%s]\n", ctrl.pit_c, ctrl.u_applied, ctrl.dbg.note);
	pf_cmd_simple(PF_CMD_STOP);
	tick(2);
	return iae;
}

static void test_observations_and_fit_across_ambients(void)
{
	cook("adaptive", -1.0, 60, 30);           /* 30 F day */
	pf_ff_fit f1 = pf_learning_fit();
	printf("after cold cook: n=%d a=%.3f b=%.4f\n", f1.n, f1.a, f1.b);
	TEST_ASSERT_TRUE(f1.n >= 3);
	cook("adaptive", 38.0, 60, 30);           /* 100 F day */
	pf_ff_fit f2 = pf_learning_fit();
	printf("after hot cook:  n=%d a=%.3f b=%.4f rms=%.3f\n", f2.n, f2.a, f2.b, f2.rms);
	TEST_ASSERT_TRUE(f2.n > f1.n);
	TEST_ASSERT_TRUE(f2.b > 0);
	int n;
	double cold = pf_learning_uff(pf_f_to_c(225), -1.0, 0.1, 0.9, &n), hot = pf_learning_uff(pf_f_to_c(225), 38.0, 0.1, 0.9, &n);
	printf("u_ff cold %.3f hot %.3f\n", cold, hot);
	TEST_ASSERT_TRUE(cold > hot);
	TEST_ASSERT_TRUE(cold < 0.9 && hot > 0.05);
}

static void test_repeat_cook_not_worse(void)
{
	double first = cook("adaptive", 10.0, 40, 30);
	double second = cook("adaptive", 10.0, 40, 30);
	double third = cook("adaptive", 10.0, 40, 30);
	printf("IAE first %.0f second %.0f third %.0f\n", first, second, third);
	TEST_ASSERT_TRUE(third <= first * 1.10);
	pf_fopdt p = pf_learning_fopdt();
	printf("plant: valid=%d K=%.1f tau=%.0f theta=%.0f\n", p.valid, p.K, p.tau, p.theta);
	TEST_ASSERT_TRUE(p.valid);
	TEST_ASSERT_TRUE(p.tau > 30 && p.tau < 3600);
}

static void test_autotune(void)
{
	pf_settings_patch("controller", "{\"selected\":\"pid\"}", NULL, 0);
	pf_control_init(&ctrl, true);
	pf_sim_reset(18.0);
	tick(5);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 40 * 60);
	TEST_ASSERT_TRUE(ctrl.target_reached);
	pf_cmd_simple(PF_CMD_AUTOTUNE_START);
	tick(1);
	TEST_ASSERT_TRUE(ctrl.autotune.active);
	double t = 0;
	while (ctrl.autotune.active && t < 60 * 60) { tick(10); t += 10; }
	pf_autotune_result r = pf_learning_autotune();
	printf("autotune: valid=%d Ku=%.3f Pu=%.0f amp=%.1f C -> PB %.1f C Ti %.0f Td %.0f (took %.0f min)\n", r.valid, r.Ku, r.Pu, r.amplitude_c, r.PB_c, r.Ti, r.Td, t / 60);
	TEST_ASSERT_FALSE(ctrl.autotune.active);
	TEST_ASSERT_TRUE(r.valid);
	TEST_ASSERT_TRUE(r.Pu > 60 && r.Pu < 1800);
	TEST_ASSERT_TRUE(r.PB_c > 0 && r.Ti > 0);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	/* apply and keep holding */
	pf_cmd_simple(PF_CMD_TUNING_APPLY);
	tick(20 * 60);
	TEST_ASSERT_DOUBLE_WITHIN(10.0, pf_f_to_c(225), ctrl.pit_c);
}

int main(void)
{
	pf_log_init(PF_LOG_ERROR);
	UNITY_BEGIN();
	RUN_TEST(test_observations_and_fit_across_ambients);
	RUN_TEST(test_repeat_cook_not_worse);
	RUN_TEST(test_autotune);
	return UNITY_END();
}
