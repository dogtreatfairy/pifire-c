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

static void test_monitor_adjusts_scale(void)
{
	g_kv[0] = 0;
	const pf_controller_ops *ops = pf_controller_find("adaptive");
	void *c = ops->create("{\"_units\":\"C\"}", &env);
	double t = 1000;
	pf_ctrl_in in0 = { .now_s = t, .pit_c = 110, .setpoint_c = 110, .ambient_c = 20, .u_prev_applied = 0.3, .u_ff = 0.3, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9 };
	ops->reset(c, &in0);
	run_window(ops, c, &t, 110, pit_good, 0);
	TEST_ASSERT_EQUAL_DOUBLE(1.0, state_num(ops, c, "scale"));         /* well-behaved: untouched */
	run_window(ops, c, &t, 110, pit_oscillating, 0);
	double s1 = state_num(ops, c, "scale");
	TEST_ASSERT_TRUE(s1 < 1.0);                                          /* oscillation: gain reduced */
	TEST_ASSERT_TRUE(strstr(g_kv, "\"scale\"") != NULL);                 /* persisted */
	t += 2000;                                                           /* well past any set-point change */
	run_window(ops, c, &t, 110, pit_sluggish, 0);
	double s2 = state_num(ops, c, "scale");
	TEST_ASSERT_TRUE(s2 > s1);                                           /* sluggish: gain raised */
	run_window(ops, c, &t, 110, pit_good, 0);                            /* flush the window with calm data */
	double s3 = state_num(ops, c, "scale");
	run_window(ops, c, &t, 110, pit_sluggish, -1);
	TEST_ASSERT_EQUAL_DOUBLE(s3, state_num(ops, c, "scale"));            /* saturated at u_min: not the loop's fault */
	ops->destroy(c);
}

static void test_overshoot_lowers_scale(void)
{
	g_kv[0] = 0;
	const pf_controller_ops *ops = pf_controller_find("adaptive");
	void *c = ops->create("{\"_units\":\"C\"}", &env);
	double t = 1000;
	pf_ctrl_in in = { .now_s = t, .pit_c = 100, .setpoint_c = 100, .ambient_c = 20, .u_prev_applied = 0.3, .u_ff = 0.3, .cycle_time_s = 20, .u_min = 0.1, .u_max = 0.9 };
	ops->reset(c, &in);
	/* step to 140: the pit climbs, overshoots to 152, then settles */
	double path[] = { 100, 108, 118, 128, 138, 146, 152, 150, 146, 142, 141, 140, 140, 140 };
	for (size_t k = 0; k < sizeof path / sizeof path[0]; k++) {
		t += 20;
		in.now_s = t; in.pit_c = path[k]; in.setpoint_c = 140;
		ops->update(c, &in, NULL);
	}
	TEST_ASSERT_TRUE(state_num(ops, c, "scale") < 1.0);
	ops->destroy(c);
}

int main(void)
{
	pf_controllers_init(NULL);
	UNITY_BEGIN();
	RUN_TEST(test_model_tuning_and_persistence);
	RUN_TEST(test_monitor_adjusts_scale);
	RUN_TEST(test_overshoot_lowers_scale);
	return UNITY_END();
}
