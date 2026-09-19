#include "core/cycle.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_brief_example(void)
{
	/* cycle 10 s, u_min 0.1, u_max 0.8 -> auger on 1 s .. 8 s per cycle */
	pf_cycle_cfg cfg = { .cycle_s = 10, .u_min = 0.1, .u_max = 0.8, .max_on_s = 60 };
	pf_cycle c;
	pf_cycle_begin(&c, &cfg, 0, 0.0);
	TEST_ASSERT_EQUAL_DOUBLE(1.0, c.on_s);
	TEST_ASSERT_EQUAL_INT(-1, c.saturated);
	pf_cycle_begin(&c, &cfg, 0, 1.0);
	TEST_ASSERT_EQUAL_DOUBLE(8.0, c.on_s);
	TEST_ASSERT_EQUAL_INT(1, c.saturated);
	pf_cycle_begin(&c, &cfg, 100, 0.5);
	TEST_ASSERT_EQUAL_DOUBLE(5.0, c.on_s);
	TEST_ASSERT_EQUAL_INT(0, c.saturated);
	TEST_ASSERT_TRUE(pf_cycle_auger_on(&c, 100.0));
	TEST_ASSERT_TRUE(pf_cycle_auger_on(&c, 104.9));
	TEST_ASSERT_FALSE(pf_cycle_auger_on(&c, 105.1));
	TEST_ASSERT_FALSE(pf_cycle_done(&c, 109.9));
	TEST_ASSERT_TRUE(pf_cycle_done(&c, 110.0));
}

static void test_absolute_cap(void)
{
	pf_cycle_cfg cfg = { .cycle_s = 120, .u_min = 0.1, .u_max = 0.9, .max_on_s = 60 };
	pf_cycle c;
	pf_cycle_begin(&c, &cfg, 0, 0.9);
	TEST_ASSERT_EQUAL_DOUBLE(60.0, c.on_s);
	TEST_ASSERT_EQUAL_DOUBLE(0.5, c.u_applied);
	TEST_ASSERT_EQUAL_INT(1, c.saturated);
}

static void test_nan_falls_to_min(void)
{
	pf_cycle_cfg cfg = { .cycle_s = 25, .u_min = 0.1, .u_max = 0.9, .max_on_s = 60 };
	pf_cycle c;
	pf_cycle_begin(&c, &cfg, 0, 0.0 / 0.0);
	TEST_ASSERT_EQUAL_DOUBLE(2.5, c.on_s);
}

static void test_fixed(void)
{
	pf_cycle_cfg cfg = { .cycle_s = 25, .u_min = 0.1, .u_max = 0.9, .max_on_s = 60 };
	pf_cycle c;
	pf_cycle_begin_fixed(&c, &cfg, 0, 15, 65); /* smoke: 15 on, 45+2*10 off */
	TEST_ASSERT_EQUAL_DOUBLE(15.0, c.on_s);
	TEST_ASSERT_TRUE(pf_cycle_auger_on(&c, 14));
	TEST_ASSERT_FALSE(pf_cycle_auger_on(&c, 16));
	TEST_ASSERT_FALSE(pf_cycle_done(&c, 79));
	TEST_ASSERT_TRUE(pf_cycle_done(&c, 80));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_brief_example);
	RUN_TEST(test_absolute_cap);
	RUN_TEST(test_nan_falls_to_min);
	RUN_TEST(test_fixed);
	return UNITY_END();
}
