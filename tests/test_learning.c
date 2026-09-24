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
	pf_settings_force_sim();
	pf_settings_patch("startup", "{\"smartstart\":{\"enabled\":false},\"startup_exit_temp\":0,\"start_to_mode\":{\"after_startup_mode\":\"Smoke\",\"primary_setpoint\":165}}", NULL, 0);
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
	/* long enough cooks that the pit settles and the steady windows the fit needs can be logged */
	cook("adaptive", -1.0, 100, 30);          /* 30 F day */
	pf_ff_fit f1 = pf_learning_fit();
	printf("after cold cook: n=%d a=%.3f b=%.4f\n", f1.n, f1.a, f1.b);
	TEST_ASSERT_TRUE(f1.n >= 3);
	cook("adaptive", 38.0, 100, 30);          /* 100 F day */
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

/* ---------------- the tuning library as a model of the whole range ---------------- */

/* A pellet grill loses more heat the hotter it runs, so the loop it presents at 250 F is genuinely
 * a different loop from the one at 350 F: a good tune at one is not a good tune at the other. The
 * library exists so both can be true at once. The property that makes that worth having is that
 * measuring the grill somewhere new never disturbs somewhere already measured -- otherwise every
 * extra run would trade one good answer for two mediocre ones. */
static void test_a_tune_at_one_set_point_leaves_the_others_alone(void)
{
	pf_learning_clear_anchors();
	pf_autotune_result at250 = { .Ku = 0.070, .Pu = 400, .PB_c = 31.4, .Ti = 880, .Td = 63.5, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(250), &at250, 10, 0);

	pf_autotune_result at350 = { .Ku = 0.030, .Pu = 600, .PB_c = 73.3, .Ti = 1320, .Td = 95.2, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(350), &at350, 10, 0);
	pf_autotune_result at225 = { .Ku = 0.085, .Pu = 360, .PB_c = 25.9, .Ti = 792, .Td = 57.1, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(225), &at225, 10, 0);
	pf_autotune_result at180 = { .Ku = 0.100, .Pu = 300, .PB_c = 22.0, .Ti = 660, .Td = 47.6, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(180), &at180, 10, 0);

	double PB = 0, Ti = 0, Td = 0;
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(250), &PB, &Ti, &Td));
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 31.4, PB);
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 880.0, Ti);
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 63.5, Td);

	/* all four stand on their own evidence, each still a single run */
	pf_tune_anchor list[PF_TUNE_ANCHORS];
	int n = pf_learning_anchor_list(list, PF_TUNE_ANCHORS);
	TEST_ASSERT_EQUAL_INT(4, n);
	for (int i = 0; i < n; i++) TEST_ASSERT_EQUAL_INT(1, list[i].runs);
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 22.0, list[0].PB_c);     /* 180 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 73.3, list[3].PB_c);     /* 350 */
}

/* Tuning the same set point again is the one case where an entry should move, because it is more
 * evidence about the same thing -- and it moves part of the way, not all of it, so one windy
 * afternoon cannot undo a well-measured entry. */
static void test_a_repeat_run_refines_that_set_point_only(void)
{
	pf_learning_clear_anchors();
	pf_autotune_result first = { .Ku = 0.070, .Pu = 400, .PB_c = 30.0, .Ti = 880, .Td = 63.5, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(250), &first, 10, 0);
	pf_autotune_result other = { .Ku = 0.030, .Pu = 600, .PB_c = 70.0, .Ti = 1320, .Td = 95.2, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(350), &other, 10, 0);

	pf_autotune_result again = { .Ku = 0.058, .Pu = 440, .PB_c = 38.0, .Ti = 968, .Td = 69.8, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(250) + 1.0, &again, 10, 0);   /* same set point, next afternoon */

	pf_tune_anchor list[PF_TUNE_ANCHORS];
	int n = pf_learning_anchor_list(list, PF_TUNE_ANCHORS);
	TEST_ASSERT_EQUAL_INT(2, n);
	TEST_ASSERT_EQUAL_INT(2, list[0].runs);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 34.0, list[0].PB_c);   /* half way, not all the way */
	TEST_ASSERT_EQUAL_INT(1, list[1].runs);
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 70.0, list[1].PB_c);   /* 350 untouched */
}

/* Between the temperatures that were measured, the model is read by interpolation, so a set point
 * nobody has tuned at still gets an answer that belongs to this grill rather than to a default.
 * Outside the measured range it holds flat: extrapolating a straight line past 450 F or below
 * 180 F would invent a tuning nothing supports. */
static void test_an_untuned_set_point_interpolates_between_its_neighbours(void)
{
	pf_learning_clear_anchors();
	pf_autotune_result at225 = { .Ku = 0.085, .Pu = 360, .PB_c = 26.0, .Ti = 800, .Td = 57.0, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(225), &at225, 10, 0);
	pf_autotune_result at250 = { .Ku = 0.070, .Pu = 400, .PB_c = 34.0, .Ti = 900, .Td = 64.0, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(250), &at250, 10, 0);

	/* 235 F sits 40 % of the way from 225 to 250 */
	double PB = 0, Ti = 0, Td = 0;
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(235), &PB, &Ti, &Td));
	TEST_ASSERT_DOUBLE_WITHIN(0.05, 29.2, PB);
	TEST_ASSERT_DOUBLE_WITHIN(0.5, 840.0, Ti);
	TEST_ASSERT_DOUBLE_WITHIN(0.1, 59.8, Td);

	/* and it is monotonic across the range: every step warmer asks for a wider band */
	double prev = 0;
	for (int f = 225; f <= 250; f += 5) {
		TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(f), &PB, NULL, NULL));
		TEST_ASSERT_TRUE(PB > prev);
		prev = PB;
	}
	/* outside the measured range, the nearest measurement stands */
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(180), &PB, NULL, NULL));
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 26.0, PB);
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(400), &PB, NULL, NULL));
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 34.0, PB);
}

/* The library holds eight entries. When a ninth arrives something has to go, and what goes should
 * be the entry the model can most nearly reconstruct without it -- one sitting right next to a
 * neighbour -- never the one at the end of the range, which is the only evidence the grill has up
 * or down there, and never a well-refined entry over a single-run one beside it. */
static void test_a_full_library_gives_up_its_most_redundant_entry(void)
{
	pf_learning_clear_anchors();
	pf_autotune_result r = { .Ku = 0.07, .Pu = 400, .PB_c = 30.0, .Ti = 880, .Td = 63.5, .valid = true };
	/* seven well spread, plus one crowded right up against a neighbour */
	const int fs[8] = { 180, 200, 250, 300, 350, 400, 445, 455 };
	for (int i = 0; i < 8; i++) { r.PB_c = 20.0 + i; pf_learning_store_anchor(pf_f_to_c(fs[i]), &r, 10, 0); }
	r.PB_c = 45.0;
	pf_learning_store_anchor(pf_f_to_c(225), &r, 10, 0);

	pf_tune_anchor list[PF_TUNE_ANCHORS];
	int n = pf_learning_anchor_list(list, PF_TUNE_ANCHORS);
	TEST_ASSERT_EQUAL_INT(8, n);
	int crowded = 0;
	bool has_180 = false, has_225 = false;
	for (int i = 0; i < n; i++) {
		double f = pf_c_to_f(list[i].setpoint_c);
		if (f > 430) crowded++;
		if (fabs(f - 180) < 2) has_180 = true;
		if (fabs(f - 225) < 2) has_225 = true;
	}
	TEST_ASSERT_TRUE_MESSAGE(has_225, "the new measurement must be in the library");
	TEST_ASSERT_TRUE_MESSAGE(has_180, "the bottom of the range must survive");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, crowded, "one of the two crowded entries is what to give up");
}

int main(void)
{
	pf_log_init(PF_LOG_ERROR);
	UNITY_BEGIN();
	RUN_TEST(test_observations_and_fit_across_ambients);
	RUN_TEST(test_repeat_cook_not_worse);
	RUN_TEST(test_autotune);
	RUN_TEST(test_a_tune_at_one_set_point_leaves_the_others_alone);
	RUN_TEST(test_a_repeat_run_refines_that_set_point_only);
	RUN_TEST(test_an_untuned_set_point_interpolates_between_its_neighbours);
	RUN_TEST(test_a_full_library_gives_up_its_most_redundant_entry);
	return UNITY_END();
}
