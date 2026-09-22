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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(void)
{
	pf_log_init(PF_LOG_WARN);
	UNITY_BEGIN();
	RUN_TEST(test_full_cook);
	RUN_TEST(test_lid_open_pauses_feed);
	RUN_TEST(test_overtemp_errors);
	RUN_TEST(test_flameout_protection_lights_the_igniter_and_recovers);
	RUN_TEST(test_flameout_reignites_then_errors);
	RUN_TEST(test_coldstart_winter);
	RUN_TEST(test_coldstart_failure);
	RUN_TEST(test_manual_refused_and_override_expires);
	RUN_TEST(test_warm_restart_resumes_hold);
	return UNITY_END();
}
