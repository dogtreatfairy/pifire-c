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
	pf_sim_model()->lid_open = true;
	tick(180);
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

static void test_flameout_reignites_then_errors(void)
{
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

int main(void)
{
	pf_log_init(PF_LOG_WARN);
	UNITY_BEGIN();
	RUN_TEST(test_full_cook);
	RUN_TEST(test_lid_open_pauses_feed);
	RUN_TEST(test_overtemp_errors);
	RUN_TEST(test_flameout_reignites_then_errors);
	RUN_TEST(test_coldstart_winter);
	RUN_TEST(test_coldstart_failure);
	RUN_TEST(test_manual_refused_and_override_expires);
	return UNITY_END();
}
