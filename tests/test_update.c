/* The updater's pure pieces: version order, branch tags, apt's listing, the install window, and
 * the streaming runner the console is fed from. */
#include "core/util.h"
#include "features/update.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_version_order(void)
{
	TEST_ASSERT_TRUE(pf_version_compare("v0.1.0-alpha.173", "0.1.0-alpha.172") > 0);
	TEST_ASSERT_TRUE(pf_version_compare("0.1.0-alpha.9", "0.1.0-alpha.10") < 0);
	TEST_ASSERT_TRUE(pf_version_compare("0.1.0", "0.1.0-rc.1") > 0);
	TEST_ASSERT_EQUAL_INT(0, pf_version_compare("0.1.0-alpha.172-3-gabc1234", "0.1.0-alpha.172-3-gabc1234"));
	/* a branch release tag never looks newer than a real release to an old daemon */
	TEST_ASSERT_TRUE(pf_version_compare("branch-relay-centring", "v0.1.0-alpha.1") < 0);
}

static void test_branch_slug(void)
{
	char s[64];
	pf_update_branch_slug("feature/new ui", s, sizeof s);
	TEST_ASSERT_EQUAL_STRING("feature-new-ui", s);
	pf_update_branch_slug("relay-centring", s, sizeof s);
	TEST_ASSERT_EQUAL_STRING("relay-centring", s);
}

static void test_apt_line(void)
{
	char n[96], to[64], from[64];
	TEST_ASSERT_EQUAL_INT(0, pf_update_parse_apt_line("libc6/stable-security 2.36-9+deb12u9 arm64 [upgradable from: 2.36-9+deb12u8]", n, sizeof n, to, sizeof to, from, sizeof from));
	TEST_ASSERT_EQUAL_STRING("libc6", n);
	TEST_ASSERT_EQUAL_STRING("2.36-9+deb12u9", to);
	TEST_ASSERT_EQUAL_STRING("2.36-9+deb12u8", from);
	TEST_ASSERT_EQUAL_INT(-1, pf_update_parse_apt_line("Listing...", n, sizeof n, to, sizeof to, from, sizeof from));
	TEST_ASSERT_EQUAL_INT(-1, pf_update_parse_apt_line("", n, sizeof n, to, sizeof to, from, sizeof from));
}

static void test_window(void)
{
	unsigned sunday = 1u << 0, all = 0x7f;
	/* Sundays at 02:00 only */
	TEST_ASSERT_TRUE(pf_update_in_window(0, 2 * 60, 120, sunday));
	TEST_ASSERT_TRUE(pf_update_in_window(0, 2 * 60 + 59, 120, sunday));
	TEST_ASSERT_FALSE(pf_update_in_window(0, 3 * 60, 120, sunday));
	TEST_ASSERT_FALSE(pf_update_in_window(1, 2 * 60 + 5, 120, sunday));
	TEST_ASSERT_FALSE(pf_update_in_window(0, 60, 120, sunday));
	/* 02:30 is two thirty */
	TEST_ASSERT_FALSE(pf_update_in_window(3, 2 * 60 + 10, 150, all));
	TEST_ASSERT_TRUE(pf_update_in_window(3, 2 * 60 + 45, 150, all));
	/* 23:30 on Saturday runs into Sunday and still belongs to Saturday */
	TEST_ASSERT_TRUE(pf_update_in_window(0, 10, 23 * 60 + 30, 1u << 6));
	TEST_ASSERT_FALSE(pf_update_in_window(0, 10, 23 * 60 + 30, sunday));
	/* no days chosen reads as every day */
	TEST_ASSERT_TRUE(pf_update_in_window(4, 125, 120, 0));
}

static char got[8][64];
static int ngot;
static void collect(const char *t, void *ud) { (void)ud; if (ngot < 8) snprintf(got[ngot++], sizeof got[0], "%s", t); }

static void test_stream(void)
{
	const char *argv[] = { "sh", "-c", "echo one; printf 'two\\rthree\\n'; echo four >&2; exit 3", NULL };
	ngot = 0;
	int rc = pf_run_stream(argv, 10, collect, NULL);
	TEST_ASSERT_EQUAL_INT(3, rc);
	TEST_ASSERT_EQUAL_INT(4, ngot);
	TEST_ASSERT_EQUAL_STRING("one", got[0]);
	TEST_ASSERT_EQUAL_STRING("two", got[1]);
	TEST_ASSERT_EQUAL_STRING("three", got[2]);
	TEST_ASSERT_EQUAL_STRING("four", got[3]);
	const char *slow[] = { "sh", "-c", "sleep 5", NULL };
	TEST_ASSERT_EQUAL_INT(-2, pf_run_stream(slow, 1, collect, NULL));
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_version_order);
	RUN_TEST(test_branch_slug);
	RUN_TEST(test_apt_line);
	RUN_TEST(test_window);
	RUN_TEST(test_stream);
	return UNITY_END();
}
