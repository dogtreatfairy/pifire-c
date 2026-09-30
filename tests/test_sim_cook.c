/* End-to-end: drive the control state machine against the simulated grill with a fake clock.
 * Startup -> Hold 225F -> Shutdown, cold ambient, lid open, flame-out, over-temp, cold-start. */
#include "core/cmdq.h"
#include "core/control.h"
#include "core/db.h"
#include "core/env.h"
#include "core/history.h"
#include "core/log.h"
#include "core/outputs.h"
#include "core/settings.h"
#include "core/status.h"
#include "controllers/registry.h"
#include "platform/sim.h"
#include "probes/probes.h"
#include "probes/registry.h"
#include "features/learning.h"
#include "unity.h"
#include <sqlite3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "core/util.h"

static char cfg_path[256], db_path[256];
static pf_control ctrl;
static double now;

static void tick(double dt)
{
	/* 100 ms control ticks; sensor poll every tick (sim probe polls at 250 ms internally) */
	int steps = (int)(dt / 0.1 + 0.5);
	for (int i = 0; i < steps; i++) {
		now += 0.1;
		pf_sim_step(0.1);
		pf_probes_poll(now);
		pf_control_step(&ctrl, now);
	}
}

/* tick until the grill is in `m`, or `max_s` has passed; the seconds it took */
static double until_mode(pf_mode m, double max_s)
{
	double t = 0;
	while (ctrl.mode != m && t < max_s) { tick(1); t += 1; }
	return t;
}

/* Ryan's grill as fitted from his cook files: a time constant near 1470 s and a dead time near 90 s.
 * The default plant cools four or five times faster than any real barrel can with the fire out, so
 * fast that the lid recognition reads it as the lid; a flame-out is judged on the real one. */
static void real_plant(void) { pf_sim_set_plant(1471, 89); }

/* keep the fire out and the pot empty for `dt` seconds */
static void dead_pot(double dt)
{
	for (double t = 0; t < dt; t += 1) { pf_sim_model()->fire_lit = false; pf_sim_model()->pot_pellets_g = 0; tick(1); }
}

void setUp(void)
{
	snprintf(cfg_path, sizeof cfg_path, "/tmp/pf_simcook_%d.json", (int)getpid());
	snprintf(db_path, sizeof db_path, "/tmp/pf_simcook_%d.db", (int)getpid());
	unlink(cfg_path); unlink(db_path);
	TEST_ASSERT_EQUAL_INT(0, pf_settings_init(cfg_path));
	pf_settings_force_sim();
	pf_settings_patch("startup", "{\"start_to_mode\":{\"after_startup_mode\":\"Smoke\",\"primary_setpoint\":165}}", NULL, 0);
	TEST_ASSERT_EQUAL_INT(0, pf_db_open(db_path));
	pf_controllers_init(NULL);
	pf_probe_drivers_init(NULL);
	pf_env env; pf_env_init(&env, "platform");
	void *inst = pf_platform_sim()->create("{}", &env);
	pf_outputs_init(pf_platform_sim(), inst);
	pf_cmdq_init();
	pf_history_init();
	pf_probes_init();
	pf_control_init(&ctrl, true);
	now = 1000;
	pf_sim_reset(18.0);
	tick(3);
}

void tearDown(void)
{
	pf_sim_set_plant(240.0, 45.0);   /* the default plant for the next test */
	pf_control_shutdown(&ctrl);
	pf_probes_shutdown();
	pf_outputs_shutdown();
	pf_db_close();
	pf_settings_shutdown();
	unlink(cfg_path); unlink(db_path);
	char wal[300]; snprintf(wal, sizeof wal, "%s-wal", db_path); unlink(wal);
	snprintf(wal, sizeof wal, "%s-shm", db_path); unlink(wal);
}

static void test_full_cook(void)
{
	TEST_ASSERT_EQUAL(PF_MODE_STOP, ctrl.mode);
	pf_cmd_mode(PF_MODE_HOLD, 225); /* from Stop: startup first, then hold */
	tick(1);
	TEST_ASSERT_EQUAL(PF_MODE_STARTUP, ctrl.mode);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_FAN));
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_POWER));

	tick(245);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));
	TEST_ASSERT_TRUE(pf_sim_model()->fire_lit);

	/* settle: 40 min */
	tick(40 * 60);
	double sp = pf_f_to_c(225);
	printf("hold: pit %.1f C setpoint %.1f C u=%.2f\n", ctrl.pit_c, sp, ctrl.u_applied);
	TEST_ASSERT_DOUBLE_WITHIN(8.0, sp, ctrl.pit_c);
	TEST_ASSERT_TRUE(ctrl.target_reached);
	TEST_ASSERT_EQUAL_STRING("", ctrl.safety.error_code);

	/* step up to 275 F */
	pf_cmd c = { .type = PF_CMD_SETPOINT, .num = 275 };
	pf_cmdq_push(&c);
	tick(30 * 60);
	printf("hold2: pit %.1f C setpoint %.1f C u=%.2f\n", ctrl.pit_c, pf_f_to_c(275), ctrl.u_applied);
	TEST_ASSERT_DOUBLE_WITHIN(8.0, pf_f_to_c(275), ctrl.pit_c);

	pf_cmd_mode(PF_MODE_SHUTDOWN, 0);
	tick(1);
	TEST_ASSERT_EQUAL(PF_MODE_SHUTDOWN, ctrl.mode);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_AUGER));
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_FAN));
	/* the cool-down runs its four minutes, then until the pit is under the hot mark (at most 12) */
	tick(245);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.mode == PF_MODE_STOP || ctrl.pit_c > ctrl.cfg.restart_hot_c, "a cool pit stops on time");
	for (int i = 0; i < 480 && ctrl.mode != PF_MODE_STOP; i++) tick(1);
	TEST_ASSERT_EQUAL(PF_MODE_STOP, ctrl.mode);
	TEST_ASSERT_EQUAL_UINT(0, pf_outputs_mask());
}

static void test_lid_open_pauses_feed(void)
{
	pf_settings_patch("cycle_data", "{\"LidOpenDetectEnabled\":true,\"LidOpenThreshold\":15,\"LidOpenPauseTime\":60}", NULL, 0);
	pf_control_reload_settings(&ctrl);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 40 * 60);
	printf("lid test: mode %d pit %.1f C target_reached %d lid_open %d tuning [%s]\n", ctrl.mode, ctrl.pit_c, ctrl.target_reached, ctrl.lid_open, ctrl.dbg.note);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_TRUE(ctrl.target_reached);
	/* Open the lid and wait for the drop to be noticed. How long that takes depends on how fast
	 * the barrel sheds heat, so poll for it rather than assume a fixed delay. */
	pf_sim_model()->lid_open = true;
	int waited = 0;
	while (!ctrl.lid_open && waited < 300) { tick(5); waited += 5; }
	printf("lid detected after %d s at pit %.1f C\n", waited, ctrl.pit_c);
	TEST_ASSERT_TRUE(ctrl.lid_open);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_AUGER));
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_FAN));
	pf_sim_model()->lid_open = false;
	tick(120);
	TEST_ASSERT_FALSE(ctrl.lid_open);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_FAN));
}

/* Opening the lid, with the feed pause left off (the default). The pit falls much faster than the
 * grill can cool by itself; the controller recognises that as the lid, and the "is it getting
 * there" clock starts again, so the stall notifications wait for a recovery rather than calling a
 * cook checking the meat a grill that cannot hold. A real lid takes the pit down 20-40 C a minute;
 * the simulator's barrel is gentler, so the drop is applied directly. */
static void test_a_lid_drop_is_recognised_and_restarts_the_clock(void)
{
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 40 * 60);
	TEST_ASSERT_TRUE(ctrl.target_reached);
	TEST_ASSERT_FALSE(ctrl.lid_event);
	double aim_before = ctrl.aim_since;
	/* 30 seconds of a lid: 0.6 C a second off the pit */
	pf_sim_model()->lid_open = true;
	for (int k = 0; k < 30; k++) {
		pf_sim_model()->pit_c -= 0.6;
		for (int i = 0; i < 8; i++) pf_sim_model()->delay[i] = pf_sim_model()->pit_c;
		tick(1);
	}
	printf("lid event %d at pit %.1f C (set point %.1f)\n", ctrl.lid_event, ctrl.pit_c, ctrl.setpoint_c);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.lid_event, "a fall that fast is the lid");
	TEST_ASSERT_FALSE_MESSAGE(ctrl.lid_open, "the feed pause is a separate switch, and it is off");
	TEST_ASSERT_TRUE_MESSAGE(ctrl.aim_since > aim_before, "the clock the stall notifications read starts again");
	pf_status st; pf_status_get(&st);
	TEST_ASSERT_TRUE(st.lid_event);
	pf_sim_model()->lid_open = false;
	int t = 0;
	while (ctrl.lid_event && t < 1200) { tick(10); t += 10; }
	printf("lid event over after %d s at pit %.1f C\n", t, ctrl.pit_c);
	TEST_ASSERT_FALSE_MESSAGE(ctrl.lid_event, "and ends when the pit has recovered");
}

/* The grill's own slow drift is not a lid: a set point lowered by 20 C lets the pit fall as fast
 * as it can by itself, and that is nowhere near the lid's rate. */
static void test_a_grill_cooling_by_itself_is_not_a_lid(void)
{
	pf_cmd_mode(PF_MODE_HOLD, 275);
	tick(250 + 40 * 60);
	pf_cmd c = { .type = PF_CMD_SETPOINT, .num = 225 };
	pf_cmdq_push(&c);
	bool seen = false;
	for (int i = 0; i < 20 * 60; i++) { tick(1); if (ctrl.lid_event) seen = true; }
	TEST_ASSERT_FALSE(seen);
}

/* An overtemperature on a pit that then falls keeps its cool-down fan. */
static void test_overtemp_with_a_falling_pit_keeps_the_fan(void)
{
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 10 * 60);
	pf_sim_model()->pit_c = pf_f_to_c(660);
	for (int i = 0; i < 8; i++) pf_sim_model()->delay[i] = pf_sim_model()->pit_c;
	for (int i = 0; i < 30 && ctrl.mode != PF_MODE_ERROR; i++) tick(1);
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	pf_sim_model()->pit_c = pf_f_to_c(600);
	for (int i = 0; i < 8; i++) pf_sim_model()->delay[i] = pf_sim_model()->pit_c;
	tick(20);
	TEST_ASSERT_TRUE_MESSAGE(pf_outputs_get(PF_OUT_FAN), "falling pit: cool-down fan runs");
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_AUGER));
}

/* ---- the safety audit's findings, each proven ---- */

/* The fire goes out mid-Hold and stays out. Twenty degrees under the set point is a flame-out and
 * is relit; the relight that does not catch is a failed start, and the grill stops. */
static void test_a_dead_pot_is_not_fed_for_long(void)
{
	real_plant();
	pf_cmd_mode(PF_MODE_HOLD, 225);
	until_mode(PF_MODE_HOLD, 900);
	tick(30 * 60);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_TRUE(ctrl.target_reached);
	double sp = ctrl.setpoint_c;
	/* the fire goes out for good: the twenty-degree drop is a flame-out, and it is relit */
	int t = 0;
	while (ctrl.mode == PF_MODE_HOLD && t < 30 * 60) { dead_pot(5); t += 5; }
	double fell_f = pf_delta_from_c(sp - ctrl.pit_c, PF_UNITS_F);
	printf("dead pot: Relight after %d s, %.1f F below the set point\n", t, fell_f);
	TEST_ASSERT_EQUAL_MESSAGE(PF_MODE_REIGNITE, ctrl.mode, "a twenty-degree drop is relit");
	TEST_ASSERT_TRUE(fell_f >= 19 && fell_f < 23);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	/* the relight does not catch: no rise in its time is a failed start, and the grill stops */
	double fed0 = ctrl.auger_total_on_s;
	int r = 0;
	while (ctrl.mode == PF_MODE_REIGNITE && r < 15 * 60) { dead_pot(5); r += 5; }
	double on = ctrl.cfg.ss_prof[ctrl.ss_profile].augerontime, off = ctrl.cfg.smoke_off_s + ctrl.cfg.ss_prof[ctrl.ss_profile].p_mode * 10;
	double duty = (ctrl.auger_total_on_s - fed0) / (double)r;
	double hold = pf_learning_uff(sp, ctrl.ambient_c, ctrl.cfg.u_min, ctrl.cfg.u_max, NULL);   /* the most it can be: fed for below the set point */
	printf("dead pot: %s after %d s of Relight, auger duty %.2f against the startup feed's %.2f and the holding rate's %.2f\n",
	       ctrl.safety.error_code, r, duty, on / (on + off), hold);
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E04_STARTUP_FAILED", ctrl.safety.error_code);
	TEST_ASSERT_TRUE_MESSAGE(r <= ctrl.cfg.ss_prove_s + 5, "within the Smart Start time");
	TEST_ASSERT_TRUE_MESSAGE(duty <= fmax(on / (on + off), hold) + 0.05, "fed no faster than the holding rate");
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_AUGER));
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));
}

/* A fire that dies before the pit has reached its set point is caught by the same rule, measured
 * from the highest the pit had climbed, rather than at the floor with the controller at full feed. */
static void test_a_fire_lost_on_the_way_up_is_caught_at_the_drop(void)
{
	pf_cmd_mode(PF_MODE_HOLD, 450);
	until_mode(PF_MODE_HOLD, 900);
	tick(4 * 60);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_FALSE_MESSAGE(ctrl.target_reached, "still climbing");
	double peak = ctrl.safety.peak_c;
	int t = 0;
	while (ctrl.mode == PF_MODE_HOLD && t < 30 * 60) { dead_pot(5); t += 5; if (ctrl.safety.peak_c > peak) peak = ctrl.safety.peak_c; }
	double fell_f = pf_delta_from_c(peak - ctrl.pit_c, PF_UNITS_F);
	printf("on the way up: Relight after %d s, %.1f F below a peak of %.0f F\n", t, fell_f, pf_from_c(peak, PF_UNITS_F));
	TEST_ASSERT_EQUAL_MESSAGE(PF_MODE_REIGNITE, ctrl.mode, "a climbing pit falling back from its peak is a flame-out");
	TEST_ASSERT_TRUE(fell_f >= 19 && fell_f < 25);
}

/* Hold from Manual goes through Startup: straight to Hold fed a cold pot with no igniter. */
static void test_hold_from_manual_lights_first(void)
{
	pf_cmd_mode(PF_MODE_MANUAL, 0);
	tick(2);
	TEST_ASSERT_EQUAL(PF_MODE_MANUAL, ctrl.mode);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(2);
	TEST_ASSERT_EQUAL(PF_MODE_STARTUP, ctrl.mode);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
}

/* A kilogram prime is one auger run at most, not an hour-long wait before lighting. */
static void test_a_huge_prime_is_clamped(void)
{
	pf_cmd c = { .type = PF_CMD_PRIME, .num = 1000 };
	strcpy(c.str, "Startup");
	pf_cmdq_push(&c);
	tick(2);
	TEST_ASSERT_EQUAL(PF_MODE_PRIME, ctrl.mode);
	TEST_ASSERT_TRUE(ctrl.prime_duration_s <= ctrl.cfg.auger_max_on_s);
	tick(ctrl.prime_duration_s + 3);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.mode != PF_MODE_PRIME, "prime over when the feed is");
}

/* Reignite and Prime are not requests anyone may make of a burning grill. */
static void test_reignite_and_prime_cannot_be_requested_mid_cook(void)
{
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 5 * 60);
	pf_cmd_mode(PF_MODE_REIGNITE, 0); tick(2);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	pf_cmd_mode(PF_MODE_PRIME, 0); tick(2);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
}

/* A set point above the overtemperature limit is held below it. */
static void test_a_set_point_cannot_reach_the_limit(void)
{
	pf_cmd_mode(PF_MODE_HOLD, 900);
	tick(3);
	TEST_ASSERT_DOUBLE_WITHIN(0.1, pf_f_to_c(550), ctrl.setpoint_c);
	pf_cmd c = { .type = PF_CMD_SETPOINT, .num = 120 };
	pf_cmdq_push(&c);
	tick(1);
	TEST_ASSERT_DOUBLE_WITHIN(0.1, pf_f_to_c(160), ctrl.setpoint_c);
}

/* Recovery is once per cook, and never for a light that was not confirmed. */
static void test_recovery_is_refused_when_it_is_not_safe(void)
{
	pf_settings_patch("safety", "{\"power_loss\":{\"recovery\":true,\"max_s\":300}}", NULL, 0);
	pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	tick(2);
	char once[512];
	snprintf(once, sizeof once, "{\"mode\":\"Hold\",\"saved_wall\":%.0f,\"setpoint_c\":107,\"recoveries\":1,\"boot_id\":\"x\"}", pf_wall() - 30);
	TEST_ASSERT_TRUE(pf_control_recover(&ctrl, once, now));
	TEST_ASSERT_EQUAL_MESSAGE(PF_MODE_ERROR, ctrl.mode, "a second recovery in one cook");
	TEST_ASSERT_EQUAL_STRING("E08_POWER_LOSS", ctrl.safety.error_code);

	pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	tick(2);
	char unlit[512];
	snprintf(unlit, sizeof unlit, "{\"mode\":\"Startup\",\"saved_wall\":%.0f,\"coldstart_active\":true,\"coldstart_reached\":false}", pf_wall() - 30);
	TEST_ASSERT_TRUE(pf_control_recover(&ctrl, unlit, now));
	TEST_ASSERT_EQUAL_MESSAGE(PF_MODE_ERROR, ctrl.mode, "a light never confirmed is not lit again");
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));
}

/* Stop, however it arrives, ends a recipe: it used to read the Stop as its step ending and relight. */
static void test_a_stop_request_ends_a_recipe(void)
{
	ctrl.recipe.active = true;
	pf_cmd_mode(PF_MODE_STOP, 0);
	tick(2);
	TEST_ASSERT_FALSE(ctrl.recipe.active);
}

/* Smoke has no relight assist: it feeds at the startup rate already, and the floor stops a dead pot. */
static void test_a_dead_pot_in_smoke_is_caught(void)
{
	real_plant();
	pf_cmd_mode(PF_MODE_SMOKE, 0);
	until_mode(PF_MODE_SMOKE, 900);
	tick(20 * 60);
	TEST_ASSERT_EQUAL(PF_MODE_SMOKE, ctrl.mode);
	int t = 0;
	while (ctrl.mode == PF_MODE_SMOKE && t < 90 * 60) { dead_pot(5); t += 5; }
	printf("dead pot in smoke: %s after %d s at pit %.0f F\n", pf_mode_name(ctrl.mode), t, pf_from_c(ctrl.pit_c, PF_UNITS_F));
	TEST_ASSERT_EQUAL(PF_MODE_REIGNITE, ctrl.mode);
	int r = 0;
	while (ctrl.mode == PF_MODE_REIGNITE && r < 15 * 60) { dead_pot(5); r += 5; }
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E04_STARTUP_FAILED", ctrl.safety.error_code);
}

static void test_overtemp_errors(void)
{
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 10 * 60);
	pf_sim_model()->pit_c = pf_f_to_c(660);
	for (int i = 0; i < 8; i++) pf_sim_model()->delay[i] = pf_sim_model()->pit_c;
	tick(15);
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E01_OVERTEMP", ctrl.safety.error_code);
	TEST_ASSERT_EQUAL_STRING("Overheat: pit 660°F, limit 650°F", ctrl.safety.error_msg);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_AUGER));
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));
	TEST_ASSERT_TRUE_MESSAGE(pf_outputs_get(PF_OUT_FAN), "the cool-down fan runs");
	/* the pit goes on climbing after the error: a fan now would be feeding a fire, so it stops */
	pf_sim_model()->pit_c = pf_f_to_c(690);
	for (int i = 0; i < 8; i++) pf_sim_model()->delay[i] = pf_sim_model()->pit_c;
	tick(15);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_FAN));
	/* only Stop is accepted */
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(1);
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	pf_cmd_simple(PF_CMD_STOP);
	tick(1);
	TEST_ASSERT_EQUAL(PF_MODE_STOP, ctrl.mode);
}

/* Stop is off with the power still on: nothing is cooking, nothing is being controlled, and the
   app shows the pit as zero. Logging a row every few seconds through the days between cooks is SD
   card wear spent recording room temperature, and it fills the history with a flat line that means
   nothing. Monitor is the mode for watching without running, and it still logs. */
static int history_rows(void)
{
	pf_history_flush();
	sqlite3_stmt *st;
	int n = 0;
	if (sqlite3_prepare_v2(pf_db_handle(), "SELECT COUNT(*) FROM history_probe", -1, &st, NULL) == SQLITE_OK) {
		if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
		sqlite3_finalize(st);
	}
	return n;
}

static void test_stop_records_nothing(void)
{
	pf_cmd_simple(PF_CMD_STOP);
	tick(30);
	TEST_ASSERT_EQUAL(PF_MODE_STOP, ctrl.mode);

	int before = history_rows();
	tick(10 * 60);
	int after = history_rows();
	printf("stopped: %d history rows before, %d after ten minutes\n", before, after);
	TEST_ASSERT_EQUAL_INT_MESSAGE(before, after, "a stopped grill should not be writing rows");

	/* Monitor is the watch-without-running mode, and it does log */
	pf_cmd_mode(PF_MODE_MONITOR, 0);
	tick(5 * 60);
	int monitored = history_rows();
	printf("monitoring: %d rows\n", monitored);
	TEST_ASSERT_TRUE_MESSAGE(monitored > after, "Monitor is for watching, so it should record");
	pf_cmd_simple(PF_CMD_STOP);
	tick(10);
}

/* A flame-out in Hold -- the pit twenty degrees under its set point -- is relit through the Smart
   Start sequence, once per cook by default; the next one stops the grill. */
static void test_flameout_protection_lights_the_igniter_and_recovers(void)
{
	real_plant();
	pf_cmd_mode(PF_MODE_HOLD, 225);
	until_mode(PF_MODE_HOLD, 900);
	tick(20 * 60);
	TEST_ASSERT_TRUE(ctrl.target_reached);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));
	/* the fire goes out with the grill on its target; the pot empties */
	pf_sim_model()->fire_lit = false;
	pf_sim_model()->pot_pellets_g = 0;
	double t = until_mode(PF_MODE_REIGNITE, 30 * 60);
	printf("relight after %.0f s at pit %.1f F\n", t, pf_from_c(ctrl.pit_c, PF_UNITS_F));
	TEST_ASSERT_EQUAL(PF_MODE_REIGNITE, ctrl.mode);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	TEST_ASSERT_TRUE(ctrl.safety.ss_active);
	/* the Relight runs Smart Start: the pellets it feeds catch, the rise proves it, and the grill
	 * goes back to Hold at the same set point, heating */
	t = until_mode(PF_MODE_HOLD, 15 * 60);
	printf("back in Hold after %.0f s of Relight\n", t);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_DOUBLE_WITHIN(0.1, pf_f_to_c(225), ctrl.setpoint_c);
	TEST_ASSERT_TRUE(ctrl.safety.heating);
	TEST_ASSERT_EQUAL_INT(0, ctrl.safety.reignite_retries_left);
	/* a second flame-out in the cook has no relight left: the grill stops */
	tick(20 * 60);
	pf_sim_model()->fire_lit = false;
	pf_sim_model()->pot_pellets_g = 0;
	int k = 0;
	while (ctrl.mode == PF_MODE_HOLD && k < 40 * 60) { dead_pot(5); k += 5; }
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E02_FLAMEOUT", ctrl.safety.error_code);
}

/* Lowering the set point makes the grill starve the fire and coast, and a starved fire and a dead
   one cool alike, so the fire is proven where it can be: when the pit reaches the new set point the
   igniter runs for the proving time while the controller brings the feed back. */
static void test_a_big_step_down_lights_the_igniter_at_the_crossing(void)
{
	pf_cmd_mode(PF_MODE_HOLD, 300);
	until_mode(PF_MODE_HOLD, 900);
	tick(25 * 60);
	TEST_ASSERT_TRUE(ctrl.target_reached);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));
	{ pf_cmd sp = { .type = PF_CMD_SETPOINT, .num = 225 }; pf_cmdq_push(&sp); }
	tick(5);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.safety.stepdown_armed, "a lower set point with the pit above it arms the proof");
	TEST_ASSERT_FALSE_MESSAGE(pf_outputs_get(PF_OUT_IGNITER), "nothing while the pit coasts down");
	double t = 0;
	while (!ctrl.safety.proving && t < 90 * 60) { tick(1); t += 1; }
	double below_f = pf_from_c(ctrl.setpoint_c, PF_UNITS_F) - pf_from_c(ctrl.pit_c, PF_UNITS_F);
	printf("step-down: igniter on after %.0f s, %.1f F below the new set point\n", t, below_f);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.safety.proving, "reaching the lowered set point starts the proof");
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	TEST_ASSERT_TRUE_MESSAGE(below_f < 2, "at the set point, not after an undershoot");
	/* the igniter runs its proving time and goes off; the grill holds the new set point */
	tick(ctrl.cfg.relight_prove_s - 5);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	tick(10);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));
	TEST_ASSERT_FALSE(ctrl.safety.proving);
	tick(30 * 60);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_DOUBLE_WITHIN(8.0, pf_f_to_c(225), ctrl.pit_c);
}

/* With relighting switched off, a flame-out stops the grill instead. */
static void test_flameout_reignites_then_errors(void)
{
	real_plant();
	pf_settings_patch("safety", "{\"relight_enabled\":false}", NULL, 0);
	pf_control_reload_settings(&ctrl);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	until_mode(PF_MODE_HOLD, 900);
	tick(20 * 60);
	int t = 0;
	while (ctrl.mode == PF_MODE_HOLD && t < 40 * 60) { dead_pot(5); t += 5; }
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E02_FLAMEOUT", ctrl.safety.error_code);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_AUGER));
}

static void test_coldstart_winter(void)
{
	pf_sim_reset(-5.0); /* 23 F */
	tick(15);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(1);
	TEST_ASSERT_EQUAL(PF_MODE_STARTUP, ctrl.mode);
	TEST_ASSERT_TRUE(ctrl.safety.ss_active);
	double t = 0;
	while (!ctrl.safety.ss_proven && t < 400) { tick(1); t += 1; }
	double low = ctrl.safety.ss_baseline_c;
	printf("winter: proven after %.0f s from a low of %.1f F\n", t, pf_from_c(low, PF_UNITS_F));
	TEST_ASSERT_TRUE(ctrl.safety.ss_proven);
	TEST_ASSERT_TRUE(t <= ctrl.cfg.ss_prove_s);
	TEST_ASSERT_DOUBLE_WITHIN(1.5, -5.0, low);
	/* startup ends at the exit rise, not on a timer */
	t += until_mode(PF_MODE_HOLD, 400);
	double rise_f = pf_delta_from_c(ctrl.safety.filt_c - low, PF_UNITS_F);
	printf("winter: Hold after %.0f s, %.1f F over the low\n", t, rise_f);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_TRUE(rise_f >= 12);
	TEST_ASSERT_TRUE(ctrl.safety.heating);
	/* Heating, then at temperature */
	{
		tick(2);
		pf_status st; pf_status_get(&st);
		cJSON *j = pf_status_to_json(&st, PF_UNITS_F);
		TEST_ASSERT_TRUE(pf_json_bool(j, "heating.active", false));
		TEST_ASSERT_EQUAL_STRING("Heating to 225°F", pf_json_str(j, "heating.text", ""));
		cJSON_Delete(j);
	}
	tick(40 * 60);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_FALSE(ctrl.safety.heating);
	TEST_ASSERT_EQUAL_STRING("", ctrl.safety.error_code);
	TEST_ASSERT_DOUBLE_WITHIN(10.0, pf_f_to_c(225), ctrl.pit_c);
	TEST_ASSERT_DOUBLE_WITHIN(3.0, -5.0, ctrl.ambient_c);
}

static void test_coldstart_failure(void)
{
	/* the igniter's heat does not reach the probe, as on Ryan's grill: no rise at all */
	pf_sim_reset(-34.0);   /* -30 F */
	pf_sim_model()->igniter_heat_c = 0;
	tick(3);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	dead_pot(200);
	TEST_ASSERT_EQUAL(PF_MODE_STARTUP, ctrl.mode);
	/* a rise too small proves nothing */
	pf_sim_model()->pit_c += pf_delta_to_c(2, PF_UNITS_F);
	for (int i = 0; i < 8; i++) pf_sim_model()->delay[i] = pf_sim_model()->pit_c;
	dead_pot(40);
	TEST_ASSERT_FALSE(ctrl.safety.ss_proven);
	dead_pot(70);
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E04_STARTUP_FAILED", ctrl.safety.error_code);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_AUGER));
}

/* The worst case for the small rise: the igniter alone lifts the pit past it with nothing alight.
 * The exit rise is what the igniter cannot fake, so a fire that never came is still a failed start,
 * inside two Smart Start windows, and never handed to Hold. */
static void test_the_igniter_alone_cannot_pass_smart_start(void)
{
	pf_sim_reset(10.0);
	pf_sim_model()->igniter_heat_c = 6.0;
	tick(3);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(1);
	int t = 0;
	while (ctrl.mode == PF_MODE_STARTUP && t < 15 * 60) { dead_pot(5); t += 5; }
	printf("igniter only: %s after %d s (small rise %s)\n", pf_mode_name(ctrl.mode), t, ctrl.safety.ss_proven ? "seen" : "not seen");
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E04_STARTUP_FAILED", ctrl.safety.error_code);
	TEST_ASSERT_TRUE(t <= 2 * ctrl.cfg.ss_prove_s + 10);
}

/* Smart Start with what ships: on, five minutes. A fire that never lights is still lighting at
 * four and a half minutes and in error by just after five -- and it is not lit a second time. */
static void test_smart_start_defaults_error_after_five_minutes(void)
{
	pf_sim_reset(10.0);
	pf_sim_model()->igniter_heat_c = 0;
	tick(3);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	int t = 0;
	for (; t < 270; t += 5) { tick(5); pf_sim_model()->pot_pellets_g = 0; pf_sim_model()->fire_lit = false; }
	TEST_ASSERT_TRUE_MESSAGE(ctrl.mode == PF_MODE_STARTUP, "still lighting at 4.5 min");
	for (; t < 330; t += 5) { tick(5); pf_sim_model()->pot_pellets_g = 0; pf_sim_model()->fire_lit = false; }
	TEST_ASSERT_EQUAL_MESSAGE(PF_MODE_ERROR, ctrl.mode, "no rise by 5 min: error");
	TEST_ASSERT_EQUAL_STRING("E04_STARTUP_FAILED", ctrl.safety.error_code);
	TEST_ASSERT_FALSE_MESSAGE(pf_outputs_get(PF_OUT_IGNITER), "and nothing is lit again");
}

/* Startup ended by hand on a pot that never lit: the pit never climbs, so nothing falls either. The
 * heating watch is what catches it -- no gain in the Smart Start time below the minimum target is a
 * failed start, not a relight of a pot full of pellets. */
static void test_a_stalled_heat_up_is_a_failed_start(void)
{
	pf_sim_reset(10.0);
	tick(3);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	dead_pot(30);
	pf_cmd skip = { .type = PF_CMD_MODE, .mode = PF_MODE_HOLD, .num = 225, .flag = true };
	pf_cmdq_push(&skip);
	dead_pot(2);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_TRUE(ctrl.safety.heating);
	int t = 0;
	while (ctrl.mode == PF_MODE_HOLD && t < 20 * 60) { dead_pot(5); t += 5; }
	printf("stalled heat-up: %s after %d s\n", ctrl.safety.error_code, t);
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E04_STARTUP_FAILED", ctrl.safety.error_code);
	TEST_ASSERT_TRUE(t <= ctrl.cfg.ss_prove_s + 10);
}

/* -30 F outside, on Ryan's grill's own plant. Every Smart Start threshold is a rise over the grill's
 * lowest reading, so the cold moves where it starts and nothing else: proven, handed over, heating,
 * and never a failed start while the fire burns -- even where Smoke settles far under the minimum. */
static void test_a_start_at_minus_thirty(void)
{
	real_plant();
	pf_sim_reset(-34.4);   /* -30 F */
	pf_sim_model()->igniter_heat_c = 0;
	tick(15);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(1);
	double t = 0;
	while (!ctrl.safety.ss_proven && t < 400) { tick(1); t += 1; }
	printf("-30 F: proven after %.0f s from %.1f F\n", t, pf_from_c(ctrl.safety.ss_baseline_c, PF_UNITS_F));
	TEST_ASSERT_TRUE(ctrl.safety.ss_proven);
	t += until_mode(PF_MODE_HOLD, 400);
	printf("-30 F: Hold after %.0f s at %.1f F\n", t, pf_from_c(ctrl.pit_c, PF_UNITS_F));
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	tick(60 * 60);
	printf("-30 F: an hour on, %s at %.0f F, error '%s'\n", pf_mode_name(ctrl.mode), pf_from_c(ctrl.pit_c, PF_UNITS_F), ctrl.safety.error_code);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("", ctrl.safety.error_code);
	TEST_ASSERT_TRUE(ctrl.safety.established);
	/* Smoke at -30 F on the P-mode that suits a mild day cannot hold 180 F: that is a feed too low,
	 * and the grill says so. Ryan's answer is the P-mode, turned down until Smoke sits above 180 F. */
	pf_settings_patch("cycle_data", "{\"PMode\":0,\"SmokeOnCycleTime\":25}", NULL, 0);
	pf_control_reload_settings(&ctrl);
	pf_cmd_mode(PF_MODE_SMOKE, 0);
	tick(60 * 60);
	printf("-30 F: Smoke on P0 an hour on at %.0f F, %s %s\n", pf_from_c(ctrl.pit_c, PF_UNITS_F), pf_mode_name(ctrl.mode), ctrl.safety.error_msg);
	TEST_ASSERT_EQUAL(PF_MODE_SMOKE, ctrl.mode);
	TEST_ASSERT_TRUE(pf_from_c(ctrl.pit_c, PF_UNITS_F) >= 175);
}

/* Started again straight after a cook, with the barrel at 300 F: the pellets catch from the heat
 * and hold the pit, which cannot show a rise on cue. The stopped fall proves it. */
static void test_a_hot_restart_is_proven_by_the_stopped_fall(void)
{
	real_plant();
	pf_cmd_mode(PF_MODE_HOLD, 300);
	until_mode(PF_MODE_HOLD, 900);
	tick(60 * 60);
	pf_cmd_simple(PF_CMD_STOP);
	tick(20);
	double from = ctrl.pit_c;
	pf_cmd_mode(PF_MODE_HOLD, 300);
	tick(1);
	TEST_ASSERT_EQUAL(PF_MODE_STARTUP, ctrl.mode);
	double t = until_mode(PF_MODE_HOLD, 700);
	printf("hot restart from %.0f F: Hold after %.0f s (%s)\n", pf_from_c(from, PF_UNITS_F), t, ctrl.safety.ss_held ? "held" : "rose");
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
}

/* The same hot barrel with a dead pot and the igniter's heat reaching the probe: it keeps falling,
 * so neither proof passes, and it is a failed start. */
static void test_a_hot_dead_pot_is_not_proven(void)
{
	real_plant();
	pf_cmd_mode(PF_MODE_HOLD, 300);
	until_mode(PF_MODE_HOLD, 900);
	tick(60 * 60);
	pf_cmd_simple(PF_CMD_STOP);
	tick(20);
	pf_sim_model()->igniter_heat_c = 6.0;
	pf_cmd_mode(PF_MODE_HOLD, 300);
	tick(1);
	int t = 0;
	while (ctrl.mode == PF_MODE_STARTUP && t < 15 * 60) { dead_pot(5); t += 5; }
	printf("hot dead pot: %s after %d s\n", pf_mode_name(ctrl.mode), t);
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E04_STARTUP_FAILED", ctrl.safety.error_code);
}

/* Smoke whose P-mode cannot hold 180 F: the pit comes down past it, and that is a flame-out or a
 * feed too low. It is relit once, and when the relit fire still cannot climb the grill stops and
 * says to lower the P-mode. */
static void test_smoke_that_cannot_hold_180_is_stopped(void)
{
	real_plant();
	pf_sim_reset(-34.4);
	pf_settings_patch("cycle_data", "{\"PMode\":9}", NULL, 0);
	pf_control_reload_settings(&ctrl);
	pf_cmd_mode(PF_MODE_HOLD, 250);
	until_mode(PF_MODE_HOLD, 900);
	tick(60 * 60);
	pf_cmd_mode(PF_MODE_SMOKE, 0);
	int t = 0;
	while (ctrl.mode != PF_MODE_ERROR && t < 4 * 3600) { tick(10); t += 10; }
	printf("smoke on P9 at -30 F: %s after %d s: %s\n", pf_mode_name(ctrl.mode), t, ctrl.safety.error_msg);
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_NOT_NULL(strstr(ctrl.safety.error_msg, "P-mode"));
}

static void test_manual_refused_and_override_expires(void)
{
	pf_cmd m = { .type = PF_CMD_MANUAL_OUTPUT, .flag = true };
	strcpy(m.str, "auger");
	pf_cmdq_push(&m);
	tick(1);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_AUGER)); /* not in Manual mode, allow_manual false */
	pf_cmd_mode(PF_MODE_MANUAL, 0);
	tick(1);
	pf_cmdq_push(&m);
	tick(1);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_AUGER));
	tick(70); /* absolute auger cap (60 s) applies even in Manual */
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_AUGER));
}

/* software update mid-cook: the outgoing process snapshots the cook, the new one resumes it */
static void test_warm_restart_resumes_hold(void)
{
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(1);
	TEST_ASSERT_EQUAL(PF_MODE_STARTUP, ctrl.mode);
	/* restart during Startup: the countdown continues from where it was */
	tick(100);
	char *snap = pf_control_resume_json(&ctrl, now);
	TEST_ASSERT_NOT_NULL(snap);
	double cook_start = ctrl.cook_start_wall;
	pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	tick(2);
	TEST_ASSERT_EQUAL(PF_MODE_STOP, ctrl.mode);
	TEST_ASSERT_TRUE(pf_control_resume(&ctrl, snap, now));
	free(snap);
	TEST_ASSERT_EQUAL(PF_MODE_STARTUP, ctrl.mode);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.next_mode);
	TEST_ASSERT_DOUBLE_WITHIN(5, 100, now - ctrl.mode_start);
	TEST_ASSERT_EQUAL_DOUBLE(cook_start, ctrl.cook_start_wall);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	tick(150);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);

	/* restart during Hold with a probe target and a timer set */
	tick(10 * 60);
	pf_notify_set_target(&ctrl.notify, "Probe1", pf_f_to_c(203), PF_AFTER_NONE);
	pf_notify_timer_start(&ctrl.notify, 1800, PF_AFTER_NONE, now);
	tick(1);
	snap = pf_control_resume_json(&ctrl, now);
	TEST_ASSERT_NOT_NULL(snap);
	double u_before = ctrl.u_applied;
	pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	tick(2);
	TEST_ASSERT_TRUE(pf_control_resume(&ctrl, snap, now));
	free(snap);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, pf_f_to_c(225), ctrl.setpoint_c);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, u_before, ctrl.u_applied);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_FAN));
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_POWER));
	const pf_notify_probe *np = pf_notify_find(&ctrl.notify, "Probe1");
	TEST_ASSERT_NOT_NULL(np);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, pf_f_to_c(203), np->target_c);
	TEST_ASSERT_TRUE(ctrl.notify.timer.running);
	TEST_ASSERT_DOUBLE_WITHIN(5, 1800, ctrl.notify.timer.end_t - now);
	tick(5 * 60);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("", ctrl.safety.error_code);

	/* a stale snapshot (or one taken while stopped) is refused */
	pf_cmd_mode(PF_MODE_STOP, 0);
	tick(1);
	TEST_ASSERT_NULL(pf_control_resume_json(&ctrl, now));
	TEST_ASSERT_FALSE(pf_control_resume(&ctrl, "{\"mode\":\"Hold\",\"saved_wall\":1}", now));
	TEST_ASSERT_EQUAL(PF_MODE_STOP, ctrl.mode);
}

/* Power loss. The cook writes a checkpoint every ten seconds; a daemon starting unclean within
 * safety.power_loss.max_s of it relights for safety.power_loss.igniter_s and goes back to the mode
 * it was in; beyond that it goes to Error and stays there, because a pot that has been out for
 * that long has to be looked at before it is lit again. */
static void test_power_loss_recovery(void)
{
	/* setUp has already built the controller; it reads the safety settings on init, so re-init
	 * after patching them -- shutting the first one down, or the sanitiser calls it a leak */
	pf_settings_patch("safety", "{\"power_loss\":{\"recovery\":true,\"max_s\":300}}", NULL, 0);
	pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	pf_control_set_checkpoint_path(&ctrl, "/tmp/pf_test_checkpoint.json");
	unlink("/tmp/pf_test_checkpoint.json");
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(400);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_TRUE_MESSAGE(pf_file_exists("/tmp/pf_test_checkpoint.json"), "a cook leaves a checkpoint behind");
	char *snap = pf_read_file("/tmp/pf_test_checkpoint.json", NULL);
	TEST_ASSERT_NOT_NULL(snap);

	/* the power comes back within the limit: relight, then Hold at the same set point */
	cJSON *o = cJSON_Parse(snap);
	cJSON_ReplaceItemInObject(o, "saved_wall", cJSON_CreateNumber(pf_wall() - 90));
	char *soon = cJSON_PrintUnformatted(o);
	pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	tick(2);
	/* this one the grill chose itself, to recover its Wi-Fi: it says so, and says the cook resumes */
	pf_control_note_restart(&ctrl, "Wi-Fi radio not responding");
	TEST_ASSERT_TRUE(pf_control_recover(&ctrl, soon, now));
	TEST_ASSERT_EQUAL(PF_MODE_REIGNITE, ctrl.mode);
	{
		tick(1);
		pf_status st; pf_status_get(&st);
		cJSON *j = pf_status_to_json(&st, PF_UNITS_F);
		TEST_ASSERT_TRUE(pf_json_bool(j, "restarted.active", false));
		TEST_ASSERT_EQUAL_STRING("Wi-Fi radio not responding", pf_json_str(j, "restarted.reason", ""));
		TEST_ASSERT_EQUAL_STRING(" · Cook resumed", pf_json_str(j, "restarted.resuming", ""));
		cJSON_Delete(j);
	}
	TEST_ASSERT_TRUE_MESSAGE(ctrl.safety.ss_active, "the relight runs Smart Start");
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.safety.reignite_last);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	until_mode(PF_MODE_HOLD, 600);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_DOUBLE_WITHIN(0.5, pf_f_to_c(225), ctrl.setpoint_c);
	free(soon);

	/* the power was out too long: Error, and no attempt to light */
	cJSON_ReplaceItemInObject(o, "saved_wall", cJSON_CreateNumber(pf_wall() - 900));
	char *late = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	tick(2);
	TEST_ASSERT_TRUE(pf_control_recover(&ctrl, late, now));
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E08_POWER_LOSS", ctrl.safety.error_code);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));
	free(late);
	free(snap);

	/* stopping the cook takes the checkpoint away */
	pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	pf_control_set_checkpoint_path(&ctrl, "/tmp/pf_test_checkpoint.json");
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(30);
	TEST_ASSERT_TRUE(pf_file_exists("/tmp/pf_test_checkpoint.json"));
	pf_cmd_simple(PF_CMD_STOP);
	tick(2);
	TEST_ASSERT_FALSE_MESSAGE(pf_file_exists("/tmp/pf_test_checkpoint.json"), "a stopped grill has nothing to recover");
}

/* A hold temperature chosen during startup must not end startup: the fire is not lit. It is
 * where startup goes when it finishes. Ryan changed the hold temperature while the grill was
 * lighting and it went straight to Hold with a cold pot. */
static void test_a_hold_request_during_startup_waits_for_ignition(void)
{
	pf_cmd_mode(PF_MODE_STARTUP, 0);
	tick(30);
	TEST_ASSERT_EQUAL(PF_MODE_STARTUP, ctrl.mode);
	pf_cmd_mode(PF_MODE_HOLD, 250);            /* a Hold request, unforced */
	tick(5);
	TEST_ASSERT_EQUAL_MESSAGE(PF_MODE_STARTUP, ctrl.mode, "startup goes on until the fire is lit");
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.next_mode);
	TEST_ASSERT_DOUBLE_WITHIN(0.5, pf_f_to_c(250), ctrl.setpoint_c);
	pf_cmd c = { .type = PF_CMD_SETPOINT, .num = 275 };   /* and a plain set-point change likewise */
	pf_cmdq_push(&c);
	tick(5);
	TEST_ASSERT_EQUAL(PF_MODE_STARTUP, ctrl.mode);
	TEST_ASSERT_DOUBLE_WITHIN(0.5, pf_f_to_c(275), ctrl.setpoint_c);
	tick(300);
	TEST_ASSERT_EQUAL_MESSAGE(PF_MODE_HOLD, ctrl.mode, "and it finishes into Hold at that temperature");
	TEST_ASSERT_DOUBLE_WITHIN(0.5, pf_f_to_c(275), ctrl.setpoint_c);
}

/* A rest-to target moves the take-off with the climb: aimed 2 F over the rest asked for, less the
 * carry-over the rate predicts. A fast climb comes off earlier than a slow one for the same rest. */
static void test_a_rest_target_comes_off_early_by_the_climb(void)
{
	double rest = pf_f_to_c(145);
	double still = pf_notify_rest_pull_c(rest, 0, NAN, NAN, ""), slow = pf_notify_rest_pull_c(rest, 0.002, NAN, NAN, ""), fast = pf_notify_rest_pull_c(rest, 0.01, NAN, NAN, "");
	printf("rest to 145 F: take off at %.1f F still, %.1f F slow climb, %.1f F fast climb\n", pf_c_to_f(still), pf_c_to_f(slow), pf_c_to_f(fast));
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.05, 147.0, pf_c_to_f(still), "with no climb it aims 2 F over the rest");
	TEST_ASSERT_TRUE_MESSAGE(slow < still && fast < slow, "the faster it climbs, the earlier it comes off");
	TEST_ASSERT_TRUE_MESSAGE(pf_c_to_f(fast) >= 147.0 - 9.0 - 0.01, "and never by more than the carry-over cap");
	/* through the command: the status reports the rest and a take-off at or under the aim */
	pf_cmd c = { .type = PF_CMD_NOTIFY_TARGET, .num = 145, .flag = true };
	pf_strlcpy(c.str, "Probe1", sizeof c.str);
	pf_cmdq_push(&c);
	tick(2);
	const pf_notify_probe *np = pf_notify_find(&ctrl.notify, "Probe1");
	TEST_ASSERT_NOT_NULL(np);
	TEST_ASSERT_DOUBLE_WITHIN(0.05, rest, np->rest_c);
	TEST_ASSERT_TRUE(np->target_c <= rest + PF_REST_MARGIN_C + 1e-6);
}

/* Carry-over learns from rests: a meat whose rests keep carrying further than predicted comes off
 * earlier next time, and past the base cap once it has shown it needs to. */
static void test_carryover_learns_from_rests(void)
{
	double rate = 0.008;                                   /* C per second at the pull, no cooking temperature known */
	double base = pf_carryover_learned_c(rate, NAN, NAN, "Roast");
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 3.36, base);           /* 0.008 x 420 s */
	for (int i = 0; i < 5; i++) pf_carryover_learn("Roast", base, base * 2.0);   /* it rested twice as far, every time */
	double learned = pf_carryover_learned_c(rate, NAN, NAN, "Roast");
	printf("carry-over at 0.008 C/s: base %.2f C, after five rests of twice that %.2f C (x%.2f)\n", base, learned, pf_carryover_k("Roast"));
	TEST_ASSERT_TRUE_MESSAGE(learned > base * 1.7, "five rests of twice the prediction move the estimate most of the way");
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.001, 1.0, pf_carryover_k("Fish"), "another meat is untouched");
	TEST_ASSERT_TRUE(pf_notify_rest_pull_c(pf_f_to_c(135), rate, NAN, NAN, "Roast") < pf_notify_rest_pull_c(pf_f_to_c(135), rate, NAN, NAN, "Fish"));
}

/* The model tells a flank steak from a thick one and a hot cook from a low one, from the cook
 * itself: the thin cut climbs fast for its gap, the thick one slowly. Each pair is the same meat
 * at the same pull temperature; only the cut or the heat differs. */
static void test_carryover_follows_the_cut_and_the_heat(void)
{
	double c130 = pf_f_to_c(130), grill = pf_f_to_c(450);
	double flank = pf_carryover_model_c(10.0 * 5 / 9 / 60, c130, grill);                        /* 10 F a minute */
	double one = pf_carryover_model_c(3.0 * 5 / 9 / 60, c130, grill);                          /* 3 F a minute  */
	double two = pf_carryover_model_c(2.0 * 5 / 9 / 60, c130, grill);                          /* 2 F a minute  */
	double low = pf_carryover_model_c(0.5 * 5 / 9 / 60, pf_f_to_c(125), pf_f_to_c(225));      /* roast, low and slow */
	double hot = pf_carryover_model_c(1.0 * 5 / 9 / 60, pf_f_to_c(125), pf_f_to_c(350));      /* roast, hot */
	double brisket = pf_carryover_model_c(0.3 * 5 / 9 / 60, pf_f_to_c(200), pf_f_to_c(250));
	printf("carry-over, F: flank %.1f, 1in steak %.1f, 2in steak %.1f, roast at 225 %.1f, roast at 350 %.1f, brisket %.1f\n",
	       flank * 9 / 5, one * 9 / 5, two * 9 / 5, low * 9 / 5, hot * 9 / 5, brisket * 9 / 5);
	TEST_ASSERT_TRUE_MESSAGE(flank < one && one < two, "on the same grill, the thicker cut carries further");
	TEST_ASSERT_TRUE_MESSAGE(low < hot, "the same roast carries further off a hotter grill");
	TEST_ASSERT_TRUE_MESSAGE(flank * 9 / 5 < 4 && two * 9 / 5 > 7, "a flank steak a couple of degrees, a 2 inch steak most of ten");
	TEST_ASSERT_TRUE_MESSAGE(brisket * 9 / 5 < 3, "a brisket near 200 barely moves");
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-9, 0, pf_carryover_model_c(0.01, pf_f_to_c(150), pf_f_to_c(140)), "nothing carries when the meat is hotter than its surroundings");
}

/* A Bluetooth probe's ambient sensor says when the meat came off: the rest is timed from there,
 * and the carry-over it shows is what the next estimate learns from. */
static void test_the_ambient_sensor_sees_the_meat_come_off(void)
{
	pf_notify nt; memset(&nt, 0, sizeof nt);
	pf_sensors s; memset(&s, 0, sizeof s);
	s.n = 2; s.primary = -1;
	pf_strlcpy(s.p[0].label, "BTX", sizeof s.p[0].label); pf_strlcpy(s.p[0].name, "BTX", sizeof s.p[0].name);
	s.p[0].role = PF_PROBE_FOOD; s.p[0].enabled = true; s.p[0].valid = true; s.p[0].companion = 1;
	pf_strlcpy(s.p[1].label, "BTXAmb", sizeof s.p[1].label); s.p[1].role = PF_PROBE_AUX; s.p[1].enabled = true; s.p[1].valid = true;
	s.p[1].companion = -1; s.p[1].is_companion = true;
	pf_notify_sync(&nt, &s);
	TEST_ASSERT_EQUAL(0, pf_notify_set_rest(&nt, "BTX", pf_f_to_c(140), 0));
	pf_notify_set_target_note(&nt, "BTX", "Testmeat", "Medium", pf_f_to_c(140));
	double t = 1000, centre = pf_f_to_c(100);
	/* on a 400 F grill, climbing 2 F a minute */
	for (int k = 0; k < 400; k++) { t += 3; centre += 2.0 * 5 / 9 / 20; s.p[0].temp_c = centre; s.p[1].temp_c = pf_f_to_c(400); pf_notify_tick(&nt, &s, PF_MODE_HOLD, t, PF_UNITS_F); }
	const pf_notify_probe *np = pf_notify_find(&nt, "BTX");
	TEST_ASSERT_FALSE(np->removed);
	/* off the grill: the ambient falls to the room */
	for (int k = 0; k < 3; k++) { t += 3; s.p[1].temp_c = pf_f_to_c(75); pf_notify_tick(&nt, &s, PF_MODE_HOLD, t, PF_UNITS_F); }
	TEST_ASSERT_TRUE_MESSAGE(np->removed, "the fall in ambient is a removal");
	TEST_ASSERT_TRUE_MESSAGE(np->rest_watch, "and the rest is watched from that moment");
	double pulled = np->pull_c;
	/* the rest: up 6 F over ten minutes, then away */
	for (int k = 0; k < 200; k++) { t += 3; s.p[0].temp_c = pulled + (6.0 * 5 / 9) * k / 200.0; pf_notify_tick(&nt, &s, PF_MODE_HOLD, t, PF_UNITS_F); }
	for (int k = 0; k < 20; k++) { t += 3; s.p[0].temp_c -= 0.2; pf_notify_tick(&nt, &s, PF_MODE_HOLD, t, PF_UNITS_F); }
	TEST_ASSERT_FALSE_MESSAGE(np->rest_watch, "the rest peaked and fell, so it has been learned from");
	printf("rest seen by the ambient sensor: predicted %.1f F, rested %.1f F, correction x%.2f\n", np->pull_predicted_c * 9 / 5, 6.0, pf_carryover_k("Testmeat"));
	TEST_ASSERT_TRUE(pf_carryover_k("Testmeat") != 1.0);
}

int main(void)
{
	pf_log_init(PF_LOG_WARN);
	UNITY_BEGIN();
	RUN_TEST(test_full_cook);
	RUN_TEST(test_lid_open_pauses_feed);
	RUN_TEST(test_a_lid_drop_is_recognised_and_restarts_the_clock);
	RUN_TEST(test_a_grill_cooling_by_itself_is_not_a_lid);
	RUN_TEST(test_overtemp_errors);
	RUN_TEST(test_a_dead_pot_is_not_fed_for_long);
	RUN_TEST(test_a_fire_lost_on_the_way_up_is_caught_at_the_drop);
	RUN_TEST(test_a_dead_pot_in_smoke_is_caught);
	RUN_TEST(test_hold_from_manual_lights_first);
	RUN_TEST(test_a_huge_prime_is_clamped);
	RUN_TEST(test_reignite_and_prime_cannot_be_requested_mid_cook);
	RUN_TEST(test_a_set_point_cannot_reach_the_limit);
	RUN_TEST(test_recovery_is_refused_when_it_is_not_safe);
	RUN_TEST(test_a_stop_request_ends_a_recipe);
	RUN_TEST(test_overtemp_with_a_falling_pit_keeps_the_fan);
	RUN_TEST(test_stop_records_nothing);
	RUN_TEST(test_flameout_protection_lights_the_igniter_and_recovers);
	RUN_TEST(test_a_big_step_down_lights_the_igniter_at_the_crossing);
	RUN_TEST(test_flameout_reignites_then_errors);
	RUN_TEST(test_coldstart_winter);
	RUN_TEST(test_coldstart_failure);
	RUN_TEST(test_the_igniter_alone_cannot_pass_smart_start);
	RUN_TEST(test_a_stalled_heat_up_is_a_failed_start);
	RUN_TEST(test_a_start_at_minus_thirty);
	RUN_TEST(test_a_hot_restart_is_proven_by_the_stopped_fall);
	RUN_TEST(test_a_hot_dead_pot_is_not_proven);
	RUN_TEST(test_smoke_that_cannot_hold_180_is_stopped);
	RUN_TEST(test_smart_start_defaults_error_after_five_minutes);
	RUN_TEST(test_manual_refused_and_override_expires);
	RUN_TEST(test_warm_restart_resumes_hold);
	RUN_TEST(test_power_loss_recovery);
	RUN_TEST(test_a_hold_request_during_startup_waits_for_ignition);
	RUN_TEST(test_a_rest_target_comes_off_early_by_the_climb);
	RUN_TEST(test_carryover_learns_from_rests);
	RUN_TEST(test_carryover_follows_the_cut_and_the_heat);
	RUN_TEST(test_the_ambient_sensor_sees_the_meat_come_off);
	return UNITY_END();
}
