/* Adaptive controller self-tuning: model-derived gains, persistence, and the performance monitor. */
#include "controllers/registry.h"
#include "pifire/controller.h"
#include "unity.h"
#include <cJSON.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* in-memory env: one kv slot, quiet log */
static char g_kv[512];
static int kv_get(const pf_env *e, const char *key, char *out, size_t n) { (void)e; (void)key; if (!g_kv[0]) return 1; snprintf(out, n, "%s", g_kv); return 0; }
static int kv_put(const pf_env *e, const char *key, const char *json) { (void)e; (void)key; snprintf(g_kv, sizeof g_kv, "%s", json); return 0; }
static void logf_(int level, const char *tag, const char *fmt, ...) { (void)level; (void)tag; (void)fmt; }
static pf_env env = { .log = logf_, .kv_get = kv_get, .kv_put = kv_put };

static double state_num(const pf_controller_ops *ops, void *c, const char *key)
{
	char buf[512];
	ops->state_json(c, buf, sizeof buf);
	cJSON *j = cJSON_Parse(buf);
	cJSON *it = cJSON_GetObjectItem(j, key);
	double v = !it ? NAN : cJSON_IsTrue(it) ? 1 : cJSON_IsFalse(it) ? 0 : it->valuedouble;
	cJSON_Delete(j);
	return v;
}

static void test_model_tuning_and_persistence(void)
{
	g_kv[0] = 0;
	const pf_controller_ops *ops = pf_controller_find("adaptive");
	TEST_ASSERT_NOT_NULL(ops);
	void *c = ops->create("{\"_units\":\"C\",\"PB\":80,\"Ti\":400,\"Td\":30}", &env);
	TEST_ASSERT_EQUAL_DOUBLE(80.0, state_num(ops, c, "PB_c"));
	/* a typical pellet grill: 300 C per unit duty, tau 240 s, dead time 45 s */
	ops->apply_tuning(c, 0, 0, 300, 240, 45);
	double PB = state_num(ops, c, "PB_c"), Ti = state_num(ops, c, "Ti"), Td = state_num(ops, c, "Td");
	TEST_ASSERT_TRUE(PB >= 40 && PB <= 120);
	TEST_ASSERT_TRUE(Ti >= 200 && Ti <= 240.5);
	TEST_ASSERT_TRUE(Td > 0 && Td <= 60);
	TEST_ASSERT_EQUAL_DOUBLE(1.0, state_num(ops, c, "learned"));
	TEST_ASSERT_TRUE(strstr(g_kv, "\"src\":\"model\"") != NULL);
	ops->destroy(c);
	/* a new instance restores the learned gains from kv */
	c = ops->create("{\"_units\":\"C\"}", &env);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, PB, state_num(ops, c, "PB_c"));
	TEST_ASSERT_DOUBLE_WITHIN(0.1, Ti, state_num(ops, c, "Ti"));
	/* auto_tune off -> configured values win, learned kept */
	ops->configure(c, "{\"_units\":\"C\",\"auto_tune\":false,\"PB\":60}");
	TEST_ASSERT_EQUAL_DOUBLE(60.0, state_num(ops, c, "PB_c"));
	ops->destroy(c);
}

static void run_window(const pf_controller_ops *ops, void *c, double *t, double sp, double (*pit)(double t), int saturated)
{
	/* 16 minutes of 20 s cycles */
	for (int k = 0; k < 48; k++) {
		*t += 20;
		pf_ctrl_in in = { .now_s = *t, .pit_c = pit(*t), .setpoint_c = sp, .ambient_c = 20, .u_prev_raw = 0.3, .u_prev_applied = 0.3, .u_ff = 0.3, .saturated = saturated, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9 };
		ops->update(c, &in, NULL);
	}
}

static double pit_oscillating(double t) { return 110 + 6.0 * sin(t / 40.0); }   /* +/-6 C, period ~4 min */
static double pit_sluggish(double t) { (void)t; return 103; }                   /* sits 7 C low, no movement */
static double pit_good(double t) { return 110 + 0.5 * sin(t / 60.0); }

/* The monitor's whole job is to move the proportional band itself -- in degrees, the same number
 * that is displayed, exported and typed into another grill -- rather than a hidden factor applied
 * to it. A wider band is a gentler loop, a narrower one more aggressive. */
static void test_monitor_adjusts_the_band(void)
{
	g_kv[0] = 0;
	const pf_controller_ops *ops = pf_controller_find("adaptive");
	void *c = ops->create("{\"_units\":\"C\"}", &env);
	double t = 1000;
	pf_ctrl_in in0 = { .now_s = t, .pit_c = 110, .setpoint_c = 110, .ambient_c = 20, .u_prev_applied = 0.3, .u_ff = 0.3, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9 };
	ops->reset(c, &in0);
	double base = state_num(ops, c, "PB_c");
	TEST_ASSERT_TRUE(base > 0);
	run_window(ops, c, &t, 110, pit_good, 0);
	TEST_ASSERT_EQUAL_DOUBLE(base, state_num(ops, c, "PB_c"));           /* well-behaved: untouched */
	run_window(ops, c, &t, 110, pit_oscillating, 0);
	double b1 = state_num(ops, c, "PB_c");
	TEST_ASSERT_TRUE_MESSAGE(b1 > base, "oscillation should widen the band");
	TEST_ASSERT_TRUE(strstr(g_kv, "\"band_learned\"") != NULL);           /* persisted */
	t += 2000;                                                           /* well past any set-point change */
	run_window(ops, c, &t, 110, pit_sluggish, 0);
	double b2 = state_num(ops, c, "PB_c");
	TEST_ASSERT_TRUE_MESSAGE(b2 < b1, "sluggish should tighten the band");
	run_window(ops, c, &t, 110, pit_good, 0);                            /* flush the window with calm data */
	double b3 = state_num(ops, c, "PB_c");
	run_window(ops, c, &t, 110, pit_sluggish, -1);
	TEST_ASSERT_EQUAL_DOUBLE(b3, state_num(ops, c, "PB_c"));             /* saturated at u_min: not the loop's fault */
	/* whatever it settles on stays within reach of the tuning it is refining */
	TEST_ASSERT_TRUE(b3 >= base * 0.69 && b3 <= base * 1.61);
	ops->destroy(c);
}

static void test_overshoot_widens_the_band(void)
{
	g_kv[0] = 0;
	const pf_controller_ops *ops = pf_controller_find("adaptive");
	void *c = ops->create("{\"_units\":\"C\"}", &env);
	double t = 1000;
	pf_ctrl_in in = { .now_s = t, .pit_c = 100, .setpoint_c = 100, .ambient_c = 20, .u_prev_applied = 0.3, .u_ff = 0.3, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9 };
	ops->reset(c, &in);
	double base = state_num(ops, c, "PB_c");
	/* step to 140: the pit climbs, overshoots to 152, then settles */
	double path[] = { 100, 108, 118, 128, 138, 146, 152, 150, 146, 142, 141, 140, 140, 140 };
	for (size_t k = 0; k < sizeof path / sizeof path[0]; k++) {
		t += 20;
		in.now_s = t; in.pit_c = path[k]; in.setpoint_c = 140;
		ops->update(c, &in, NULL);
	}
	TEST_ASSERT_TRUE(state_num(ops, c, "PB_c") > base);
	ops->destroy(c);
}

/* Hold entered 30 C below the target with the u_min placeholder as "last duty": the integrator must not be
 * seeded negative from it (that held the feed back for the whole cook in a real log); near the target the
 * seed is bumpless within +/-0.15 duty. */
static void test_integrator_seed_and_no_opposition(void)
{
	const pf_controller_ops *ops = pf_controller_find("adaptive");
	void *c = ops->create("{\"auto_tune\":false,\"PB\":90,\"Ti\":286,\"Td\":75}", &env);
	pf_ctrl_dbg dbg;
	double t = 0;
	pf_ctrl_in in = { .now_s = t, .pit_c = 146, .setpoint_c = 176, .ambient_c = 20, .u_prev_applied = 0.10, .u_ff = 0.6, .cycle_time_s = 25, .u_min = 0.1, .u_max = 0.9 };
	ops->reset(c, &in);
	double u = ops->update(c, &in, &dbg);
	TEST_ASSERT_DOUBLE_WITHIN(0.02, 0.0, dbg.i);            /* no negative seed */
	TEST_ASSERT_TRUE(u > 0.6 + 0.2);                        /* ff + P pushing hard */
	/* a negative integral cannot survive while the pit is far below target */
	for (int k = 0; k < 20; k++) { t += 25; in.now_s = t; in.pit_c = 150; ops->update(c, &in, &dbg); }
	TEST_ASSERT_TRUE(dbg.i >= -0.001);
	/* near the target the seed is bumpless: last duty 0.55 vs ff 0.6 + P ~0 -> i ~ -0.05 */
	in.pit_c = 175; in.u_prev_applied = 0.55; in.now_s = t + 25;
	ops->reset(c, &in);
	ops->update(c, &in, &dbg);
	TEST_ASSERT_DOUBLE_WITHIN(0.03, -0.05, dbg.i);
	/* and the seed is capped at +/-0.15 duty */
	in.u_prev_applied = 0.10; in.now_s = t + 50;
	ops->reset(c, &in);
	ops->update(c, &in, &dbg);
	TEST_ASSERT_TRUE(dbg.i >= -0.16);
	ops->destroy(c);
}

/* an oscillation around a 230 C set point, for the top band */
static double pit_oscillating_hot(double t) { return 230 + 6.0 * sin(t / 40.0); }

/* What the loop needs at 110 C is not what it needs at 230 C, so the band learning settles on is
 * kept per temperature range and a lesson at one end must not move the other. */
static void test_the_band_is_learned_per_temperature_range(void)
{
	g_kv[0] = 0;
	const pf_controller_ops *ops = pf_controller_find("adaptive");
	void *c = ops->create("{\"_units\":\"C\"}", &env);
	double t = 1000;
	pf_ctrl_in in0 = { .now_s = t, .pit_c = 110, .setpoint_c = 110, .ambient_c = 20, .u_prev_applied = 0.3, .u_ff = 0.3, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9 };
	ops->reset(c, &in0);
	double base = state_num(ops, c, "PB_c");

	/* hunt badly at 110 C (a low band) until the monitor widens the band */
	run_window(ops, c, &t, 110, pit_oscillating, 0);
	run_window(ops, c, &t, 110, pit_oscillating, 0);
	double low = state_num(ops, c, "PB_c");
	printf("band at 110 C: %.1f C (from %.1f)\n", low, base);
	TEST_ASSERT_TRUE_MESSAGE(low > base, "hunting should widen the band");

	/* move to 230 C: the lesson from the low band must not follow */
	t += 2000;
	run_window(ops, c, &t, 230, pit_good, 0);
	double hot = state_num(ops, c, "PB_c");
	printf("band at 230 C: %.1f C\n", hot);
	TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(base, hot, "the hot band should start from the tuning, untouched");

	/* and a lesson up here stays up here */
	run_window(ops, c, &t, 230, pit_oscillating_hot, 0);
	run_window(ops, c, &t, 230, pit_oscillating_hot, 0);
	double hot2 = state_num(ops, c, "PB_c");
	TEST_ASSERT_TRUE(hot2 > base);
	t += 2000;
	run_window(ops, c, &t, 110, pit_good, 0);
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.001, low, state_num(ops, c, "PB_c"), "the low band should be as it was left");

	/* both survive a restart */
	ops->destroy(c);
	c = ops->create("{\"_units\":\"C\"}", &env);
	pf_ctrl_in in1 = { .now_s = t, .pit_c = 230, .setpoint_c = 230, .ambient_c = 20, .u_prev_applied = 0.3, .u_ff = 0.3, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9 };
	ops->reset(c, &in1);
	ops->update(c, &in1, NULL);
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.001, hot2, state_num(ops, c, "PB_c"), "per-band learning should persist");
	ops->destroy(c);
}

/* A tune measured after the fact is the authority, and a lesson learned against the old numbers is
 * carried onto it in proportion rather than thrown away or applied literally: the point of the
 * whole arrangement is that the band actually running is the tuned band, refined. */
static void test_learning_refines_a_tune_rather_than_being_overruled_by_it(void)
{
	g_kv[0] = 0;
	const pf_controller_ops *ops = pf_controller_find("adaptive");
	void *c = ops->create("{\"_units\":\"C\",\"PB\":100,\"Ti\":400,\"Td\":30}", &env);
	double t = 1000;
	pf_ctrl_in in = { .now_s = t, .pit_c = 110, .setpoint_c = 110, .ambient_c = 20, .u_prev_applied = 0.3, .u_ff = 0.3, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9 };
	ops->reset(c, &in);
	run_window(ops, c, &t, 110, pit_oscillating, 0);
	run_window(ops, c, &t, 110, pit_oscillating, 0);
	double learned = state_num(ops, c, "PB_c");
	TEST_ASSERT_TRUE(learned > 100);
	double ratio = learned / 100.0;

	/* the tuning library now offers 60 C for this set point: that is the new anchor, and the
	 * lesson rides on it instead of the number it was learned against */
	for (int k = 0; k < 3; k++) {
		t += 20;
		pf_ctrl_in sc = { .now_s = t, .pit_c = 110, .setpoint_c = 110, .ambient_c = 20, .u_prev_applied = 0.3, .u_ff = 0.3, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9,
		                  .sched_PB_c = 60, .sched_Ti = 300, .sched_Td = 20 };
		ops->update(c, &sc, NULL);
	}
	double now = state_num(ops, c, "PB_c");
	printf("learned %.1f on 100, riding a tune of 60 -> %.1f\n", learned, now);
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.5, 60.0 * ratio, now, "the lesson should ride the new tune in proportion");
	TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(300.0, state_num(ops, c, "Ti"), "Ti is measured, not learned");
	ops->destroy(c);
}

/* Gains move under the controller all the time: a new set point picks a different entry from the
 * tuning library, the monitor corrects a band, a fresh tune lands. The integrator stores an
 * accumulated error, so its contribution is ki times that, and if the accumulator is not rescaled
 * when ki changes the contribution jumps by the ratio of the gains. At the extremes that was
 * enough to push the output off scale and have the daemon swap the controller out. */
static void test_gain_change_does_not_jolt_the_integrator(void)
{
	g_kv[0] = 0;
	const pf_controller_ops *ops = pf_controller_find("adaptive");
	/* Two identical loops, fed the same inputs cycle for cycle. Only one of them is handed a new
	 * tuning part way through, so whatever the integrator does in the other is what this input was
	 * always going to make it do -- the prediction winding in, the wind-up trim at the band edge --
	 * and the gap between the two is the jolt the gain change itself caused, which is the only
	 * thing this test is about. Comparing a single loop against its own previous cycle cannot tell
	 * those apart, and it wrote off a correctly behaving controller once already. */
	const char *cfg = "{\"_units\":\"C\",\"PB\":80,\"Ti\":400,\"Td\":30}";
	void *a = ops->create(cfg, &env);   /* gains change under this one */
	void *b = ops->create(cfg, &env);   /* control: same inputs, same gains throughout */
	pf_ctrl_dbg da = { 0 }, db = { 0 };
	double t = 1000;

	/* settle in with a seeded integrator, close to the target */
	pf_ctrl_in in = { .now_s = t, .pit_c = 108, .setpoint_c = 110, .ambient_c = 20,
	                  .u_prev_applied = 0.45, .u_ff = 0.30, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9 };
	ops->reset(a, &in); ops->reset(b, &in);
	for (int k = 0; k < 5; k++) { t += 20; in.now_s = t; ops->update(a, &in, &da); ops->update(b, &in, &db); }
	double i_before = da.i, inter_before = da.integral;

	/* now hand over a very different tuning for this set point, as the library does */
	pf_ctrl_in sched = in;
	sched.sched_PB_c = 20; sched.sched_Ti = 60; sched.sched_Td = 10;
	t += 20; in.now_s = t; sched.now_s = t;
	double u = ops->update(a, &sched, &da);
	ops->update(b, &in, &db);
	/* Those gains are about twenty-seven times more aggressive. What the accumulator did this
	 * cycle is the same in both loops -- the error it integrates does not depend on the gains -- so
	 * the untouched loop measures it, and the contribution the changed loop should be showing is
	 * the one it carried in plus that fresh error at the NEW gain. Unrescaled, the carried part
	 * would arrive multiplied by twenty-seven instead. */
	double d_inter = db.integral - inter_before;
	double ki_new = -1.0 / (state_num(ops, a, "PB_c") * state_num(ops, a, "Ti"));
	double carried = i_before + ki_new * d_inter;
	printf("integral %.4f -> %.4f (carry-over predicts %.4f), output %.3f\n", i_before, da.i, carried, u);
	TEST_ASSERT_TRUE_MESSAGE(fabs(u) <= 5.0, "the output must stay in range when the gains change");
	TEST_ASSERT_TRUE_MESSAGE(fabs(da.i - carried) < 0.5 * fabs(i_before) + 0.05,
	                         "the integral's contribution should carry over, not scale with the gains");

	/* several more cycles at the new gains: it must stay bounded rather than run away */
	for (int k = 0; k < 20; k++) { t += 20; sched.now_s = in.now_s = t; u = ops->update(a, &sched, &da); ops->update(b, &in, &db); }
	printf("after 20 more cycles: integral %.4f, output %.3f\n", da.i, u);
	TEST_ASSERT_TRUE_MESSAGE(fabs(u) <= 5.0, "the output must stay in range");

	/* and back the other way, to a much wider band. The control loop carries on at the gains it
	 * has had all along, so it still measures only what this input does on its own. */
	double i_mid = da.i, ctrl_mid = db.integral;
	sched.sched_PB_c = 200; sched.sched_Ti = 1200; sched.sched_Td = 90;
	t += 20; sched.now_s = in.now_s = t;
	u = ops->update(a, &sched, &da);
	ops->update(b, &in, &db);
	ki_new = -1.0 / (state_num(ops, a, "PB_c") * state_num(ops, a, "Ti"));
	carried = i_mid + ki_new * (db.integral - ctrl_mid);
	TEST_ASSERT_TRUE(fabs(u) <= 5.0);
	TEST_ASSERT_TRUE_MESSAGE(fabs(da.i - carried) < 0.5 * fabs(i_mid) + 0.05,
	                         "widening the band should not jolt it either");
	ops->destroy(a); ops->destroy(b);
}

/* Learning settles a correction per temperature range, and a tuning run measures the grill at one
 * temperature. Rescaling every range whenever any run finished was the last way a run at 350 F
 * could quietly make 250 F worse: the correction earned around 250 over whole cooks was pulled
 * half way back toward a measurement taken somewhere else entirely. A run now only touches the
 * range it was taken in; what the grill learned about the others is carried onto the new number in
 * proportion, which is what makes another tune an addition to the model rather than a trade. */
static void test_a_tune_at_one_temperature_keeps_what_was_learned_at_another(void)
{
	g_kv[0] = 0;
	const pf_controller_ops *ops = pf_controller_find("adaptive");
	void *c = ops->create("{\"_units\":\"C\"}", &env);
	double t = 1000;
	pf_ctrl_in in = { .now_s = t, .pit_c = 110, .setpoint_c = 110, .ambient_c = 20, .u_prev_applied = 0.3, .u_ff = 0.3, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9 };
	ops->reset(c, &in);
	double base = state_num(ops, c, "PB_c");

	/* a cook around 230 F teaches the loop to back off there */
	run_window(ops, c, &t, 110, pit_oscillating, 0);
	double learned = state_num(ops, c, "PB_c");
	TEST_ASSERT_TRUE(learned > base);
	double ratio = learned / base;

	/* now measure the grill at 350 F, which lands in a different range */
	in.now_s = t += 600; in.setpoint_c = 176.7; in.pit_c = 176.7;
	ops->reset(c, &in);
	ops->apply_tuning(c, 0.05, 400, 0, 0, 0);          /* relay: PB 44 C */
	double measured = state_num(ops, c, "PB_c");
	TEST_ASSERT_DOUBLE_WITHIN(0.5, 44.0, measured);    /* up here, the measurement as taken */

	/* back down to the cook it had learned about: the lesson survives, applied to the new tuning */
	in.now_s = t += 600; in.setpoint_c = 110; in.pit_c = 110;
	ops->reset(c, &in);
	TEST_ASSERT_DOUBLE_WITHIN(0.5, 44.0 * ratio, state_num(ops, c, "PB_c"));
	ops->destroy(c);
}

int main(void)
{
	pf_controllers_init(NULL);
	UNITY_BEGIN();
	RUN_TEST(test_model_tuning_and_persistence);
	RUN_TEST(test_monitor_adjusts_the_band);
	RUN_TEST(test_overshoot_widens_the_band);
	RUN_TEST(test_the_band_is_learned_per_temperature_range);
	RUN_TEST(test_learning_refines_a_tune_rather_than_being_overruled_by_it);
	RUN_TEST(test_gain_change_does_not_jolt_the_integrator);
	RUN_TEST(test_integrator_seed_and_no_opposition);
	RUN_TEST(test_a_tune_at_one_temperature_keeps_what_was_learned_at_another);
	return UNITY_END();
}
