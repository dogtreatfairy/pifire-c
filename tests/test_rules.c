/* The conditional-notification engine: selection, conditions, hold times, edge firing and the
 * message templates. Events are captured through a sink instead of being sent anywhere. */
#include "core/events.h"
#include "core/settings.h"
#include "core/util.h"
#include "features/rules.h"
#include "unity.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char cfg_path[256];

/* ---- captured events ---- */
#define CAP_MAX 16
static struct { char code[40], title[160], body[320]; int crit; unsigned sinks; } g_cap[CAP_MAX];
static int g_ncap;

static void capture(const pf_event *e, void *ctx)
{
	(void)ctx;
	if (g_ncap >= CAP_MAX) return;
	snprintf(g_cap[g_ncap].code, sizeof g_cap[0].code, "%s", e->code);
	snprintf(g_cap[g_ncap].title, sizeof g_cap[0].title, "%s", e->title);
	snprintf(g_cap[g_ncap].body, sizeof g_cap[0].body, "%s", e->body);
	g_cap[g_ncap].crit = e->crit;
	g_cap[g_ncap].sinks = e->sinks;
	g_ncap++;
}

static const char *STATUS =
"{\"units\":\"F\",\"mode\":\"Hold\",\"setpoint\":225,\"cook_elapsed\":3600,\"hopper_pct\":40,"
"\"outputs\":{\"auger\":true,\"fan\":true,\"igniter\":false,\"power\":true,\"fan_pct\":100},"
"\"cycle\":{\"u_applied\":0.4,\"u_ff\":0.35},\"timers\":{\"mode_remaining\":0},"
"\"timer\":{\"running\":false,\"remaining\":0},\"safety\":{\"error_code\":\"\",\"error_msg\":\"\"},"
"\"probes\":["
 "{\"label\":\"Grill\",\"name\":\"Grill\",\"role\":\"Primary\",\"enabled\":true,\"valid\":true,\"temp\":227,\"target\":0},"
 "{\"label\":\"BT1\",\"name\":\"BT1\",\"role\":\"Food\",\"enabled\":true,\"valid\":true,\"temp\":150,\"target\":203,"
   "\"eta_s\":1800,\"wireless\":true,\"battery\":45,\"signal\":3},"
 "{\"label\":\"BT1Amb\",\"name\":\"BT1 Ambient\",\"role\":\"Food\",\"enabled\":true,\"valid\":true,\"temp\":221,"
   "\"target\":0,\"wireless\":true,\"companion\":true},"
 "{\"label\":\"Probe2\",\"name\":\"Probe 2\",\"role\":\"Food\",\"enabled\":true,\"valid\":true,\"temp\":195,\"target\":195}"
"]}";

void setUp(void)
{
	snprintf(cfg_path, sizeof cfg_path, "/tmp/pf_rules_%d.json", (int)getpid());
	unlink(cfg_path);
	TEST_ASSERT_EQUAL_INT(0, pf_settings_init(cfg_path));
	pf_events_init();
	pf_events_add_sink(capture, NULL);
	g_ncap = 0;
	pf_rules_init();
}

void tearDown(void)
{
	pf_settings_shutdown();
	unlink(cfg_path);
}

/* replace the shipped rules with just this one */
static void only_rule(const char *json)
{
	char buf[2048];
	snprintf(buf, sizeof buf, "{\"rules\":[%s]}", json);
	char err[160];
	TEST_ASSERT_EQUAL_INT(0, pf_settings_patch("notify", buf, err, sizeof err));
	pf_rules_init();
	g_ncap = 0;
}

static cJSON *status(void) { return cJSON_Parse(STATUS); }

/* a rule over every food probe fires once per probe that matches, naming the one that did */
static void test_class_rule_fires_per_probe(void)
{
	only_rule("{\"id\":\"t\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"probe\",\"role\":\"Food\",\"link\":\"any\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":["
	            "{\"trait\":\"target\",\"op\":\">\",\"value\":0},"
	            "{\"trait\":\"temp\",\"op\":\">=\",\"value\":{\"trait\":\"target\"}}]},"
	          "\"title\":\"{probe} reached {target}\",\"body\":\"{probe} is at {temp}.\","
	          "\"level\":\"high\",\"sinks\":[\"app\"],\"cooldown_s\":600}");
	cJSON *st = status();
	pf_rules_tick(st, 1000);
	/* Probe 2 is at its target, BT1 is not, and the ambient companion is never selected */
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_STRING("Probe 2 reached 195\xC2\xB0""F", g_cap[0].title);
	TEST_ASSERT_EQUAL_STRING("Probe 2 is at 195\xC2\xB0""F.", g_cap[0].body);
	TEST_ASSERT_EQUAL_INT(PF_CRIT_HIGH, g_cap[0].crit);
	TEST_ASSERT_EQUAL_UINT(PF_SINK_APP, g_cap[0].sinks);

	/* edge triggered: still true a second later, but it does not send again */
	pf_rules_tick(st, 1001);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	cJSON_Delete(st);
}

/* link and exclude narrow a class down */
static void test_selectors(void)
{
	only_rule("{\"id\":\"t\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"probe\",\"role\":\"any\",\"link\":\"bluetooth\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"temp\",\"op\":\">\",\"value\":0}]},"
	          "\"title\":\"{probe}\",\"body\":\"\",\"level\":\"info\",\"cooldown_s\":600}");
	cJSON *st = status();
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);                 /* only BT1: wired probes and the companion are out */
	TEST_ASSERT_EQUAL_STRING("BT1", g_cap[0].title);

	only_rule("{\"id\":\"t2\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"probe\",\"role\":\"Food\",\"link\":\"any\",\"match\":\"any\",\"exclude\":[\"BT1\"]},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"temp\",\"op\":\">\",\"value\":0}]},"
	          "\"title\":\"{probe}\",\"body\":\"\",\"level\":\"info\",\"cooldown_s\":600}");
	pf_rules_tick(st, 2000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_STRING("Probe 2", g_cap[0].title);
	cJSON_Delete(st);
}

/* a condition must hold for its duration before the rule sends */
static void test_hold_time(void)
{
	only_rule("{\"id\":\"t\",\"enabled\":true,\"only_while_cooking\":true,\"for_s\":10,"
	          "\"select\":{\"domain\":\"probe\",\"role\":\"any\",\"link\":\"bluetooth\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"battery\",\"op\":\"<\",\"value\":50}]},"
	          "\"title\":\"{probe} battery {battery}\",\"body\":\"\",\"level\":\"info\",\"cooldown_s\":600}");
	cJSON *st = status();
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);     /* true, but not for long enough */
	pf_rules_tick(st, 1005);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);
	pf_rules_tick(st, 1011);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_STRING("BT1 battery 45%", g_cap[0].title);
	cJSON_Delete(st);
}

/* any/all groups, and comparing a trait of another entity */
static void test_condition_groups(void)
{
	/* all: the grill must be holding AND the probe past 190 */
	only_rule("{\"id\":\"t\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"probe\",\"role\":\"Food\",\"link\":\"any\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":["
	            "{\"entity\":\"grill\",\"trait\":\"mode\",\"op\":\"is\",\"value\":\"Hold\"},"
	            "{\"trait\":\"temp\",\"op\":\">\",\"value\":190}]},"
	          "\"title\":\"{probe}\",\"body\":\"\",\"level\":\"info\",\"cooldown_s\":600}");
	cJSON *st = status();
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_STRING("Probe 2", g_cap[0].title);

	/* any: either of two thresholds is enough, so both probes qualify */
	only_rule("{\"id\":\"t2\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"probe\",\"role\":\"Food\",\"link\":\"any\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"any\",\"conditions\":["
	            "{\"trait\":\"temp\",\"op\":\">\",\"value\":190},"
	            "{\"trait\":\"battery\",\"op\":\"<\",\"value\":50}]},"
	          "\"title\":\"{probe}\",\"body\":\"\",\"level\":\"info\",\"cooldown_s\":600}");
	pf_rules_tick(st, 2000);
	TEST_ASSERT_EQUAL_INT(2, g_ncap);
	cJSON_Delete(st);
}

/* the predictive case the user asked for, and the duration token */
static void test_eta_rule_and_tokens(void)
{
	only_rule("{\"id\":\"t\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"probe\",\"role\":\"Food\",\"link\":\"any\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":["
	            "{\"trait\":\"eta\",\"op\":\">\",\"value\":0},"
	            "{\"trait\":\"eta\",\"op\":\"<=\",\"value\":1800}]},"
	          "\"title\":\"{probe} - {eta} to {target}\","
	          "\"body\":\"now {temp}, grill {grill_temp}, cook {cook_time}, hopper {hopper}\","
	          "\"level\":\"normal\",\"cooldown_s\":600}");
	cJSON *st = status();
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_STRING("BT1 - 30 min to 203\xC2\xB0""F", g_cap[0].title);
	TEST_ASSERT_EQUAL_STRING("now 150\xC2\xB0""F, grill 227\xC2\xB0""F, cook 1h 0m, hopper 40%", g_cap[0].body);
	cJSON_Delete(st);
}

/* a rule that only runs while cooking stays quiet when the grill is stopped */
static void test_only_while_cooking(void)
{
	only_rule("{\"id\":\"t\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"probe\",\"role\":\"Food\",\"link\":\"any\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"temp\",\"op\":\">\",\"value\":0}]},"
	          "\"title\":\"{probe}\",\"body\":\"\",\"level\":\"info\",\"cooldown_s\":600}");
	cJSON *st = status();
	cJSON_ReplaceItemInObject(st, "mode", cJSON_CreateString("Stop"));
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);
	cJSON_ReplaceItemInObject(st, "mode", cJSON_CreateString("Hold"));
	pf_rules_tick(st, 1001);
	TEST_ASSERT_TRUE(g_ncap > 0);
	cJSON_Delete(st);
}

/* the shipped rules load and the catalogue describes what the editor can offer */
static void test_builtin_rules_and_catalogue(void)
{
	cJSON *rules = pf_set_dup("notify.rules");
	TEST_ASSERT_TRUE(cJSON_IsArray(rules));
	TEST_ASSERT_TRUE(cJSON_GetArraySize(rules) >= 4);
	bool saw_target = false, saw_offline = false;
	cJSON *r;
	cJSON_ArrayForEach(r, rules) {
		const char *id = pf_json_str(r, "id", "");
		if (!strcmp(id, "probe-target")) { saw_target = true; TEST_ASSERT_EQUAL_STRING("high", pf_json_str(r, "level", "")); }
		if (!strcmp(id, "probe-offline")) saw_offline = true;
	}
	TEST_ASSERT_TRUE(saw_target);
	TEST_ASSERT_TRUE(saw_offline);
	cJSON_Delete(rules);

	cJSON *st = status();
	cJSON *cat = pf_rules_catalogue_json(st);
	cJSON *domains = cJSON_GetObjectItem(cat, "domains");
	TEST_ASSERT_TRUE(cJSON_GetArraySize(domains) >= 6);
	bool probe_has_eta = false;
	cJSON *d;
	cJSON_ArrayForEach(d, domains) {
		if (strcmp(pf_json_str(d, "id", ""), "probe")) continue;
		TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(d, "multi")));
		cJSON *t;
		cJSON_ArrayForEach(t, cJSON_GetObjectItem(d, "traits"))
			if (!strcmp(pf_json_str(t, "id", ""), "eta")) {
				probe_has_eta = true;
				TEST_ASSERT_EQUAL_STRING("duration", pf_json_str(t, "type", ""));
			}
		/* the companion probe is not offered as an instance */
		TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(cJSON_GetObjectItem(d, "instances")));
	}
	TEST_ASSERT_TRUE(probe_has_eta);
	TEST_ASSERT_TRUE(cJSON_GetArraySize(cJSON_GetObjectItem(cat, "tokens")) > 10);
	cJSON_Delete(cat);
	cJSON_Delete(st);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_class_rule_fires_per_probe);
	RUN_TEST(test_selectors);
	RUN_TEST(test_hold_time);
	RUN_TEST(test_condition_groups);
	RUN_TEST(test_eta_rule_and_tokens);
	RUN_TEST(test_only_while_cooking);
	RUN_TEST(test_builtin_rules_and_catalogue);
	return UNITY_END();
}
