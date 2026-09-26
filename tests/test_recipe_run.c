/* The recipe runner's clock. "Hold 250 for an hour" is an hour at 250: the step's clock must not
 * start until the pit gets there, and the countdown the app shows must be the clock, not a
 * probe's wandering estimate. */
#include "core/cmdq.h"
#include "core/control.h"
#include "core/db.h"
#include "core/env.h"
#include "core/events.h"
#include "core/history.h"
#include "core/log.h"
#include "core/outputs.h"
#include "core/settings.h"
#include "core/status.h"
#include "controllers/registry.h"
#include "features/learning.h"
#include "features/tuner.h"
#include "features/recipe.h"
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
/* The clock for the once-a-second services work. It lives out here, and is reset with `now` in
 * setUp, because it used to be a static inside tick(): `now` restarts at 1000 for every test while
 * that static kept the previous test's value, so after a long test the services block -- which is
 * what drives the tuning run -- silently did nothing for the whole of the next one. A run started
 * there would sit in "starting" for ever and measure nothing, which looked exactly like a broken
 * tuner and was a broken stopwatch. */
static double last_service;

/* one simulated second of everything, including the once-a-second services work */
static void tick(double dt)
{
	int steps = (int)(dt / 0.1 + 0.5);
	for (int i = 0; i < steps; i++) {
		now += 0.1;
		pf_sim_step(0.1);
		pf_probes_poll(now);
		pf_control_step(&ctrl, now);
		if (now - last_service >= 1.0) {
			last_service = now;
			pf_status st;
			pf_status_get(&st);
			cJSON *j = pf_status_to_json(&st, pf_settings_units());
			pf_tuner_tick(j, now);
			cJSON_Delete(j);
		}
	}
}

void setUp(void)
{
	snprintf(cfg_path, sizeof cfg_path, "/tmp/pf_tuner_%d.json", (int)getpid());
	snprintf(db_path, sizeof db_path, "/tmp/pf_tuner_%d.db", (int)getpid());
	unlink(cfg_path); unlink(db_path);
	TEST_ASSERT_EQUAL_INT(0, pf_settings_init(cfg_path));
	pf_settings_force_sim();
	pf_settings_patch("startup", "{\"smartstart\":{\"enabled\":false},\"startup_exit_temp\":0,"
	                  "\"start_to_mode\":{\"after_startup_mode\":\"Hold\",\"primary_setpoint\":225}}", NULL, 0);
	TEST_ASSERT_EQUAL_INT(0, pf_db_open(db_path));
	pf_events_init();
	pf_controllers_init(NULL);
	pf_probe_drivers_init(NULL);
	pf_env env; pf_env_init(&env, "platform");
	void *inst = pf_platform_sim()->create("{}", &env);
	pf_outputs_init(pf_platform_sim(), inst);
	pf_cmdq_init();
	pf_history_init();
	pf_probes_init();
	pf_learning_init();
	pf_tuner_init();
	pf_control_init(&ctrl, true);
	now = 1000;
	last_service = now;
	pf_sim_reset(18.0);
	tick(3);
}

void tearDown(void)
{
	pf_control_shutdown(&ctrl);
	pf_probes_shutdown();
	pf_outputs_shutdown();
	pf_db_close();
	pf_settings_shutdown();
	unlink(cfg_path); unlink(db_path);
	char extra[300];
	snprintf(extra, sizeof extra, "%s-wal", db_path); unlink(extra);
	snprintf(extra, sizeof extra, "%s-shm", db_path); unlink(extra);
}


static int save_recipe(const char *json)
{
	char err[128] = "";
	int id = pf_recipe_save(json, err, sizeof err);
	TEST_ASSERT_TRUE_MESSAGE(id > 0, err);
	return id;
}

static void test_a_hold_steps_clock_starts_when_the_pit_arrives(void)
{
	pf_recipes_init();
	/* one hold, 20 minutes at 110 C, on a grill that starts at 18 C and is lit by the recipe */
	int id = save_recipe("{\"name\":\"Clock\",\"units\":\"C\",\"steps\":[{\"mode\":\"Startup\"},"
	                     "{\"mode\":\"Hold\",\"setpoint\":110,\"ends\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"elapsed\",\"op\":\">=\",\"value\":1200}]}},"
	                     "{\"mode\":\"Shutdown\"}]}");
	pf_cmd c = { .type = PF_CMD_RECIPE_START, .num = id };
	pf_cmdq_push(&c);
	tick(2);
	TEST_ASSERT_TRUE(ctrl.recipe.active);
	/* run until the recipe is on its Hold step and the grill is in Hold, still below the set point */
	for (int i = 0; i < 60 * 60 && !(ctrl.recipe.step == 1 && ctrl.mode == PF_MODE_HOLD); i++) tick(1);
	TEST_ASSERT_EQUAL_INT(1, ctrl.recipe.step);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.pit_c < 110, "the pit should still be climbing when the Hold step begins");
	double hold_began = now;
	/* the clock has not started: elapsed stays at zero and the remaining time is the whole
	 * twenty minutes plus the climb, however long the grill spends getting there */
	tick(120);
	pf_status st; pf_status_get(&st);
	TEST_ASSERT_FALSE_MESSAGE(st.recipe.at_temp, "not at temperature yet");
	TEST_ASSERT_EQUAL_DOUBLE(0, ctrl.recipe.at_temp_since);
	TEST_ASSERT_TRUE_MESSAGE(st.recipe.clock_s < 0 || st.recipe.clock_s >= 1200 - 1, "the clock must not have run while the pit was climbing");
	/* now let it arrive */
	for (int i = 0; i < 60 * 60 && ctrl.recipe.at_temp_since <= 0; i++) tick(1);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.recipe.at_temp_since > 0, "the pit should reach the set point");
	TEST_ASSERT_TRUE_MESSAGE(ctrl.recipe.at_temp_since - hold_began > 60, "arrival came well after the step began");
	pf_status_get(&st);
	TEST_ASSERT_TRUE(st.recipe.at_temp);
	double clock0 = st.recipe.clock_s;
	TEST_ASSERT_DOUBLE_WITHIN(5, 1200, clock0);
	/* and from here the clock runs at one second per second, whatever the pit does */
	tick(300);
	pf_status_get(&st);
	TEST_ASSERT_DOUBLE_WITHIN(5, clock0 - 300, st.recipe.clock_s);
	/* and the step ends twenty minutes after arrival, not twenty minutes after it began */
	TEST_ASSERT_EQUAL_INT(1, ctrl.recipe.step);
	tick(1200 - 300 + 30);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.recipe.step >= 2 || !ctrl.recipe.active, "the step should have ended twenty minutes after arrival");
}

/* The overrides a cook sets while the run is going: a step marked Skip is passed over when the
 * run reaches it, one marked Auto answers its own prompt, one marked Pause waits at its end for
 * the cook and goes on when they continue. */
static void test_step_overrides_skip_auto_and_pause(void)
{
	pf_recipes_init();
	int id = save_recipe("{\"name\":\"Overrides\",\"units\":\"C\",\"steps\":[{\"mode\":\"Startup\"},"
	                     "{\"mode\":\"Hold\",\"setpoint\":100,\"ends\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"elapsed\",\"op\":\">=\",\"value\":10},{\"trait\":\"prompt\",\"op\":\"is_on\"}]}},"
	                     "{\"mode\":\"Hold\",\"setpoint\":110,\"ends\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"elapsed\",\"op\":\">=\",\"value\":10}]}},"
	                     "{\"mode\":\"Hold\",\"setpoint\":120,\"ends\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"elapsed\",\"op\":\">=\",\"value\":10}]}},"
	                     "{\"mode\":\"Shutdown\"}]}");
	pf_cmd c = { .type = PF_CMD_RECIPE_START, .num = id };
	pf_cmdq_push(&c);
	tick(2);
	pf_cmd f1 = { .type = PF_CMD_RECIPE_FLAG, .num = 1, .num2 = 3 };   /* auto */
	pf_cmd f2 = { .type = PF_CMD_RECIPE_FLAG, .num = 2, .num2 = 2 };   /* skip */
	pf_cmd f3 = { .type = PF_CMD_RECIPE_FLAG, .num = 3, .num2 = 1 };   /* pause */
	pf_cmdq_push(&f1); pf_cmdq_push(&f2); pf_cmdq_push(&f3);
	tick(2);
	TEST_ASSERT_EQUAL_INT(3, ctrl.recipe.flags[1]);
	/* step 1 answers its own prompt: the run must reach step 3 (skipping 2) without any Next */
	for (int i = 0; i < 3 * 60 * 60 && ctrl.recipe.active && ctrl.recipe.step < 3; i++) tick(1);
	TEST_ASSERT_EQUAL_INT_MESSAGE(3, ctrl.recipe.step, "auto should have answered step 1's prompt and skip should have passed over step 2");
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, ctrl.recipe.flags[2], "a skip is spent when it is used");
	/* step 3 ends after ten seconds at 120 C, then pauses for the cook */
	for (int i = 0; i < 2 * 60 * 60 && ctrl.recipe.active && !ctrl.recipe.waiting; i++) tick(1);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.recipe.waiting, "the paused step should wait for the cook");
	TEST_ASSERT_EQUAL_INT(3, ctrl.recipe.step);
	tick(120);
	TEST_ASSERT_EQUAL_INT_MESSAGE(3, ctrl.recipe.step, "and keep waiting");
	pf_cmd n = { .type = PF_CMD_RECIPE_NEXT };
	pf_cmdq_push(&n);
	for (int i = 0; i < 60 && ctrl.recipe.step == 3; i++) tick(1);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.recipe.step >= 4 || !ctrl.recipe.active, "Next releases the pause and the run goes on");
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_a_hold_steps_clock_starts_when_the_pit_arrives);
	RUN_TEST(test_step_overrides_skip_auto_and_pause);
	return UNITY_END();
}
