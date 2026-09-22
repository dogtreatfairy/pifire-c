/* The conditional-notification engine: selection, conditions, hold times, edge firing and the
 * message templates. Events are captured through a sink instead of being sent anywhere. */
#include "core/events.h"
#include "core/settings.h"
#include "core/util.h"
#include "features/rules.h"
#include "features/alarms.h"
#include "features/push.h"
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
	TEST_ASSERT_TRUE(cJSON_GetArraySize(rules) >= 9);
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

/* the grill's distance from its set point, the tolerance operator, and a mode condition */
static void test_grill_stability_rules(void)
{
	/* stabilised: within 15 of the set point, and only while holding */
	only_rule("{\"id\":\"t\",\"enabled\":true,\"only_while_cooking\":true,\"for_s\":0,"
	          "\"select\":{\"domain\":\"grill\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":["
	            "{\"entity\":\"grill\",\"trait\":\"mode\",\"op\":\"is\",\"value\":\"Hold\"},"
	            "{\"entity\":\"grill\",\"trait\":\"over\",\"op\":\"within\",\"value\":0,\"value2\":15}]},"
	          "\"title\":\"up to temperature\",\"body\":\"{grill_temp} against {setpoint}\","
	          "\"level\":\"normal\",\"cooldown_s\":600}");
	cJSON *st = status();   /* pit 227, set point 225: two degrees over */
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_STRING("227\xC2\xB0""F against 225\xC2\xB0""F", g_cap[0].body);

	/* 30 over is outside the band, so it says nothing */
	only_rule("{\"id\":\"t2\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"grill\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"entity\":\"grill\",\"trait\":\"over\",\"op\":\"within\",\"value\":0,\"value2\":15}]},"
	          "\"title\":\"x\",\"body\":\"\",\"level\":\"normal\",\"cooldown_s\":600}");
	cJSON *probes = cJSON_GetObjectItem(st, "probes");
	cJSON_ReplaceItemInObject(cJSON_GetArrayItem(probes, 0), "temp", cJSON_CreateNumber(255));
	pf_rules_tick(st, 2000);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);

	/* running hot is the same reading past a threshold */
	only_rule("{\"id\":\"t3\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"grill\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"entity\":\"grill\",\"trait\":\"over\",\"op\":\">\",\"value\":20}]},"
	          "\"title\":\"running hot\",\"body\":\"\",\"level\":\"high\",\"cooldown_s\":600}");
	pf_rules_tick(st, 3000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_INT(PF_CRIT_HIGH, g_cap[0].crit);

	/* a mode condition keeps it quiet outside Hold */
	only_rule("{\"id\":\"t4\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"grill\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":["
	            "{\"entity\":\"grill\",\"trait\":\"mode\",\"op\":\"is\",\"value\":\"Smoke\"},"
	            "{\"entity\":\"grill\",\"trait\":\"over\",\"op\":\">\",\"value\":20}]},"
	          "\"title\":\"x\",\"body\":\"\",\"level\":\"high\",\"cooldown_s\":600}");
	pf_rules_tick(st, 4000);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);   /* the grill is in Hold, not Smoke */
	cJSON_Delete(st);
}

/* While a tuning measurement runs, the relay deliberately drives the pit either side of the set
 * point. A rule about the grill running hot would then be reporting the tuner's own doing, several
 * times per set point, so the distance from the target stops being testable for the duration. The
 * pit temperature itself is still a fact and still fires. */
static void test_deviation_rules_are_quiet_during_a_measurement(void)
{
	only_rule("{\"id\":\"hot\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"grill\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"entity\":\"grill\",\"trait\":\"over\",\"op\":\">\",\"value\":20}]},"
	          "\"title\":\"running hot\",\"body\":\"\",\"level\":\"high\",\"cooldown_s\":600}");
	cJSON *st = status();
	cJSON *probes = cJSON_GetObjectItem(st, "probes");
	cJSON_ReplaceItemInObject(cJSON_GetArrayItem(probes, 0), "temp", cJSON_CreateNumber(255));   /* 30 over */

	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ncap, "ordinarily 30 degrees over should be reported");

	cJSON *at = cJSON_GetObjectItem(st, "autotune");
	if (at) cJSON_ReplaceItemInObject(at, "active", cJSON_CreateBool(true));
	else { at = cJSON_AddObjectToObject(st, "autotune"); cJSON_AddBoolToObject(at, "active", true); }
	g_ncap = 0;
	pf_rules_tick(st, 5000);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_ncap, "the tuner's own swing must not raise a running-hot alert");

	/* the pit temperature is untouched: a rule written against it still works */
	only_rule("{\"id\":\"temp\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"grill\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"entity\":\"grill\",\"trait\":\"temp\",\"op\":\">\",\"value\":250}]},"
	          "\"title\":\"very hot\",\"body\":\"\",\"level\":\"high\",\"cooldown_s\":600}");
	g_ncap = 0;
	pf_rules_tick(st, 9000);
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ncap, "the pit temperature is still a fact during a measurement");
	cJSON_Delete(st);
}

/* the hopper thresholds, including the critical one that keeps reminding */
static void test_hopper_rules(void)
{
	only_rule("{\"id\":\"t\",\"enabled\":true,\"only_while_cooking\":true,\"for_s\":0,"
	          "\"select\":{\"domain\":\"hopper\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"level\",\"op\":\"<\",\"value\":20}]},"
	          "\"title\":\"Pellets are low\",\"body\":\"at {hopper}\",\"level\":\"normal\",\"cooldown_s\":600}");
	cJSON *st = status();   /* the fixture sits at 40 % */
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);
	cJSON_ReplaceItemInObject(st, "hopper_pct", cJSON_CreateNumber(12));
	pf_rules_tick(st, 1001);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_STRING("at 12%", g_cap[0].body);

	/* the critical one repeats while it stays true */
	only_rule("{\"id\":\"t2\",\"enabled\":true,\"only_while_cooking\":true,\"for_s\":0,"
	          "\"select\":{\"domain\":\"hopper\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"level\",\"op\":\"<\",\"value\":10}]},"
	          "\"title\":\"x\",\"body\":\"\",\"level\":\"critical\",\"cooldown_s\":60,\"repeat_s\":120}");
	cJSON_ReplaceItemInObject(st, "hopper_pct", cJSON_CreateNumber(5));
	pf_rules_tick(st, 2000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_INT(PF_CRIT_CRITICAL, g_cap[0].crit);
	pf_rules_tick(st, 2060);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);          /* inside the repeat interval */
	pf_rules_tick(st, 2130);
	TEST_ASSERT_EQUAL_INT(2, g_ncap);          /* and again once it has passed */
	cJSON_Delete(st);
}

/* A tuning run takes hours with nobody there, so being told it started, finished or gave up is the
 * whole point of having a phone. Those events used to be filed under "system", which is off by
 * default on every sink, so none of them ever left the grill. */
static void test_tuning_events_are_their_own_category(void)
{
	TEST_ASSERT_EQUAL_STRING("tuning", pf_push_category("Tune_Started"));
	TEST_ASSERT_EQUAL_STRING("tuning", pf_push_category("Tune_Done"));
	TEST_ASSERT_EQUAL_STRING("tuning", pf_push_category("Tune_Failed"));
	TEST_ASSERT_EQUAL_STRING("tuning", pf_push_category("Autotune_Done"));
	TEST_ASSERT_EQUAL_STRING("tuning", pf_push_category("Autotune_Failed"));
	/* and they are not lumped in with the notices that default to off */
	TEST_ASSERT_EQUAL_STRING("system", pf_push_category("UPDATE_AVAILABLE"));
	TEST_ASSERT_EQUAL_STRING("alarms", pf_push_category("E02_FLAMEOUT"));
	TEST_ASSERT_EQUAL_STRING("targets", pf_push_category("Probe_Temp_Achieved"));

	/* A rule names its own services and was written by the person holding the phone. Putting it
	 * through the category switches filed every conditional notification under "system", which is
	 * off by default, so the probe target, the hopper warnings and the Test button all went
	 * nowhere. A RULE_ code must never be filtered by category. */
	TEST_ASSERT_EQUAL_STRING("system", pf_push_category("RULE_probe-target"));
	char perr[160];
	pf_settings_patch("notify", "{\"ntfy\":{\"enabled\":true,\"system\":false}}", perr, sizeof perr);
	TEST_ASSERT_TRUE_MESSAGE(pf_push_wanted("ntfy", "RULE_probe-target"),
	                         "a rule must reach an enabled sink whatever the category switches say");
	TEST_ASSERT_FALSE_MESSAGE(pf_push_wanted("ntfy", "UPDATE_AVAILABLE"),
	                          "the daemon's own system chatter still respects the switch");
}


/* The shape Ryan asked for: the mode is one of two, AND the pit is within a band of the set point,
   AND it has held there. The "or" has to bind tighter than the "and", which is only expressible
   with a group inside a group. */
static void test_nested_groups_bind_or_tighter_than_and(void)
{
	only_rule("{\"id\":\"atsp\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"grill\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":["
	            "{\"op\":\"any\",\"conditions\":["
	              "{\"trait\":\"mode\",\"op\":\"is\",\"value\":\"Hold\"},"
	              "{\"trait\":\"mode\",\"op\":\"is\",\"value\":\"Smoke\"}]},"
	            "{\"trait\":\"temp\",\"op\":\"within\",\"value\":{\"trait\":\"setpoint\"},\"value2\":15}]},"
	          "\"title\":\"Grill is at set temp of {setpoint}\",\"body\":\"\","
	          "\"level\":\"normal\",\"sinks\":[\"app\"],\"for_s\":180,\"cooldown_s\":600}");
	cJSON *st = status();                       /* Hold, set point 225, pit 227: inside the band */

	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);           /* true, but it has not held for three minutes */
	pf_rules_tick(st, 1179);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);
	pf_rules_tick(st, 1180);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_STRING("Grill is at set temp of 225\xC2\xB0""F", g_cap[0].title);

	/* Smoke satisfies the same rule: the inner group is an or. The rule is edge triggered, so it
	   has to go false in between before it can say anything again. */
	cJSON_ReplaceItemInObject(st, "mode", cJSON_CreateString("Startup"));
	pf_rules_tick(st, 1500);
	cJSON_ReplaceItemInObject(st, "mode", cJSON_CreateString("Smoke"));
	g_ncap = 0;
	pf_rules_tick(st, 2000);
	pf_rules_tick(st, 2179);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);
	pf_rules_tick(st, 2180);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);

	/* Startup does not, even though the pit is still sitting on the set point */
	cJSON_ReplaceItemInObject(st, "mode", cJSON_CreateString("Startup"));
	g_ncap = 0;
	pf_rules_tick(st, 3000);
	pf_rules_tick(st, 3180);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);

	/* and neither does drifting out of the band while still in Hold */
	cJSON_ReplaceItemInObject(st, "mode", cJSON_CreateString("Hold"));
	cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItem(st, "probes"), 0);
	cJSON_ReplaceItemInObject(p, "temp", cJSON_CreateNumber(250));
	g_ncap = 0;
	pf_rules_tick(st, 4000);
	pf_rules_tick(st, 4180);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);
	cJSON_Delete(st);
}

/* the same thought written as one row with a list, which is how the editor offers it */
static void test_is_one_of(void)
{
	only_rule("{\"id\":\"m\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"grill\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":["
	            "{\"trait\":\"mode\",\"op\":\"is_one_of\",\"value\":[\"Hold\",\"Smoke\"]}]},"
	          "\"title\":\"{mode}\",\"body\":\"\",\"level\":\"info\",\"sinks\":[\"app\"],\"cooldown_s\":600}");
	cJSON *st = status();
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_STRING("Hold", g_cap[0].title);

	cJSON_ReplaceItemInObject(st, "mode", cJSON_CreateString("Shutdown"));
	g_ncap = 0;
	pf_rules_tick(st, 2000);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);

	/* is_none_of is its mirror */
	only_rule("{\"id\":\"m2\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"grill\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":["
	            "{\"trait\":\"mode\",\"op\":\"is_none_of\",\"value\":[\"Hold\",\"Smoke\"]}]},"
	          "\"title\":\"{mode}\",\"body\":\"\",\"level\":\"info\",\"sinks\":[\"app\"],\"cooldown_s\":600}");
	pf_rules_tick(st, 3000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	cJSON_Delete(st);
}

/* A rule still being written describes nothing and must not fire. */
static void test_an_empty_group_never_fires(void)
{
	only_rule("{\"id\":\"e\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"grill\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[]},"
	          "\"title\":\"nope\",\"body\":\"\",\"level\":\"info\",\"sinks\":[\"app\"],\"cooldown_s\":600}");
	cJSON *st = status();
	pf_rules_tick(st, 1000);
	pf_rules_tick(st, 2000);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);

	/* nor does an empty group nested inside a real one */
	only_rule("{\"id\":\"e2\",\"enabled\":true,\"only_while_cooking\":true,"
	          "\"select\":{\"domain\":\"grill\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":["
	            "{\"trait\":\"mode\",\"op\":\"is\",\"value\":\"Hold\"},"
	            "{\"op\":\"any\",\"conditions\":[]}]},"
	          "\"title\":\"nope\",\"body\":\"\",\"level\":\"info\",\"sinks\":[\"app\"],\"cooldown_s\":600}");
	pf_rules_tick(st, 3000);
	TEST_ASSERT_EQUAL_INT(0, g_ncap);
	cJSON_Delete(st);
}


/* ---- the alarm table: conditions that end by themselves ---- */

static cJSON *alarms(void) { return pf_alarms_json(); }
static int alarm_count(void) { cJSON *j = alarms(); int n = cJSON_GetArraySize(cJSON_GetObjectItem(j, "alarms")); cJSON_Delete(j); return n; }
static cJSON *alarm_at(cJSON *j, int i) { return cJSON_GetArrayItem(cJSON_GetObjectItem(j, "alarms"), i); }

/* The complaint that started this: a probe that has gone quiet is a condition, not an event. When
   it comes back, nobody should have to tell the grill that it is no longer missing. */
static void test_a_condition_ends_when_it_stops_being_true(void)
{
	only_rule("{\"id\":\"off\",\"enabled\":true,\"only_while_cooking\":true,\"for_s\":0,"
	          "\"select\":{\"domain\":\"probe\",\"role\":\"any\",\"link\":\"bluetooth\",\"match\":\"any\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"trait\":\"connected\",\"op\":\"is_off\"}]},"
	          "\"title\":\"{probe} went offline\",\"body\":\"\",\"level\":\"normal\",\"sinks\":[\"app\"],\"cooldown_s\":600}");
	cJSON *st = status();
	cJSON *bt1 = cJSON_GetArrayItem(cJSON_GetObjectItem(st, "probes"), 1);

	cJSON_ReplaceItemInObject(bt1, "valid", cJSON_CreateFalse());
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_INT(1, alarm_count());
	cJSON *j = alarms();
	TEST_ASSERT_TRUE_MESSAGE(cJSON_IsTrue(cJSON_GetObjectItem(alarm_at(j, 0), "active")), "it should be standing");
	TEST_ASSERT_EQUAL_STRING("BT1 went offline", cJSON_GetStringValue(cJSON_GetObjectItem(alarm_at(j, 0), "title")));
	cJSON_Delete(j);

	/* plugged back in: the condition is false, so the alarm is over and goes on its own */
	cJSON_ReplaceItemInObject(bt1, "valid", cJSON_CreateTrue());
	pf_rules_tick(st, 1010);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, alarm_count(), "a reconnected probe should not still be reported missing");

	/* and it can be reported again the next time it happens */
	g_ncap = 0;
	cJSON_ReplaceItemInObject(bt1, "valid", cJSON_CreateFalse());
	pf_rules_tick(st, 2000);
	TEST_ASSERT_EQUAL_INT(1, g_ncap);
	TEST_ASSERT_EQUAL_INT(1, alarm_count());
	cJSON_Delete(st);
}

/* Acknowledgement lives in the daemon, so clearing on one device clears on all of them. A
   condition still true stays on the list after being read; one already over leaves. */
static void test_acknowledgement_is_shared_and_respects_what_is_still_true(void)
{
	only_rule("{\"id\":\"hot\",\"enabled\":true,\"only_while_cooking\":true,\"for_s\":0,"
	          "\"select\":{\"domain\":\"grill\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"entity\":\"grill\",\"trait\":\"temp\",\"op\":\">\",\"value\":100}]},"
	          "\"title\":\"hot\",\"body\":\"\",\"level\":\"normal\",\"sinks\":[\"app\"],\"cooldown_s\":600}");
	cJSON *st = status();
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(1, alarm_count());

	cJSON *j = alarms();
	char key[96];
	pf_strlcpy(key, cJSON_GetStringValue(cJSON_GetObjectItem(alarm_at(j, 0), "key")), sizeof key);
	cJSON_Delete(j);

	TEST_ASSERT_EQUAL_INT(0, pf_alarms_ack(key));
	j = alarms();
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, cJSON_GetArraySize(cJSON_GetObjectItem(j, "alarms")),
	                              "reading it does not make it untrue");
	TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(alarm_at(j, 0), "acked")));
	TEST_ASSERT_EQUAL_INT(0, (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "unacked")));
	cJSON_Delete(j);

	/* now it is no longer true, and having been read there is nothing left to keep */
	cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItem(st, "probes"), 0);
	cJSON_ReplaceItemInObject(p, "temp", cJSON_CreateNumber(50));
	pf_rules_tick(st, 1010);
	TEST_ASSERT_EQUAL_INT(0, alarm_count());
	cJSON_Delete(st);
}

/* A rule that stops applying cannot go on claiming its condition is true. */
static void test_a_rule_that_stops_applying_retires_what_it_raised(void)
{
	only_rule("{\"id\":\"cook\",\"enabled\":true,\"only_while_cooking\":true,\"for_s\":0,"
	          "\"select\":{\"domain\":\"grill\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"entity\":\"grill\",\"trait\":\"temp\",\"op\":\">\",\"value\":100}]},"
	          "\"title\":\"hot\",\"body\":\"\",\"level\":\"normal\",\"sinks\":[\"app\"],\"cooldown_s\":600}");
	cJSON *st = status();
	pf_rules_tick(st, 1000);
	TEST_ASSERT_EQUAL_INT(1, alarm_count());

	/* the cook ends: a rule that only watches during a cook is no longer watching */
	cJSON_ReplaceItemInObject(st, "mode", cJSON_CreateString("Stop"));
	pf_rules_tick(st, 1010);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, alarm_count(), "a rule that is not being evaluated must not leave a standing claim");
	cJSON_Delete(st);
}

/* A condition on the boundary of its threshold can come and go every few seconds. A phone that
   buzzes twenty times in ten minutes teaches its owner to ignore it, so it gets silenced. */
static void test_a_chattering_condition_is_silenced(void)
{
	only_rule("{\"id\":\"chat\",\"enabled\":true,\"only_while_cooking\":true,\"for_s\":0,"
	          "\"select\":{\"domain\":\"grill\"},"
	          "\"when\":{\"op\":\"all\",\"conditions\":[{\"entity\":\"grill\",\"trait\":\"temp\",\"op\":\">\",\"value\":100}]},"
	          "\"title\":\"hot\",\"body\":\"\",\"level\":\"normal\",\"sinks\":[\"app\"],\"cooldown_s\":0}");
	cJSON *st = status();
	cJSON *p = cJSON_GetArrayItem(cJSON_GetObjectItem(st, "probes"), 0);
	double t = 1000;
	for (int i = 0; i < 8; i++) {
		cJSON_ReplaceItemInObject(p, "temp", cJSON_CreateNumber(227));
		pf_rules_tick(st, t++);
		cJSON_ReplaceItemInObject(p, "temp", cJSON_CreateNumber(50));
		pf_rules_tick(st, t++);
	}
	printf("chattering condition announced %d time(s) in %d swings\n", g_ncap, 8);
	TEST_ASSERT_TRUE_MESSAGE(g_ncap < 8, "it should stop announcing itself long before the eighth time");
	cJSON_Delete(st);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_class_rule_fires_per_probe);
	RUN_TEST(test_selectors);
	RUN_TEST(test_hold_time);
	RUN_TEST(test_condition_groups);
	RUN_TEST(test_nested_groups_bind_or_tighter_than_and);
	RUN_TEST(test_is_one_of);
	RUN_TEST(test_an_empty_group_never_fires);
	RUN_TEST(test_eta_rule_and_tokens);
	RUN_TEST(test_only_while_cooking);
	RUN_TEST(test_tuning_events_are_their_own_category);
	RUN_TEST(test_grill_stability_rules);
	RUN_TEST(test_deviation_rules_are_quiet_during_a_measurement);
	RUN_TEST(test_hopper_rules);
	RUN_TEST(test_builtin_rules_and_catalogue);
	RUN_TEST(test_a_condition_ends_when_it_stops_being_true);
	RUN_TEST(test_acknowledgement_is_shared_and_respects_what_is_still_true);
	RUN_TEST(test_a_rule_that_stops_applying_retires_what_it_raised);
	RUN_TEST(test_a_chattering_condition_is_silenced);
	return UNITY_END();
}
