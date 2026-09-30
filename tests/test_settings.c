#include "core/log.h"
#include "core/settings.h"
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char path[256];

void setUp(void)
{
	snprintf(path, sizeof path, "/tmp/pifire_test_settings_%d.json", (int)getpid());
	unlink(path);
	TEST_ASSERT_EQUAL_INT(0, pf_settings_init(path));
}
void tearDown(void)
{
	pf_settings_shutdown();
	unlink(path);
}

static void test_defaults_loaded(void)
{
	TEST_ASSERT_EQUAL_DOUBLE(650, pf_set_num("safety.maxtemp", 0));
	TEST_ASSERT_EQUAL_DOUBLE(0.1, pf_set_num("cycle_data.u_min", 0));
	TEST_ASSERT_EQUAL(PF_UNITS_F, pf_settings_units());
	char s[16];
	TEST_ASSERT_TRUE(pf_set_str("controller.selected", s, sizeof s, ""));
	TEST_ASSERT_EQUAL_STRING("adaptive", s);
}

static void test_patch_and_reload(void)
{
	char err[128] = "";
	TEST_ASSERT_EQUAL_INT(0, pf_settings_patch("safety", "{\"maxtemp\":600}", err, sizeof err));
	TEST_ASSERT_EQUAL_DOUBLE(600, pf_set_num("safety.maxtemp", 0));
	/* invalid: u_min > u_max */
	TEST_ASSERT_NOT_EQUAL(0, pf_settings_patch("cycle_data", "{\"u_min\":0.95}", err, sizeof err));
	TEST_ASSERT_EQUAL_DOUBLE(0.1, pf_set_num("cycle_data.u_min", 0));
	/* persisted across reload */
	pf_settings_shutdown();
	TEST_ASSERT_EQUAL_INT(0, pf_settings_init(path));
	TEST_ASSERT_EQUAL_DOUBLE(600, pf_set_num("safety.maxtemp", 0));
}

static void test_units_conversion(void)
{
	TEST_ASSERT_EQUAL_INT(0, pf_settings_set_units(PF_UNITS_C));
	TEST_ASSERT_EQUAL(PF_UNITS_C, pf_settings_units());
	TEST_ASSERT_EQUAL_DOUBLE(343, pf_set_num("safety.maxtemp", 0));     /* 650F -> 343.3C */
	TEST_ASSERT_EQUAL_DOUBLE(74, pf_set_num("keep_warm.temp", 0));      /* 165F -> 73.9C */
	TEST_ASSERT_EQUAL_DOUBLE(7, pf_set_num("startup.smartstart.exit_rise", 0)); /* 12F delta -> 6.7C */
	TEST_ASSERT_EQUAL_DOUBLE(2, pf_set_num("startup.smartstart.prove_rise", 0)); /* 3F delta -> 1.7C */
	TEST_ASSERT_EQUAL_DOUBLE(11, pf_set_num("safety.relight_drop", 0));        /* 20F delta -> 11.1C */
	TEST_ASSERT_EQUAL_DOUBLE(71, pf_set_num("safety.min_target", 0));
	TEST_ASSERT_EQUAL_DOUBLE(82, pf_set_num("safety.smoke_min", 0));
	TEST_ASSERT_EQUAL_INT(0, pf_settings_set_units(PF_UNITS_F));
	/* whole degrees each way: 650 F is 343.3 C, stored as 343, which is 649.4 F */
	TEST_ASSERT_DOUBLE_WITHIN(1.0, 650, pf_set_num("safety.maxtemp", 0));
}

static void test_put_creates_path(void)
{
	TEST_ASSERT_EQUAL_INT(0, pf_set_put_num("controller.config.pid.PB", 42));
	TEST_ASSERT_EQUAL_DOUBLE(42, pf_set_num("controller.config.pid.PB", 0));
	cJSON *d = pf_set_dup("controller.config");
	TEST_ASSERT_NOT_NULL(d);
	TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(d, "pid"));
	cJSON_Delete(d);
}

static void test_igniter_never_over_five_minutes(void)
{
	char err[128] = "";
	TEST_ASSERT_EQUAL_DOUBLE(300, pf_set_num("safety.igniter_max_on_s", 0));
	TEST_ASSERT_NOT_EQUAL(0, pf_settings_patch("safety", "{\"igniter_max_on_s\":301}", err, sizeof err));
	TEST_ASSERT_NOT_EQUAL(0, pf_settings_patch("startup", "{\"smartstart\":{\"prove_s\":301}}", err, sizeof err));
	TEST_ASSERT_EQUAL_INT(0, pf_settings_patch("safety", "{\"igniter_max_on_s\":240}", err, sizeof err));
	/* a file from before the ceiling, at the old 600 s cap, comes down to five minutes */
	pf_settings_shutdown();
	FILE *f = fopen(path, "w");
	TEST_ASSERT_NOT_NULL(f);
	fputs("{\"schema_version\":24,\"safety\":{\"igniter_max_on_s\":600,\"power_loss\":{\"igniter_s\":600}}}", f);
	fclose(f);
	TEST_ASSERT_EQUAL_INT(0, pf_settings_init(path));
	TEST_ASSERT_EQUAL_DOUBLE(300, pf_set_num("safety.igniter_max_on_s", 0));
	TEST_ASSERT_EQUAL_DOUBLE(-1, pf_set_num("safety.power_loss.igniter_s", -1));   /* the relight is Smart Start now */
}

/* Schema 29: startup's three answers to "is it lit" become Smart Start, a user's own timeout and
 * rise carry over, the overheat limit that shipped moves to 650 F, and the set point has limits. */
static void test_smart_start_migration_and_limits(void)
{
	pf_settings_shutdown();
	FILE *f = fopen(path, "w");
	TEST_ASSERT_NOT_NULL(f);
	fputs("{\"schema_version\":28,\"safety\":{\"maxtemp\":550,\"minstartuptemp\":75,\"startup_check\":true,"
	      "\"coldstart\":{\"enabled\":true,\"delta_rise\":15,\"timeout_s\":240},\"power_loss\":{\"recovery\":true,\"igniter_s\":180},"
	      "\"relight_timeout_s\":300,\"relight_recover\":10},"
	      "\"startup\":{\"duration\":240,\"startup_exit_temp\":140,\"smartstart\":{\"enabled\":true,\"exit_temp\":120,"
	      "\"profiles\":[{\"startuptime\":360,\"augerontime\":15,\"p_mode\":0}]}},"
	      "\"smoke_plus\":{\"min_temp\":160,\"max_temp\":220},"
	      "\"notify\":{\"rules\":[{\"id\":\"mine\",\"when\":{\"conditions\":[{\"trait\":\"mode\",\"value\":\"Reignite\"}]}}]}}", f);
	fclose(f);
	TEST_ASSERT_EQUAL_INT(0, pf_settings_init(path));
	TEST_ASSERT_EQUAL_DOUBLE(650, pf_set_num("safety.maxtemp", 0));
	TEST_ASSERT_EQUAL_DOUBLE(160, pf_set_num("safety.min_target", 0));
	TEST_ASSERT_EQUAL_DOUBLE(550, pf_set_num("safety.max_target", 0));
	TEST_ASSERT_EQUAL_DOUBLE(240, pf_set_num("startup.smartstart.prove_s", 0));
	TEST_ASSERT_EQUAL_DOUBLE(3, pf_set_num("startup.smartstart.prove_rise", 0));
	TEST_ASSERT_EQUAL_DOUBLE(15, pf_set_num("startup.smartstart.exit_rise", 0));
	TEST_ASSERT_EQUAL_DOUBLE(180, pf_set_num("safety.relight_prove_s", 0));
	const char *gone[] = { "safety.minstartuptemp", "safety.startup_check", "safety.coldstart.timeout_s", "safety.power_loss.igniter_s",
	                       "safety.relight_timeout_s", "safety.relight_recover", "startup.duration", "startup.startup_exit_temp",
	                       "startup.smartstart.enabled", "startup.smartstart.exit_temp", "startup.smartstart.profiles.0.startuptime" };
	for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++) TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(-1, pf_set_num(gone[i], -1), gone[i]);
	TEST_ASSERT_EQUAL_DOUBLE(-1, pf_set_num("smoke_plus.min_temp", -1));   /* schema 30 */
	TEST_ASSERT_EQUAL_DOUBLE(220, pf_set_num("smoke_plus.max_temp", 0));
	char mode[16] = "";
	pf_set_str("notify.rules.0.when.conditions.0.value", mode, sizeof mode, "");
	TEST_ASSERT_EQUAL_STRING("Relight", mode);
	/* the limits refuse what is outside them */
	char err[160] = "";
	TEST_ASSERT_NOT_EQUAL(0, pf_settings_patch("safety", "{\"max_target\":650}", err, sizeof err));
	TEST_ASSERT_NOT_EQUAL(0, pf_settings_patch("safety", "{\"min_target\":600}", err, sizeof err));
	TEST_ASSERT_NOT_EQUAL(0, pf_settings_patch("startup", "{\"smartstart\":{\"exit_rise\":2}}", err, sizeof err));
	TEST_ASSERT_EQUAL_INT(0, pf_settings_patch("safety", "{\"min_target\":180}", err, sizeof err));
}

/* the same file on a Celsius grill: the new temperatures arrive converted */
static void test_smart_start_migration_celsius(void)
{
	pf_settings_shutdown();
	FILE *f = fopen(path, "w");
	TEST_ASSERT_NOT_NULL(f);
	fputs("{\"schema_version\":28,\"globals\":{\"units\":\"C\"},\"safety\":{\"maxtemp\":288}}", f);
	fclose(f);
	TEST_ASSERT_EQUAL_INT(0, pf_settings_init(path));
	TEST_ASSERT_EQUAL_DOUBLE(343, pf_set_num("safety.maxtemp", 0));
	TEST_ASSERT_EQUAL_DOUBLE(71, pf_set_num("safety.min_target", 0));
	TEST_ASSERT_EQUAL_DOUBLE(288, pf_set_num("safety.max_target", 0));
	TEST_ASSERT_EQUAL_DOUBLE(2, pf_set_num("startup.smartstart.prove_rise", 0));
	TEST_ASSERT_EQUAL_DOUBLE(7, pf_set_num("startup.smartstart.exit_rise", 0));
}

/* a password generated by alpha.189 goes back to the standard one; one the owner chose stays */
static void test_generated_hotspot_password_is_rolled_back(void)
{
	const char *cases[][2] = { { "k7mq2xhz9p", "pifire1234" }, { "MyGrill2024", "MyGrill2024" } };
	for (int i = 0; i < 2; i++) {
		pf_settings_shutdown();
		FILE *f = fopen(path, "w");
		TEST_ASSERT_NOT_NULL(f);
		fprintf(f, "{\"schema_version\":26,\"network\":{\"hotspot_password\":\"%s\"}}", cases[i][0]);
		fclose(f);
		TEST_ASSERT_EQUAL_INT(0, pf_settings_init(path));
		char pw[64];
		pf_set_str("network.hotspot_password", pw, sizeof pw, "");
		TEST_ASSERT_EQUAL_STRING(cases[i][1], pw);
	}
}

int main(void)
{
	pf_log_init(PF_LOG_ERROR);
	UNITY_BEGIN();
	RUN_TEST(test_defaults_loaded);
	RUN_TEST(test_patch_and_reload);
	RUN_TEST(test_units_conversion);
	RUN_TEST(test_put_creates_path);
	RUN_TEST(test_igniter_never_over_five_minutes);
	RUN_TEST(test_generated_hotspot_password_is_rolled_back);
	RUN_TEST(test_smart_start_migration_and_limits);
	RUN_TEST(test_smart_start_migration_celsius);
	return UNITY_END();
}
