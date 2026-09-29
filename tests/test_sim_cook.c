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

void setUp(void)
{
	snprintf(cfg_path, sizeof cfg_path, "/tmp/pf_simcook_%d.json", (int)getpid());
	snprintf(db_path, sizeof db_path, "/tmp/pf_simcook_%d.db", (int)getpid());
	unlink(cfg_path); unlink(db_path);
	TEST_ASSERT_EQUAL_INT(0, pf_settings_init(cfg_path));
	pf_settings_force_sim();
	pf_settings_patch("startup", "{\"smartstart\":{\"enabled\":false},\"startup_exit_temp\":0,\"start_to_mode\":{\"after_startup_mode\":\"Smoke\",\"primary_setpoint\":165}}", NULL, 0);
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
	tick(245);
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

static void test_overtemp_errors(void)
{
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 10 * 60);
	pf_sim_model()->pit_c = pf_f_to_c(600);
	for (int i = 0; i < 8; i++) pf_sim_model()->delay[i] = pf_sim_model()->pit_c;
	tick(15);
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E01_OVERTEMP", ctrl.safety.error_code);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_AUGER));
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_FAN)); /* cooldown */
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

/* The pit falling away from a set point it was holding is a fire that is failing. The igniter is
   the cheap answer, and it goes on long before the grill has cooled far enough for the old fixed
   floor to call it a flame-out. It comes off again once the pit has climbed back from its lowest
   point, which is the evidence the fire has taken rather than that the igniter is warming the pot. */
static void test_flameout_protection_lights_the_igniter_and_recovers(void)
{
	pf_settings_patch("safety", "{\"relight_enabled\":true,\"relight_drop\":20,\"relight_recover\":10}", NULL, 0);
	pf_control_reload_settings(&ctrl);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 20 * 60);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_TRUE(ctrl.target_reached);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));

	/* the fire goes out with the grill sitting on its target */
	pf_sim_model()->fire_lit = false;
	pf_sim_model()->pot_pellets_g = 0;
	double t = 0;
	while (!ctrl.safety.relight_active && t < 30 * 60) { tick(5); t += 5; }
	printf("relight after %.0f s at pit %.1f C (set point %.1f C)\n", t, ctrl.pit_c, ctrl.setpoint_c);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.safety.relight_active, "a pit falling away from its target should light the igniter");
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	TEST_ASSERT_EQUAL_MESSAGE(PF_MODE_HOLD, ctrl.mode, "it stays in Hold: this is a rescue, not a restart");
	/* it triggered on the way down, so the drop should be about the configured twenty degrees */
	TEST_ASSERT_TRUE(pf_delta_from_c(ctrl.setpoint_c - ctrl.pit_c, PF_UNITS_F) >= 19);

	/* the fire catches: once the pit is back up from its low, the igniter is no longer needed */
	double low = ctrl.safety.relight_low_c;
	t = 0;
	while (ctrl.safety.relight_active && t < 20 * 60) { tick(5); t += 5; }
	printf("igniter off after %.0f s, pit %.1f C from a low of %.1f C\n", t, ctrl.pit_c, low);
	TEST_ASSERT_FALSE_MESSAGE(ctrl.safety.relight_active, "the igniter should not stay on once the fire has taken");
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
}

/* Lowering the set point a long way makes the grill starve the fire on purpose and coast down, and
   that coast is exactly when a fire dies -- by the end of it there may be nothing left to catch.
   Waiting for another twenty degrees of undershoot would mean waiting through the most dangerous
   part of it, so the igniter goes on the moment the pit crosses the new set point on the way down,
   and comes off once the pit has stopped falling and climbed back. */
static void test_a_big_step_down_lights_the_igniter_at_the_crossing(void)
{
	pf_settings_patch("safety", "{\"relight_enabled\":true,\"relight_drop\":20,\"relight_recover\":10,\"relight_recover_step\":3}", NULL, 0);
	pf_control_reload_settings(&ctrl);
	pf_cmd_mode(PF_MODE_HOLD, 300);
	tick(250 + 25 * 60);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_TRUE(ctrl.target_reached);
	TEST_ASSERT_FALSE(pf_outputs_get(PF_OUT_IGNITER));

	/* down a long way: the grill stops feeding and the barrel coasts */
	{ pf_cmd sp = { .type = PF_CMD_SETPOINT, .num = 225 }; pf_cmdq_push(&sp); }
	tick(5);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.safety.stepdown_armed, "a big step down should arm the coast watch");
	TEST_ASSERT_FALSE_MESSAGE(pf_outputs_get(PF_OUT_IGNITER), "nothing to do while the pit is still above the target");

	/* nothing happens on the way down until the pit reaches the new set point */
	double t = 0;
	while (!ctrl.safety.relight_active && t < 90 * 60) { tick(5); t += 5; }
	printf("step-down: igniter at pit %.1f F, set point %.1f F, after %.0f s\n",
	       pf_from_c(ctrl.pit_c, PF_UNITS_F), pf_from_c(ctrl.setpoint_c, PF_UNITS_F), t);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.safety.relight_active, "crossing the lowered set point should light the igniter");
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	/* it fired at the crossing, not twenty degrees below it */
	double below_f = pf_from_c(ctrl.setpoint_c, PF_UNITS_F) - pf_from_c(ctrl.pit_c, PF_UNITS_F);
	printf("step-down: fired %.1f F below the new set point\n", below_f);
	TEST_ASSERT_TRUE_MESSAGE(below_f < 10, "it should fire at the crossing, not after a long undershoot");
	TEST_ASSERT_EQUAL_MESSAGE(PF_MODE_HOLD, ctrl.mode, "coasting to a lower target is not a flame-out");

	/* The pit keeps falling for a while with the igniter on, so the low it is judged against is
	   the bottom of the dip and not where it was when the igniter came on. */
	t = 0;
	while (ctrl.safety.relight_active && t < 30 * 60) { tick(5); t += 5; }
	double low_f = pf_from_c(ctrl.safety.relight_low_c, PF_UNITS_F), now_f = pf_from_c(ctrl.pit_c, PF_UNITS_F);
	printf("step-down: igniter off after %.0f s, pit %.1f F from a low of %.1f F (rise %.1f F)\n",
	       t, now_f, low_f, now_f - low_f);
	TEST_ASSERT_FALSE_MESSAGE(ctrl.safety.relight_active, "once the pit is climbing again the igniter is not needed");
	/* Coasting down the fire was starved rather than lost, so the question is only whether the pit
	   has turned around. A few degrees answers it; demanding the full ten would hold the igniter on
	   well past the point where the grill is plainly recovering. */
	TEST_ASSERT_TRUE_MESSAGE(now_f - low_f >= 2.5, "it should wait for a real turnaround");
	TEST_ASSERT_TRUE_MESSAGE(now_f - low_f < 8, "a level change should not need a full relight's worth of rise");
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
}

/* The hard floor is the last word, and it is what the assist hands over to. With the assist off,
   this is the original path: the fire is out, the grill re-ignites, and a second failure errors. */
static void test_flameout_reignites_then_errors(void)
{
	pf_settings_patch("safety", "{\"relight_enabled\":false}", NULL, 0);
	pf_control_reload_settings(&ctrl);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 20 * 60);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	/* fire dies: empty the pot and put the fire out */
	pf_sim_model()->fire_lit = false;
	pf_sim_model()->pot_pellets_g = 0;
	/* pellets that arrive can't relight without the igniter */
	double t = 0;
	while (ctrl.mode == PF_MODE_HOLD && t < 40 * 60) { tick(10); t += 10; pf_sim_model()->fire_lit = false; }
	TEST_ASSERT_EQUAL(PF_MODE_REIGNITE, ctrl.mode);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	/* let reignite succeed normally */
	tick(250);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_EQUAL_INT(0, ctrl.safety.reignite_retries_left);
	/* second flame-out -> error */
	pf_sim_model()->fire_lit = false;
	pf_sim_model()->pot_pellets_g = 0;
	t = 0;
	while (ctrl.mode == PF_MODE_HOLD && t < 40 * 60) { tick(10); t += 10; pf_sim_model()->fire_lit = false; }
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E02_FLAMEOUT", ctrl.safety.error_code);
}

static void test_coldstart_winter(void)
{
	pf_settings_patch("safety", "{\"coldstart\":{\"enabled\":true,\"delta_rise\":12,\"timeout_s\":300,\"baseline_window_s\":60}}", NULL, 0);
	pf_control_reload_settings(&ctrl);
	pf_sim_reset(-5.0); /* 23 F ambient, below minstartuptemp */
	tick(15);           /* let the probe filter settle on the cold reading */
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(1);
	TEST_ASSERT_EQUAL(PF_MODE_STARTUP, ctrl.mode);
	TEST_ASSERT_TRUE(ctrl.safety.coldstart_active);
	TEST_ASSERT_DOUBLE_WITHIN(1.5, -5.0, ctrl.safety.baseline_c);
	tick(320);
	TEST_ASSERT_TRUE(ctrl.safety.coldstart_reached || ctrl.mode == PF_MODE_HOLD);
	tick(40 * 60);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("", ctrl.safety.error_code);
	TEST_ASSERT_DOUBLE_WITHIN(10.0, pf_f_to_c(225), ctrl.pit_c);
	/* ambient estimate came from the baseline */
	TEST_ASSERT_DOUBLE_WITHIN(3.0, -5.0, ctrl.ambient_c);
}

static void test_coldstart_failure(void)
{
	pf_settings_patch("safety", "{\"reigniteretries\":0,\"coldstart\":{\"enabled\":true,\"delta_rise\":12,\"timeout_s\":180,\"baseline_window_s\":60}}", NULL, 0);
	pf_control_reload_settings(&ctrl);
	pf_sim_reset(-5.0);
	tick(3);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	/* igniter never lights: keep the pot empty */
	for (int i = 0; i < 40; i++) { tick(5); pf_sim_model()->pot_pellets_g = 0; pf_sim_model()->fire_lit = false; }
	TEST_ASSERT_EQUAL(PF_MODE_ERROR, ctrl.mode);
	TEST_ASSERT_EQUAL_STRING("E04_STARTUP_FAILED", ctrl.safety.error_code);
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
	pf_settings_patch("safety", "{\"power_loss\":{\"recovery\":true,\"max_s\":300,\"igniter_s\":180}}", NULL, 0);
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
	TEST_ASSERT_TRUE(pf_control_recover(&ctrl, soon, now));
	TEST_ASSERT_EQUAL(PF_MODE_REIGNITE, ctrl.mode);
	TEST_ASSERT_DOUBLE_WITHIN(1, 180, ctrl.startup_duration_s);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.safety.reignite_last);
	TEST_ASSERT_TRUE(pf_outputs_get(PF_OUT_IGNITER));
	tick(200);
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
	RUN_TEST(test_stop_records_nothing);
	RUN_TEST(test_flameout_protection_lights_the_igniter_and_recovers);
	RUN_TEST(test_a_big_step_down_lights_the_igniter_at_the_crossing);
	RUN_TEST(test_flameout_reignites_then_errors);
	RUN_TEST(test_coldstart_winter);
	RUN_TEST(test_coldstart_failure);
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
