/* The recipe runner: what ends a step, what the message says and when, how a step aimed at "any
 * food probe" behaves with several in the meat, and what carryover does to the temperature a step
 * actually pulls at. These are the parts that drive the grill on their own for six hours, so the
 * question each test asks is one a cook would ask about their dinner. */
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
#include "features/alarms.h"
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

void setUp(void)
{
	snprintf(cfg_path, sizeof cfg_path, "/tmp/pf_rcp_%d.json", (int)getpid());
	snprintf(db_path, sizeof db_path, "/tmp/pf_rcp_%d.db", (int)getpid());
	unlink(cfg_path); unlink(db_path);
	pf_settings_init(cfg_path);
	pf_settings_force_sim();
	pf_db_open(db_path);
	pf_events_init();
	pf_recipes_init();
}

void tearDown(void)
{
	pf_db_close();
	pf_settings_shutdown();
	unlink(cfg_path); unlink(db_path);
	char p[300]; snprintf(p, sizeof p, "%s-wal", db_path); unlink(p); snprintf(p, sizeof p, "%s-shm", db_path); unlink(p);
}

static int save(const char *json)
{
	char err[128] = "";
	int id = pf_recipe_save(json, err, sizeof err);
	if (id < 0) TEST_FAIL_MESSAGE(err);
	return id;
}

/* Every step's ending is a condition tree now, whether it was written as one or grew out of the
 * older timer / probe / wait fields, so these ask questions of trees. */
static cJSON *ends_of(const pf_recipe_step *st)
{
	cJSON *t = cJSON_Parse(st->ends);
	TEST_ASSERT_NOT_NULL_MESSAGE(t, "step ending is not valid JSON");
	return t;
}

/* Is there a node anywhere in here saying exactly this? */
static bool has_term(const cJSON *n, const char *trait, const char *op, double value, bool check_value)
{
	if (!n) return false;
	const cJSON *kids = cJSON_GetObjectItem((cJSON *)n, "conditions");
	if (cJSON_IsArray(kids)) {
		const cJSON *k;
		cJSON_ArrayForEach(k, kids) if (has_term(k, trait, op, value, check_value)) return true;
		return false;
	}
	if (strcmp(pf_json_str((cJSON *)n, "trait", ""), trait)) return false;
	if (op && strcmp(pf_json_str((cJSON *)n, "op", ""), op)) return false;
	return !check_value || fabs(pf_json_num((cJSON *)n, "value", 0) - value) < 0.6;
}

/* The one that ships. It is the recipe a cook will actually run, so it is the one that has to
 * survive being written down, stored, and read back as the runner will see it. */
static void test_the_built_in_ribs_recipe_loads_as_it_was_written(void)
{
	pf_recipes_seed();
	pf_recipe r;
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(1, &r));
	TEST_ASSERT_EQUAL_STRING("3-2-1 Ribs", r.name);
	TEST_ASSERT_EQUAL_INT(5, r.nsteps);

	/* three hours of smoke at 180 F, or 160 F in the meat, whichever comes first, and then it
	 * waits for the ribs to come off -- which the lid answers as well as a tap does */
	TEST_ASSERT_EQUAL_INT(PF_MODE_HOLD, r.steps[1].mode);
	TEST_ASSERT_DOUBLE_WITHIN(0.3, pf_f_to_c(180), r.steps[1].setpoint_c);
	cJSON *e = ends_of(&r.steps[1]);
	TEST_ASSERT_TRUE(has_term(e, "elapsed", ">=", 180 * 60, true));
	TEST_ASSERT_TRUE(has_term(e, "food_max", ">=", pf_f_to_c(160), true));
	TEST_ASSERT_TRUE(has_term(e, "prompt", "is_on", 0, false));
	TEST_ASSERT_TRUE(has_term(e, "lid", "is_on", 0, false));
	cJSON_Delete(e);
	TEST_ASSERT_EQUAL_DOUBLE(10 * 60, r.steps[1].lead_s);
	TEST_ASSERT_TRUE(r.steps[1].lead_message[0] != 0);

	/* two hours wrapped at 225, then one more to 205 in the meat, rested */
	e = ends_of(&r.steps[2]);
	TEST_ASSERT_TRUE(has_term(e, "elapsed", ">=", 120 * 60, true));
	cJSON_Delete(e);
	e = ends_of(&r.steps[3]);
	TEST_ASSERT_TRUE(has_term(e, "food_rested", ">=", pf_f_to_c(205), true));
	cJSON_Delete(e);
	TEST_ASSERT_EQUAL_DOUBLE(2 * 60, r.steps[3].lead_s);
	TEST_ASSERT_EQUAL_INT(PF_MODE_SHUTDOWN, r.steps[4].mode);
}

/* Seeding twice must not leave two of them: a cook who restarts the daemon does not want a second
 * copy of every recipe they were given. */
static void test_the_built_in_recipe_is_added_once(void)
{
	pf_recipes_seed();
	pf_recipes_seed();
	cJSON *l = pf_recipes_list();
	TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(l));
	cJSON_Delete(l);
}

/* The numbers in a recipe belong to the unit they were written in. Before they carried one, a
 * recipe written at 225 F read as 225 C the moment the grill was switched to Celsius. */
static void test_a_recipe_keeps_its_units_when_the_grill_changes_its_own(void)
{
	pf_settings_patch("globals", "{\"units\":\"F\"}", NULL, 0);
	int id = save("{\"name\":\"F recipe\",\"units\":\"F\",\"steps\":[{\"mode\":\"Hold\",\"setpoint\":225,\"probe\":\"Probe1\",\"probe_temp\":203}]}");

	pf_recipe r;
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(id, &r));
	TEST_ASSERT_DOUBLE_WITHIN(0.3, pf_f_to_c(225), r.steps[0].setpoint_c);

	pf_settings_patch("globals", "{\"units\":\"C\"}", NULL, 0);
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(id, &r));
	TEST_ASSERT_DOUBLE_WITHIN(0.3, pf_f_to_c(225), r.steps[0].setpoint_c);
	TEST_ASSERT_DOUBLE_WITHIN(0.3, pf_f_to_c(203), r.steps[0].probe_temp_c);
}

/* "wait" says how a step ends and subsumes the older "pause" flag; a recipe written before wait
 * existed still has to read correctly. */
static void test_an_older_recipe_still_waits_where_it_used_to(void)
{
	int id = save("{\"name\":\"old\",\"units\":\"F\",\"steps\":[{\"mode\":\"Hold\",\"setpoint\":225,\"pause\":true}]}");
	pf_recipe r;
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(id, &r));
	TEST_ASSERT_TRUE(r.steps[0].pause);
	TEST_ASSERT_EQUAL_INT(PF_RSTEP_WAIT_NONE, r.steps[0].wait);
}

/* Carryover is worked out from how fast the probe is climbing when it comes off: fast means more
 * still to come, a stall means none, and the estimate is capped because it is built on a slope. */
static void test_carryover_follows_the_rate_the_probe_is_climbing_at(void)
{
	TEST_ASSERT_EQUAL_DOUBLE(0, pf_carryover_c(0));
	TEST_ASSERT_EQUAL_DOUBLE(0, pf_carryover_c(-0.01));
	/* half a degree C a minute is an ordinary climb through the last stretch of a cook */
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.5 / 60.0 * PF_CARRYOVER_TAU_S, pf_carryover_c(0.5 / 60.0));
	/* a wild slope must not predict a wild number */
	TEST_ASSERT_EQUAL_DOUBLE(PF_CARRYOVER_MAX_C, pf_carryover_c(10.0));
	/* and it must be worth having: a rack climbing a degree F a minute coasts a real amount */
	TEST_ASSERT_TRUE(pf_carryover_c(1.0 / 1.8 / 60.0) > 1.0);
}

/* A step with no name in it watches every Food probe in the cook. The recipe is written once and
 * used with however many probes go in the meat. */
static void test_a_step_can_be_aimed_at_any_food_probe(void)
{
	int id = save("{\"name\":\"any\",\"units\":\"F\",\"steps\":["
	              "{\"mode\":\"Hold\",\"setpoint\":225,\"probe\":\"@food\",\"probe_temp\":203,\"probe_match\":\"all\"}]}");
	pf_recipe r;
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(id, &r));
	TEST_ASSERT_EQUAL_STRING(PF_RECIPE_ANY_FOOD, r.steps[0].probe);
	TEST_ASSERT_TRUE(r.steps[0].probe_all);

	id = save("{\"name\":\"first\",\"units\":\"F\",\"steps\":["
	          "{\"mode\":\"Hold\",\"setpoint\":225,\"probe\":\"@food\",\"probe_temp\":203}]}");
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(id, &r));
	TEST_ASSERT_FALSE(r.steps[0].probe_all);
}

/* A recipe has to be refused rather than half-run: a step naming a mode the grill does not have
 * would otherwise be silently turned into a Hold. */
static void test_a_step_with_a_mode_the_grill_does_not_have_is_refused(void)
{
	char err[128] = "";
	TEST_ASSERT_TRUE(pf_recipe_save("{\"name\":\"bad\",\"steps\":[{\"mode\":\"Broil\"}]}", err, sizeof err) < 0);
	TEST_ASSERT_TRUE(err[0] != 0);
	TEST_ASSERT_TRUE(pf_recipe_save("{\"name\":\"\",\"steps\":[{\"mode\":\"Hold\"}]}", err, sizeof err) < 0);
}

/* A Startup step on a grill that is already lit is satisfied by the fire that is there. The runner
 * decides that from this one question, so this is where it is pinned down: Shutdown is deliberately
 * not a firing mode, because the fire in there is already on its way out. */
static void test_which_modes_count_as_a_grill_that_is_already_lit(void)
{
	TEST_ASSERT_TRUE(pf_mode_is_firing(PF_MODE_STARTUP));
	TEST_ASSERT_TRUE(pf_mode_is_firing(PF_MODE_REIGNITE));
	TEST_ASSERT_TRUE(pf_mode_is_firing(PF_MODE_SMOKE));
	TEST_ASSERT_TRUE(pf_mode_is_firing(PF_MODE_HOLD));
	TEST_ASSERT_FALSE(pf_mode_is_firing(PF_MODE_STOP));
	TEST_ASSERT_FALSE(pf_mode_is_firing(PF_MODE_MONITOR));
	TEST_ASSERT_FALSE(pf_mode_is_firing(PF_MODE_PRIME));
	TEST_ASSERT_FALSE(pf_mode_is_firing(PF_MODE_SHUTDOWN));
	TEST_ASSERT_FALSE(pf_mode_is_firing(PF_MODE_MANUAL));
	TEST_ASSERT_FALSE(pf_mode_is_firing(PF_MODE_ERROR));
}

static cJSON *find_alarm(cJSON *all, const char *code)
{
	cJSON *arr = cJSON_GetObjectItem(all, "alarms"), *a;
	cJSON_ArrayForEach(a, arr) if (!strcmp(pf_json_str(a, "code", ""), code)) return a;
	return NULL;
}

/* A recipe that ends without a Shutdown step leaves a fire burning with nothing behind it. That is
 * a condition, not a moment: it is asked about, it can be snoozed, and it ends by itself when the
 * grill goes out -- which is what makes snoozing it for an hour harmless. */
static void test_the_still_running_question_carries_its_own_answers_and_ends_with_the_fire(void)
{
	pf_alarms_init();
	pf_alarms_raise("RECIPE:left_running", "W11_RECIPE_LEFT_RUNNING", "Grill Still Running",
	                PF_CRIT_HIGH, PF_SINK_ALL, "The grill is still running", "It is still in Hold.");
	pf_alarms_offer("RECIPE:left_running", "shutdown", 3600);

	cJSON *all = pf_alarms_json();
	cJSON *a = find_alarm(all, "W11_RECIPE_LEFT_RUNNING");
	TEST_ASSERT_NOT_NULL(a);
	TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(a, "active")));
	/* the remedy and the snooze travel with it, so the app can offer both where it is read */
	TEST_ASSERT_EQUAL_STRING("shutdown", pf_json_str(a, "fix", ""));
	TEST_ASSERT_EQUAL_DOUBLE(3600, pf_json_num(a, "snooze_s", 0));
	cJSON_Delete(all);

	TEST_ASSERT_EQUAL_INT(0, pf_alarms_shelve("RECIPE:left_running", 3600));
	all = pf_alarms_json();
	a = find_alarm(all, "W11_RECIPE_LEFT_RUNNING");
	TEST_ASSERT_TRUE(pf_json_num(a, "shelved_for", 0) > 3500);
	cJSON_Delete(all);

	/* the grill goes out well inside the hour: the question is moot and the snooze goes with it */
	pf_alarms_clear("RECIPE:left_running");
	all = pf_alarms_json();
	TEST_ASSERT_NULL(find_alarm(all, "W11_RECIPE_LEFT_RUNNING"));
	cJSON_Delete(all);
}

static bool warns(const char *steps_json, const char *code)
{
	cJSON *steps = cJSON_Parse(steps_json);
	cJSON *w = pf_recipe_shape_warnings(steps), *e;
	bool found = false;
	cJSON_ArrayForEach(e, w) if (!strcmp(pf_json_str(e, "code", ""), code)) found = true;
	cJSON_Delete(w); cJSON_Delete(steps);
	return found;
}

/* A recipe lights the grill before it cooks and puts it out when it is done. Both are said rather
 * than enforced, and the case that decides why is the cool-down: a recipe of nothing but Shutdown
 * must not be told to add a step that lights the grill. */
static void test_a_recipe_is_told_when_it_does_not_light_the_grill_first(void)
{
	TEST_ASSERT_TRUE(warns("[{\"mode\":\"Hold\",\"setpoint\":225}]", "no_startup"));
	TEST_ASSERT_TRUE(warns("[{\"mode\":\"Smoke\"},{\"mode\":\"Shutdown\"}]", "no_startup"));
	TEST_ASSERT_FALSE(warns("[{\"mode\":\"Startup\"},{\"mode\":\"Hold\",\"setpoint\":225}]", "no_startup"));
	/* a cool-down cooks nothing, so there is nothing to light and nothing to say */
	TEST_ASSERT_FALSE(warns("[{\"mode\":\"Shutdown\"}]", "no_startup"));
	TEST_ASSERT_FALSE(warns("[{\"mode\":\"Shutdown\"},{\"mode\":\"Stop\"}]", "no_startup"));
	TEST_ASSERT_FALSE(warns("[]", "no_startup"));
}

static void test_a_recipe_is_told_when_it_leaves_the_grill_running(void)
{
	TEST_ASSERT_TRUE(warns("[{\"mode\":\"Startup\"},{\"mode\":\"Hold\",\"setpoint\":225}]", "no_shutdown"));
	TEST_ASSERT_FALSE(warns("[{\"mode\":\"Startup\"},{\"mode\":\"Hold\"},{\"mode\":\"Shutdown\"}]", "no_shutdown"));
	/* a cool-down ends in Shutdown by definition, so it is not warned about either */
	TEST_ASSERT_FALSE(warns("[{\"mode\":\"Shutdown\"}]", "no_shutdown"));
	TEST_ASSERT_FALSE(warns("[]", "no_shutdown"));
}

/* The one that ships has both ends. */
static void test_the_built_in_ribs_recipe_has_nothing_wrong_with_its_shape(void)
{
	pf_recipes_seed();
	cJSON *l = pf_recipes_list();
	cJSON *r = cJSON_GetArrayItem(l, 0);
	cJSON *w = pf_recipe_shape_warnings(cJSON_GetObjectItem(r, "steps"));
	TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(w));
	cJSON_Delete(w); cJSON_Delete(l);
}

/* The two signals that can end a step, and how they join -- the shape a condition has. There is
 * deliberately no lid-only: the prompt is always one of them, because a lid switch that does not
 * fire would otherwise strand a recipe with no way to carry on. */
static void test_a_step_can_end_on_the_lid_the_prompt_or_both(void)
{
	int id = save("{\"name\":\"ends\",\"units\":\"F\",\"steps\":["
	              "{\"mode\":\"Hold\",\"setpoint\":225,\"wait\":\"confirm\"},"
	              "{\"mode\":\"Hold\",\"setpoint\":225,\"wait\":\"lid\"},"
	              "{\"mode\":\"Hold\",\"setpoint\":225,\"wait\":\"lid_and\"},"
	              "{\"mode\":\"Hold\",\"setpoint\":225}]}");
	pf_recipe r;
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(id, &r));
	TEST_ASSERT_EQUAL_INT(PF_RSTEP_WAIT_CONFIRM, r.steps[0].wait);
	TEST_ASSERT_EQUAL_INT(PF_RSTEP_WAIT_LID, r.steps[1].wait);
	TEST_ASSERT_EQUAL_INT(PF_RSTEP_WAIT_LID_AND, r.steps[2].wait);
	TEST_ASSERT_EQUAL_INT(PF_RSTEP_WAIT_NONE, r.steps[3].wait);
	/* every one of them that waits also pauses, whichever signal ends it */
	TEST_ASSERT_TRUE(r.steps[0].pause);
	TEST_ASSERT_TRUE(r.steps[1].pause);
	TEST_ASSERT_TRUE(r.steps[2].pause);
	TEST_ASSERT_FALSE(r.steps[3].pause);
	/* anything unrecognised waits for nothing rather than silently waiting for something */
	id = save("{\"name\":\"odd\",\"units\":\"F\",\"steps\":[{\"mode\":\"Hold\",\"wait\":\"whenever\"}]}");
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(id, &r));
	TEST_ASSERT_EQUAL_INT(PF_RSTEP_WAIT_NONE, r.steps[0].wait);
}

/* A step written before endings were conditions still runs, because it is turned into the tree
 * that says the same thing: (the clock OR the meat) AND (you said so [or the lid did]). */
static void test_an_older_step_becomes_the_condition_it_always_meant(void)
{
	int id = save("{\"name\":\"old\",\"units\":\"F\",\"steps\":[{\"mode\":\"Hold\",\"setpoint\":225,"
	              "\"timer_min\":90,\"probe\":\"@food\",\"probe_temp\":203,\"wait\":\"lid\"}]}");
	pf_recipe r;
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(id, &r));
	cJSON *e = ends_of(&r.steps[0]);
	TEST_ASSERT_TRUE(has_term(e, "elapsed", ">=", 90 * 60, true));
	TEST_ASSERT_TRUE(has_term(e, "food_max", ">=", pf_f_to_c(203), true));
	TEST_ASSERT_TRUE(has_term(e, "prompt", "is_on", 0, false));
	TEST_ASSERT_TRUE(has_term(e, "lid", "is_on", 0, false));
	cJSON_Delete(e);

	/* every food probe rather than any one of them */
	id = save("{\"name\":\"all\",\"units\":\"F\",\"steps\":[{\"mode\":\"Hold\",\"setpoint\":225,"
	          "\"probe\":\"@food\",\"probe_temp\":205,\"probe_match\":\"all\"}]}");
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(id, &r));
	e = ends_of(&r.steps[0]);
	TEST_ASSERT_TRUE(has_term(e, "food_min", ">=", pf_f_to_c(205), true));
	cJSON_Delete(e);

	/* where the meat ends up rather than where it came off */
	id = save("{\"name\":\"rested\",\"units\":\"F\",\"steps\":[{\"mode\":\"Hold\",\"setpoint\":225,"
	          "\"probe\":\"@food\",\"probe_temp\":205,\"carryover\":true}]}");
	TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(id, &r));
	e = ends_of(&r.steps[0]);
	TEST_ASSERT_TRUE(has_term(e, "food_rested", ">=", pf_f_to_c(205), true));
	cJSON_Delete(e);
}

/* A step's temperatures belong to the unit the recipe was written in, and end up in Celsius --
 * which is what the control loop thinks in and what the facts they are compared against are built
 * in. What the grill happens to be displaying never comes into it, so the same recipe means the
 * same thing on a grill set to either. */
static void test_a_condition_is_read_in_the_unit_it_was_written_in(void)
{
	int f = save("{\"name\":\"F\",\"units\":\"F\",\"steps\":[{\"mode\":\"Hold\",\"setpoint\":225,"
	             "\"ends\":{\"trait\":\"food_max\",\"op\":\">=\",\"value\":203}}]}");
	int cc = save("{\"name\":\"C\",\"units\":\"C\",\"steps\":[{\"mode\":\"Hold\",\"setpoint\":107,"
	              "\"ends\":{\"trait\":\"food_max\",\"op\":\">=\",\"value\":95}}]}");
	pf_recipe r;
	for (int pass = 0; pass < 2; pass++) {
		/* the grill switches units between the passes and neither recipe changes meaning */
		pf_settings_patch("globals", pass ? "{\"units\":\"C\"}" : "{\"units\":\"F\"}", NULL, 0);
		TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(f, &r));
		cJSON *e = ends_of(&r.steps[0]);
		TEST_ASSERT_TRUE(has_term(e, "food_max", ">=", pf_f_to_c(203), true));
		cJSON_Delete(e);
		TEST_ASSERT_EQUAL_INT(0, pf_recipe_load(cc, &r));
		e = ends_of(&r.steps[0]);
		TEST_ASSERT_TRUE(has_term(e, "food_max", ">=", 95, true));
		cJSON_Delete(e);
	}
	pf_settings_patch("globals", "{\"units\":\"F\"}", NULL, 0);
}

int main(void)
{
	pf_log_init(PF_LOG_ERROR);
	UNITY_BEGIN();
	RUN_TEST(test_the_built_in_ribs_recipe_loads_as_it_was_written);
	RUN_TEST(test_an_older_step_becomes_the_condition_it_always_meant);
	RUN_TEST(test_a_condition_is_read_in_the_unit_it_was_written_in);
	RUN_TEST(test_the_built_in_recipe_is_added_once);
	RUN_TEST(test_a_recipe_keeps_its_units_when_the_grill_changes_its_own);
	RUN_TEST(test_an_older_recipe_still_waits_where_it_used_to);
	RUN_TEST(test_carryover_follows_the_rate_the_probe_is_climbing_at);
	RUN_TEST(test_a_step_can_be_aimed_at_any_food_probe);
	RUN_TEST(test_a_step_with_a_mode_the_grill_does_not_have_is_refused);
	RUN_TEST(test_which_modes_count_as_a_grill_that_is_already_lit);
	RUN_TEST(test_a_step_can_end_on_the_lid_the_prompt_or_both);
	RUN_TEST(test_a_recipe_is_told_when_it_does_not_light_the_grill_first);
	RUN_TEST(test_a_recipe_is_told_when_it_leaves_the_grill_running);
	RUN_TEST(test_the_built_in_ribs_recipe_has_nothing_wrong_with_its_shape);
	RUN_TEST(test_the_still_running_question_carries_its_own_answers_and_ends_with_the_fire);
	return UNITY_END();
}
