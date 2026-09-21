#include "pifire/common.h"
#include "probes/shh.h"
#include "probes/tempq.h"
#include "unity.h"
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

/* PT-1000-Ideal profile from probes.json */
static const pf_shh pt1000 = { 0.05966017913127897, -0.01048654943772497, 4.987389532180097e-05 };
/* TWPS00 (Thermoworks Pro) */
static const pf_shh twps = { 0.00073431401, 0.0002157437, 9.515686e-08 };

static void test_roundtrip(void)
{
	for (double c = 20; c <= 300; c += 20) {
		double r = pf_shh_c_to_ohms(c, &pt1000);
		double back = pf_shh_ohms_to_c(r, &pt1000);
		TEST_ASSERT_DOUBLE_WITHIN(0.05, c, back);
	}
	for (double c = 20; c <= 120; c += 10) {
		double r = pf_shh_c_to_ohms(c, &twps);
		TEST_ASSERT_DOUBLE_WITHIN(0.05, c, pf_shh_ohms_to_c(r, &twps));
	}
}

static void test_divider(void)
{
	/* 10k divider, 3.28 V: 10k probe -> 1.64 V midpoint */
	TEST_ASSERT_DOUBLE_WITHIN(1, 10000, pf_shh_mv_to_ohms(1640, 10000, 3.28));
	TEST_ASSERT_TRUE(pf_shh_mv_to_ohms(0, 10000, 3.28) < 0);          /* short */
	TEST_ASSERT_TRUE(pf_shh_mv_to_ohms(3400, 10000, 3.28) < 0);       /* above reference */
	TEST_ASSERT_TRUE(pf_shh_mv_to_ohms(3279, 10000, 3.28) > 1e6);     /* open -> huge */
}

static void test_sanity_clamp(void)
{
	TEST_ASSERT_TRUE(isnan(pf_shh_ohms_to_c(1e12, &twps)));  /* below 0 F */
}

static void test_solve(void)
{
	double t1 = 25, t2 = 100, t3 = 200;
	double r1 = pf_shh_c_to_ohms(t1, &pt1000), r2 = pf_shh_c_to_ohms(t2, &pt1000), r3 = pf_shh_c_to_ohms(t3, &pt1000);
	pf_shh out;
	TEST_ASSERT_EQUAL_INT(0, pf_shh_solve(t1, r1, t2, r2, t3, r3, &out));
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, pt1000.A, out.A);
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, pt1000.B, out.B);
	TEST_ASSERT_DOUBLE_WITHIN(1e-8, pt1000.C, out.C);
}

static void test_tempq_rejects_spike(void)
{
	pf_tempq q;
	pf_tempq_init(&q);
	double v = 0;
	for (int i = 0; i < 12; i++) v = pf_tempq_push(&q, 100.0);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 100.0, v);

	/* one wrong reading, then the truth again: the median never lets it reach the average */
	v = pf_tempq_push(&q, 140.0);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 100.0, v);
	for (int i = 0; i < 4; i++) v = pf_tempq_push(&q, 100.0);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 100.0, v);

	/* two wrong readings in a row are still outnumbered inside the median window */
	v = pf_tempq_push(&q, 300.0);
	v = pf_tempq_push(&q, 295.0);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 100.0, v);
	for (int i = 0; i < 6; i++) v = pf_tempq_push(&q, 100.0);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 100.0, v);

	/* slow real change tracks */
	for (int i = 1; i <= 10; i++) v = pf_tempq_push(&q, 100.0 + i * 0.5);
	TEST_ASSERT_TRUE(v > 101.5);
}

/* The startup ramp is the fastest thing a grill really does, and the old filter stopped updating
 * while it happened because the climb itself blew past its standard-deviation gate. It must track
 * the ramp, and must still drop a spike sitting on top of one. */
static void test_tempq_tracks_a_fast_ramp(void)
{
	pf_tempq q;
	pf_tempq_init(&q);
	double v = 0;
	for (int i = 0; i < 12; i++) v = pf_tempq_push(&q, 40.0);

	double truth = 40.0;
	for (int i = 0; i < 60; i++) {           /* 3 C per sample: about 5 F a second at 4 Hz */
		truth += 3.0;
		v = pf_tempq_push(&q, truth);
	}
	/* it follows, a little behind: the lag is the two filter stages, not a freeze */
	TEST_ASSERT_TRUE_MESSAGE(v > truth - 25.0, "the filter should track a fast ramp, not stall");
	TEST_ASSERT_TRUE_MESSAGE(v < truth, "the filter should lag slightly, never lead");
	double before = v;

	/* a spike on top of the ramp is dropped, and the ramp carries on */
	truth += 3.0;
	v = pf_tempq_push(&q, truth + 120.0);
	TEST_ASSERT_TRUE_MESSAGE(v < before + 10.0, "a spike during a ramp must not reach the output");
	for (int i = 0; i < 5; i++) { truth += 3.0; v = pf_tempq_push(&q, truth); }
	TEST_ASSERT_TRUE(v > before);
	TEST_ASSERT_TRUE(v < truth);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_roundtrip);
	RUN_TEST(test_divider);
	RUN_TEST(test_sanity_clamp);
	RUN_TEST(test_solve);
	RUN_TEST(test_tempq_rejects_spike);
	RUN_TEST(test_tempq_tracks_a_fast_ramp);
	return UNITY_END();
}
