/* Chef iQ advertisement parser against the documented layouts. */
#include "probes/ble/chefiq.h"
#include "unity.h"
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

static void test_v3_temperature_and_status(void)
{
	/* type 1, version 5.0.0: ambient 25.0, food 63.4, tips 60.1 62.0 63.4 64.0, mirror */
	const uint8_t t[] = { 0x01, 0x50, 0xFA, 0x00, 0x7A, 0x02, 0x59, 0x02, 0x6C, 0x02, 0x7A, 0x02, 0x80, 0x02, 0xFA, 0x00 };
	pf_chefiq_reading r;
	TEST_ASSERT_EQUAL_INT(0, pf_chefiq_parse(t, sizeof t, &r));
	TEST_ASSERT_TRUE(r.has_temps);
	TEST_ASSERT_EQUAL_INT(1, r.packet_type);
	TEST_ASSERT_EQUAL_INT(5, r.ver_major);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 25.0, r.ambient_c);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 63.4, r.food_c);
	TEST_ASSERT_EQUAL_INT(4, r.ntips);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 64.0, r.tip_c[3]);
	TEST_ASSERT_FALSE(r.has_battery);
	/* type 3 status: battery 87 %, soc 31 C */
	const uint8_t s[] = { 0x03, 0x50, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 87, 31 };
	TEST_ASSERT_EQUAL_INT(0, pf_chefiq_parse(s, sizeof s, &r));
	TEST_ASSERT_TRUE(r.has_battery);
	TEST_ASSERT_EQUAL_INT(87, r.battery_pct);
	TEST_ASSERT_FALSE(r.has_temps);
}

static void test_v2_legacy_and_rejects(void)
{
	/* V2: battery 50, soc 28, ambient 20.0, food 40.0, three tips */
	const uint8_t v2[] = { 0x01, 0x20, 50, 28, 0xC8, 0x00, 0x90, 0x01, 0x90, 0x01, 0x90, 0x01, 0x90, 0x01, 0xC8, 0x00 };
	pf_chefiq_reading r;
	TEST_ASSERT_EQUAL_INT(0, pf_chefiq_parse(v2, sizeof v2, &r));
	TEST_ASSERT_EQUAL_INT(50, r.battery_pct);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 40.0, r.food_c);
	TEST_ASSERT_EQUAL_INT(3, r.ntips);
	/* legacy: mac, battery 70, soc 25, food 55.5, ambient 22.2 */
	const uint8_t lg[] = { 0x01, 0x10, 1, 2, 3, 4, 5, 6, 70, 25, 0x2B, 0x02, 0xDE, 0x00, 0xDE, 0x00 };
	TEST_ASSERT_EQUAL_INT(0, pf_chefiq_parse(lg, sizeof lg, &r));
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 55.5, r.food_c);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 22.2, r.ambient_c);
	/* out-of-range sentinel is dropped, hub payload (too long) and unknown types are rejected */
	const uint8_t bad[] = { 0x01, 0x50, 0xFA, 0x00, 0xFF, 0x7F };
	TEST_ASSERT_EQUAL_INT(0, pf_chefiq_parse(bad, sizeof bad, &r));
	TEST_ASSERT_TRUE(isnan(r.food_c));
	uint8_t hub[24] = { 0x01, 0x50 };
	TEST_ASSERT_EQUAL_INT(-1, pf_chefiq_parse(hub, sizeof hub, &r));
	const uint8_t odd[] = { 0x07, 0x50, 0, 0 };
	TEST_ASSERT_EQUAL_INT(-1, pf_chefiq_parse(odd, sizeof odd, &r));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_v3_temperature_and_status);
	RUN_TEST(test_v2_legacy_and_rejects);
	return UNITY_END();
}
