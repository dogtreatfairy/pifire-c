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
	TEST_ASSERT_EQUAL_DOUBLE(550, pf_set_num("safety.maxtemp", 0));
	TEST_ASSERT_EQUAL_DOUBLE(0.1, pf_set_num("cycle_data.u_min", 0));
	TEST_ASSERT_EQUAL(PF_UNITS_F, pf_settings_units());
	char s[16];
	TEST_ASSERT_TRUE(pf_set_str("controller.selected", s, sizeof s, ""));
	TEST_ASSERT_EQUAL_STRING("pid", s);
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
	TEST_ASSERT_EQUAL_DOUBLE(288, pf_set_num("safety.maxtemp", 0));     /* 550F -> 287.8C */
	TEST_ASSERT_EQUAL_DOUBLE(74, pf_set_num("keep_warm.temp", 0));      /* 165F -> 73.9C */
	TEST_ASSERT_EQUAL_DOUBLE(7, pf_set_num("safety.coldstart.delta_rise", 0)); /* 12F delta -> 6.7C */
	TEST_ASSERT_EQUAL_INT(0, pf_settings_set_units(PF_UNITS_F));
	TEST_ASSERT_EQUAL_DOUBLE(550, pf_set_num("safety.maxtemp", 0));
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

int main(void)
{
	pf_log_init(PF_LOG_ERROR);
	UNITY_BEGIN();
	RUN_TEST(test_defaults_loaded);
	RUN_TEST(test_patch_and_reload);
	RUN_TEST(test_units_conversion);
	RUN_TEST(test_put_creates_path);
	return UNITY_END();
}
