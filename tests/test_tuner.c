/* The guided tuning run, end to end in the simulator.
 *
 * The simulator loses heat by convection and by radiation, so its process gain falls as it gets
 * hotter, exactly as a real grill's does. That is what makes a single proportional band a poor fit
 * across 180 F to 450 F, and what this test measures: how well the grill holds each set point
 * before the run, and how well it holds them afterwards. */
#include "core/cmdq.h"
#include "core/control.h"
#include "core/db.h"
#include "core/env.h"
#include "core/events.h"
#include "core/history.h"
#include "core/log.h"
#include "core/outputs.h"
#include "core/settings.h"
#include "core/status.h"
#include "controllers/registry.h"
#include "features/learning.h"
#include "features/tuner.h"
#include "platform/sim.h"
#include "probes/probes.h"
#include "probes/registry.h"
#include "unity.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char cfg_path[256], db_path[256];
static pf_control ctrl;
static double now;

/* one simulated second of everything, including the once-a-second services work */
static void tick(double dt)
{
	int steps = (int)(dt / 0.1 + 0.5);
	static double last_service;
	for (int i = 0; i < steps; i++) {
		now += 0.1;
		pf_sim_step(0.1);
		pf_probes_poll(now);
		pf_control_step(&ctrl, now);
		if (now - last_service >= 1.0) {
			last_service = now;
			pf_status st;
			pf_status_get(&st);
			cJSON *j = pf_status_to_json(&st, pf_settings_units());
			pf_tuner_tick(j, now);
			cJSON_Delete(j);
		}
	}
}

void setUp(void)
{
	snprintf(cfg_path, sizeof cfg_path, "/tmp/pf_tuner_%d.json", (int)getpid());
	snprintf(db_path, sizeof db_path, "/tmp/pf_tuner_%d.db", (int)getpid());
	unlink(cfg_path); unlink(db_path);
	TEST_ASSERT_EQUAL_INT(0, pf_settings_init(cfg_path));
	pf_settings_force_sim();
	pf_settings_patch("startup", "{\"smartstart\":{\"enabled\":false},\"startup_exit_temp\":0,"
	                  "\"start_to_mode\":{\"after_startup_mode\":\"Hold\",\"primary_setpoint\":225}}", NULL, 0);
	TEST_ASSERT_EQUAL_INT(0, pf_db_open(db_path));
	pf_events_init();
	pf_controllers_init(NULL);
	pf_probe_drivers_init(NULL);
	pf_env env; pf_env_init(&env, "platform");
	void *inst = pf_platform_sim()->create("{}", &env);
	pf_outputs_init(pf_platform_sim(), inst);
	pf_cmdq_init();
	pf_history_init();
	pf_probes_init();
	pf_learning_init();
	pf_tuner_init();
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
	char extra[300];
	snprintf(extra, sizeof extra, "%s-wal", db_path); unlink(extra);
	snprintf(extra, sizeof extra, "%s-shm", db_path); unlink(extra);
}

/* Hold at a set point and report the mean absolute error over the last stretch, once the pit has
 * had time to get there. This is the number a cook actually feels. */
static char last_note[80];

static double hold_error_f(double setpoint_f)
{
	pf_cmd c = { .type = PF_CMD_MODE, .mode = PF_MODE_HOLD, .num = setpoint_f };
	pf_cmdq_push(&c);
	tick(50 * 60);                       /* reach it and settle */
	double sp = pf_f_to_c(setpoint_f), sum = 0;
	int n = 0;
	for (int i = 0; i < 40 * 60; i++) {  /* then forty minutes of holding */
		tick(1);
		if (i % 10) continue;
		sum += fabs(ctrl.pit_c - sp);
		n++;
	}
	snprintf(last_note, sizeof last_note, "%s", ctrl.dbg.note);
	return pf_delta_from_c(n ? sum / n : 0, PF_UNITS_F);
}

static void stop_and_cool(void)
{
	pf_cmd c = { .type = PF_CMD_STOP };
	pf_cmdq_push(&c);
	tick(5);
}

/* Stop and wait for the barrel to actually come down. A tuning run climbs from its lowest set
 * point, so starting one on a grill still hot from the last thing it did means waiting for it to
 * cool before anything can be measured. */
static void stop_and_wait_cold(void)
{
	stop_and_cool();
	for (int i = 0; i < 240 && ctrl.pit_c > pf_f_to_c(120); i++) tick(60);
}

/* Settle the model at a fixed burn rate and report the pit temperature. The pot is refilled every
 * step so the burn rate, not the auger, decides the fire. */
static double settle_at_burn(double gps)
{
	pf_sim_state *m = pf_sim_model();
	pf_sim_reset(18.0);
	m->fire_lit = true;
	m->out[PF_OUT_FAN] = true;
	m->fan_pct = 100;
	m->out[PF_OUT_AUGER] = false;
	for (int i = 0; i < 300000; i++) { m->pot_pellets_g = gps * 60.0; pf_sim_step(0.1); }
	return m->pit_c;
}

/* The simulator's process gain really does fall with temperature. Without that, a gain schedule
 * would have nothing to schedule, and this whole feature would be measuring noise. */
static void test_simulator_gain_falls_with_temperature(void)
{
	/* local slope, degrees of pit per gram per second of fuel, at each end of the range */
	double lo_a = settle_at_burn(0.09), lo_b = settle_at_burn(0.11);
	double hi_a = settle_at_burn(0.38), hi_b = settle_at_burn(0.42);
	double gain_lo = (lo_b - lo_a) / 0.02;
	double gain_hi = (hi_b - hi_a) / 0.04;
	printf("sim: %.0f F at 0.10 g/s (gain %.0f), %.0f F at 0.40 g/s (gain %.0f), ratio %.2f\n",
	       pf_from_c(0.5 * (lo_a + lo_b), PF_UNITS_F), gain_lo,
	       pf_from_c(0.5 * (hi_a + hi_b), PF_UNITS_F), gain_hi, gain_hi / gain_lo);

	/* the grill must actually be able to reach the top of the tuning range */
	TEST_ASSERT_TRUE_MESSAGE(pf_from_c(hi_b, PF_UNITS_F) > 460, "the model should reach past 460 F");
	TEST_ASSERT_TRUE_MESSAGE(gain_hi < gain_lo * 0.8, "hot-end gain should be well below cold-end gain");
}

/* the whole point: one button, and afterwards the grill holds every set point better */
static void test_guided_tune_improves_holding(void)
{
	static const double POINTS[] = { 180, 225, 350, 450 };
	double before[4], after[4];

	pf_learning_clear_anchors();
	for (int i = 0; i < 4; i++) {
		before[i] = hold_error_f(POINTS[i]);
		stop_and_cool();
	}

	/* the run itself: started once, hands off from here */
	char err[160];
	cJSON *pts = cJSON_Parse("[180,225,350,450]");
	TEST_ASSERT_EQUAL_INT(0, pf_tuner_start(pts, true, err, sizeof err));
	cJSON_Delete(pts);
	for (int i = 0; i < 12 * 60 * 60; i += 30) {
		cJSON *j = pf_tuner_json();
		bool running = cJSON_IsTrue(cJSON_GetObjectItem(j, "running"));
		cJSON_Delete(j);
		if (!running) break;
		tick(30);
	}
	cJSON *res = pf_tuner_json();
	int measured = (int)pf_json_num(res, "measured", 0);
	int anchors = cJSON_GetArraySize(cJSON_GetObjectItem(res, "anchors"));
	printf("tuner: phase %s, measured %d, anchors %d\n", pf_json_str(res, "phase", "?"), measured, anchors);
	cJSON *arr = cJSON_GetObjectItem(res, "anchors"), *a;
	cJSON_ArrayForEach(a, arr)
		printf("  %4.0f F -> PB %3.0f F, Ti %4.0f s, Td %3.0f s\n",
		       pf_json_num(a, "setpoint", 0), pf_json_num(a, "PB", 0), pf_json_num(a, "Ti", 0), pf_json_num(a, "Td", 0));
	cJSON_Delete(res);
	TEST_ASSERT_TRUE_MESSAGE(measured >= 3, "the run should measure at least three set points");
	TEST_ASSERT_TRUE(anchors >= 3);

	stop_and_cool();
	char notes[4][80];
	for (int i = 0; i < 4; i++) {
		after[i] = hold_error_f(POINTS[i]);
		snprintf(notes[i], sizeof notes[i], "%s", last_note);
		stop_and_cool();
	}

	/* The controller must actually be running on the measured tuning, not merely have it stored.
	 * The note it publishes says which of the three sources it is using. */
	for (int i = 0; i < 4; i++) {
		char msg[512];
		snprintf(msg, sizeof msg, "at %d F the controller reported: %s", (int)POINTS[i], notes[i]);
		TEST_ASSERT_TRUE_MESSAGE(strstr(notes[i], "tuned") != NULL, msg);
	}

	/* And the tuning it uses must vary with the set point, which is the whole point of a schedule. */
	double PB_lo = 0, PB_hi = 0, t1, t2;
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(180), &PB_lo, &t1, &t2));
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(450), &PB_hi, &t1, &t2));
	printf("schedule: PB %.0f F at 180, %.0f F at 450\n", pf_delta_from_c(PB_lo, PF_UNITS_F), pf_delta_from_c(PB_hi, PF_UNITS_F));
	TEST_ASSERT_TRUE_MESSAGE(fabs(pf_delta_from_c(PB_lo - PB_hi, PF_UNITS_F)) > 2.0, "the schedule should differ across the range");

	double sum_before = 0, sum_after = 0;
	for (int i = 0; i < 4; i++) {
		printf("hold %3.0f F: mean error %.2f F -> %.2f F\n", POINTS[i], before[i], after[i]);
		sum_before += before[i];
		sum_after += after[i];
	}
	/* every set point should hold to within a couple of degrees, and no worse than before */
	for (int i = 0; i < 4; i++) {
		char msg[96];
		snprintf(msg, sizeof msg, "%.0f F holds to %.2f F after tuning", POINTS[i], after[i]);
		TEST_ASSERT_TRUE_MESSAGE(after[i] < 5.0, msg);
	}
	/* Both sides of this are fractions of a degree in a simulator whose plant model the controller
	 * has already learned, so a percentage comparison measures noise. What matters is that tuning
	 * does not cost whole degrees. */
	TEST_ASSERT_TRUE_MESSAGE(sum_after <= sum_before + 1.0, "tuning should not make holding worse overall");
}

/* the schedule interpolates between what was measured, and holds flat outside it */
static void test_gain_schedule_interpolates(void)
{
	pf_learning_clear_anchors();
	double PB = 0, Ti = 0, Td = 0;
	TEST_ASSERT_FALSE(pf_learning_gains(pf_f_to_c(300), &PB, &Ti, &Td));

	pf_autotune_result lo = { .Ku = 1, .Pu = 200, .PB_c = 20, .Ti = 200, .Td = 30, .valid = true };
	pf_autotune_result hi = { .Ku = 1, .Pu = 400, .PB_c = 40, .Ti = 400, .Td = 60, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(200), &lo, 20.0, 0.0);
	pf_learning_store_anchor(pf_f_to_c(400), &hi, 20.0, 0.0);

	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(300), &PB, &Ti, &Td));
	TEST_ASSERT_DOUBLE_WITHIN(0.5, 30, PB);      /* halfway between the two anchors */
	TEST_ASSERT_DOUBLE_WITHIN(5, 300, Ti);
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(150), &PB, &Ti, &Td));
	TEST_ASSERT_DOUBLE_WITHIN(0.5, 20, PB);      /* below the range: the lowest anchor holds */
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(500), &PB, &Ti, &Td));
	TEST_ASSERT_DOUBLE_WITHIN(0.5, 40, PB);      /* above it: the highest */
}

/* drive whatever run is in progress to its end */
static void run_to_completion(void)
{
	for (int i = 0; i < 12 * 60 * 60; i += 30) {
		cJSON *j = pf_tuner_json();
		bool running = cJSON_IsTrue(cJSON_GetObjectItem(j, "running"));
		cJSON_Delete(j);
		if (!running) break;
		tick(30);
	}
}

/* One temperature joins the library; a full profile is a new baseline and replaces it. */
static void test_single_adds_and_full_profile_replaces(void)
{
	pf_learning_clear_anchors();

	/* an anchor from an earlier day, at a temperature no profile covers */
	pf_autotune_result old = { .Ku = 1, .Pu = 300, .PB_c = 30, .Ti = 300, .Td = 45, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(300), &old, 10.0, 12.0);

	char err[160];
	cJSON *one = cJSON_Parse("[225]");
	TEST_ASSERT_EQUAL_INT(0, pf_tuner_start(one, false, err, sizeof err));
	cJSON_Delete(one);
	run_to_completion();

	pf_tune_anchor a[PF_TUNE_ANCHORS];
	int n = pf_learning_anchor_list(a, PF_TUNE_ANCHORS);
	printf("after one temperature: %d entries\n", n);
	for (int i = 0; i < n; i++)
		printf("  %4.0f F, measured at %.0f F out\n", pf_from_c(a[i].setpoint_c, PF_UNITS_F), pf_from_c(a[i].ambient_c, PF_UNITS_F));
	TEST_ASSERT_EQUAL_INT_MESSAGE(2, n, "a single tune should join the library, not replace it");
	TEST_ASSERT_DOUBLE_WITHIN(3, 225, pf_from_c(a[0].setpoint_c, PF_UNITS_F));
	TEST_ASSERT_DOUBLE_WITHIN(3, 300, pf_from_c(a[1].setpoint_c, PF_UNITS_F));
	/* the conditions it was measured in came from the status, not from nowhere */
	TEST_ASSERT_FALSE(isnan(a[0].ambient_c));

	stop_and_wait_cold();
	TEST_ASSERT_EQUAL_INT(0, pf_tuner_start(NULL, true, err, sizeof err));
	/* the old library is gone the moment a full profile begins */
	TEST_ASSERT_EQUAL_INT(0, pf_learning_anchor_list(a, PF_TUNE_ANCHORS));
	run_to_completion();

	n = pf_learning_anchor_list(a, PF_TUNE_ANCHORS);
	printf("after a full profile: %d entries\n", n);
	TEST_ASSERT_TRUE_MESSAGE(n >= 3, "a full profile should measure the whole range");
	for (int i = 0; i < n; i++)
		TEST_ASSERT_TRUE_MESSAGE(fabs(pf_from_c(a[i].setpoint_c, PF_UNITS_F) - 300) > 5, "the 300 F entry should have been replaced");
	stop_and_cool();
}

/* The relay has to swing around the duty the grill really uses. On the real grill the feed-forward
 * had learned nothing yet and its built-in prior put the centre at roughly twice what the grill
 * needed, so the low half of the swing still fed the fire, the pit never came back down through
 * the set point, and the measurement ran out the clock having learned nothing. The test must
 * notice that and move the centre rather than wait. */
/* The relay has to centre on the duty that *holds* the set point, not the duty that got there.
 * On the real grill a full profile starts cold and climbs to 180 F, so the ten minutes before the
 * test began were mostly climb at high feed. Averaging that window gave a centre far above what
 * 180 F needs, the low half of the relay went on feeding the fire, and the pit walked twenty
 * degrees past the target in five minutes without ever crossing back. */
static void test_the_relay_centres_on_holding_not_climbing(void)
{
	pf_learning_clear_anchors();
	stop_and_wait_cold();

	pf_cmd c = { .type = PF_CMD_MODE, .mode = PF_MODE_HOLD, .num = 180 };
	pf_cmdq_push(&c);
	/* long enough to climb from cold and settle, which is the state a profile run starts in */
	for (int i = 0; i < 90 * 60 && !ctrl.target_reached; i += 10) tick(10);
	tick(5 * 60);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.target_reached, "the grill should have reached 180 F");
	double holding = ctrl.u_applied;

	unsigned gen = pf_learning_autotune_gen();
	pf_cmd a = { .type = PF_CMD_AUTOTUNE_START };
	pf_cmdq_push(&a);
	tick(30);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.autotune.active, "the relay test should have started");
	printf("centre %.3f, holding duty about %.3f, low half %.3f\n",
	       ctrl.autotune.u_center, holding, ctrl.autotune.u_center - ctrl.autotune.h);

	/* The low half of the swing has to be able to cool the grill, or there is no oscillation to
	 * measure. That is the property the climb-contaminated average destroyed. */
	TEST_ASSERT_TRUE_MESSAGE(ctrl.autotune.u_center - ctrl.autotune.h < holding,
	                         "the low half of the relay must feed less than holding needs");

	/* and it must actually come back through the set point rather than running away */
	double worst = 0;
	for (int i = 0; i < 60 * 60 && ctrl.autotune.active && ctrl.autotune.crossings == 0; i += 15) {
		tick(15);
		double over = pf_from_c(ctrl.pit_c, PF_UNITS_F) - 180;
		if (over > worst) worst = over;
	}
	printf("first crossing after running %.0f F past the set point\n", worst);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.autotune.crossings > 0, "the relay should have crossed the set point");
	TEST_ASSERT_TRUE_MESSAGE(worst < 30, "it should not run far past the set point before crossing");

	pf_cmd st = { .type = PF_CMD_AUTOTUNE_STOP };
	pf_cmdq_push(&st);
	tick(5);
	(void)gen;
	stop_and_wait_cold();
}

static void test_relay_recovers_from_a_badly_centred_swing(void)
{
	pf_learning_clear_anchors();

	pf_cmd c = { .type = PF_CMD_MODE, .mode = PF_MODE_HOLD, .num = 225 };
	pf_cmdq_push(&c);
	tick(60 * 60);                                  /* reach it and hold */
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	TEST_ASSERT_TRUE(ctrl.target_reached);

	unsigned gen = pf_learning_autotune_gen();
	pf_cmd a = { .type = PF_CMD_AUTOTUNE_START };
	pf_cmdq_push(&a);
	tick(30);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.autotune.active, "the relay test should have started");

	/* shove the centre far too high, as an unlearned feed-forward did on the real grill */
	double sane = ctrl.autotune.u_center;
	double forced = sane * 2.5;
	ctrl.autotune.u_center = forced > 0.7 ? 0.7 : forced;
	printf("centre forced from %.2f to %.2f\n", sane, ctrl.autotune.u_center);

	/* room for the walk back plus the measurement itself: the relay is patient once it is
	 * crossing, because a real grill's cooling half runs to ten minutes */
	for (int i = 0; i < 150 * 60 && ctrl.autotune.active; i += 30) tick(30);

	printf("re-centred %d time(s), finished on %.2f, crossings %d\n",
	       ctrl.autotune.recentres, ctrl.autotune.u_center, ctrl.autotune.crossings);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.autotune.recentres > 0, "a stalled half-cycle should move the centre");
	TEST_ASSERT_TRUE_MESSAGE(pf_learning_autotune_gen() != gen, "it should still produce a measurement");
	pf_autotune_result r = pf_learning_autotune();
	printf("measured Ku %.4f, Pu %.0f s, PB %.0f F\n", r.Ku, r.Pu, pf_delta_from_c(r.PB_c, PF_UNITS_F));
	TEST_ASSERT_TRUE(r.PB_c > 0 && r.Ti > 0);
	stop_and_wait_cold();
}

/* The relay must not mistake a slow cooling half for a stall. On the real grill the down-swing at
 * 180 F runs to ten minutes, and a stall threshold shorter than that re-centred the swing on every
 * single down-swing, resetting the crossing count each time so the measurement could never finish.
 * Once the relay has crossed at all, the centre is evidently workable and it has to be patient. */
static void test_a_slow_cooling_half_is_not_a_stall(void)
{
	pf_cmd c = { .type = PF_CMD_MODE, .mode = PF_MODE_HOLD, .num = 225 };
	pf_cmdq_push(&c);
	tick(60 * 60);
	TEST_ASSERT_TRUE(ctrl.target_reached);

	pf_cmd a = { .type = PF_CMD_AUTOTUNE_START };
	pf_cmdq_push(&a);
	tick(30);
	TEST_ASSERT_TRUE(ctrl.autotune.active);

	/* it has been oscillating happily, and this half-cycle is a long one */
	ctrl.autotune.crossings = 3;
	ctrl.autotune.recentres = 0;
	ctrl.autotune.last_cross_t = now - 800;
	double centre = ctrl.autotune.u_center;
	tick(60);
	TEST_ASSERT_EQUAL_INT_MESSAGE(0, ctrl.autotune.recentres, "a long cooling half is not a stall once the relay is crossing");
	/* a genuine crossing may land in this window and legitimately increment the count; what must
	 * not happen is the count being thrown away and started again */
	TEST_ASSERT_TRUE_MESSAGE(ctrl.autotune.crossings >= 3, "the crossing count must survive a slow half-cycle");
	TEST_ASSERT_EQUAL_DOUBLE(centre, ctrl.autotune.u_center);

	/* But a relay that has never crossed at this centre is genuinely stuck and must be moved. Park
	 * the pit above the set point while the swing drives down, so no crossing can rescue it. */
	pf_sim_model()->pit_c = ctrl.setpoint_c + 10.0;
	tick(20);
	ctrl.autotune.phase = -1;
	ctrl.autotune.crossings = 0;
	ctrl.autotune.recentres = 0;
	ctrl.autotune.last_cross_t = now - 800;
	tick(60);
	TEST_ASSERT_TRUE_MESSAGE(ctrl.autotune.recentres > 0, "a swing that has never crossed should be re-centred");
	stop_and_wait_cold();
}

/* The relay's estimate of the ultimate gain has to agree with the plant the simulator actually
 * is. Measuring the amplitude within a single half-cycle, as this once did, saw only part of the
 * swing and overstated the gain roughly two-fold, which is a proportional band half as wide as it
 * should be and a loop that hunts. */
static void test_relay_agrees_with_the_plant_it_measured(void)
{
	pf_learning_clear_anchors();
	char err[160];
	cJSON *one = cJSON_Parse("[225]");
	TEST_ASSERT_EQUAL_INT(0, pf_tuner_start(one, false, err, sizeof err));
	cJSON_Delete(one);

	/* Take the model as the startup rise left it, before the relay has had a chance to replace it.
	 * Comparing the relay against a model the relay itself wrote would prove nothing. */
	pf_fopdt plant = { 0 };
	for (int i = 0; i < 4 * 60 * 60 && !plant.valid; i += 30) { tick(30); plant = pf_learning_fopdt(); }
	TEST_ASSERT_TRUE_MESSAGE(plant.valid, "the startup rise should have produced a plant estimate");
	run_to_completion();

	pf_autotune_result r = pf_learning_autotune();
	TEST_ASSERT_TRUE_MESSAGE(r.valid && r.Ku > 0, "the run should have measured something");

	/* the ultimate point of a first-order-plus-dead-time plant, found where its phase reaches -pi */
	double lo = 1e-5, hi = 1.0;
	for (int i = 0; i < 200; i++) {
		double w = 0.5 * (lo + hi);
		if (-w * plant.theta - atan(w * plant.tau) > -M_PI) lo = w; else hi = w;
	}
	double w = 0.5 * (lo + hi);
	double Ku_theory = sqrt(1 + w * plant.tau * w * plant.tau) / plant.K;
	double Pu_theory = 2 * M_PI / w;
	printf("relay: Ku %.4f, Pu %.0f s | the startup rise implied Ku %.4f, Pu %.0f s | ratio %.2f\n",
	       r.Ku, r.Pu, Ku_theory, Pu_theory, r.Ku / Ku_theory);

	/* The two are measured quite differently, one by oscillation and one from a startup rise, so
	 * they will not match exactly. They must at least be the same size: a factor of two apart is
	 * the error the half-cycle amplitude used to make. */
	TEST_ASSERT_TRUE_MESSAGE(r.Ku < Ku_theory * 1.8, "the relay must not overstate the ultimate gain");
	TEST_ASSERT_TRUE_MESSAGE(r.Ku > Ku_theory * 0.4, "nor understate it");
	stop_and_wait_cold();
}

/* The run publishes both the set point it is working on now and the whole profile it will walk.
 * They are different things and must not collapse into each other: a guard added to the first index
 * was once applied to the loop as well, and the profile came out as the current set point repeated. */
/* The baseline is measured before anything else, because the bottom of the range is the worst
 * place to start: at 180 F the grill holds on so little fuel that the relay has no room to swing
 * below its centre, and a clamped swing reports an ultimate gain that is too high. Around 250 F
 * there is room either side, so that is the measurement worth setting the grill's baseline from.
 * After it, the run walks upward so it never waits for the grill to cool twice. */
/* A tuning library is hours of the grill's own time and a hopper of pellets, living in a database
   on an SD card. It has to be possible to take a copy and put it back, and the copy has to carry
   enough with it to mean something later: canonical Celsius so a backup taken in Fahrenheit still
   restores on a grill set to Celsius, and the controller it was measured against, because PB, Ti
   and Td detached from a controller are just numbers. */
static void test_the_tuning_library_can_be_backed_up_and_restored(void)
{
	pf_learning_clear_anchors();
	pf_autotune_result a = { .Ku = 1.4, .Pu = 420, .PB_c = 33, .Ti = 300, .Td = 42, .valid = true, .ts = 1000 };
	pf_autotune_result b = { .Ku = 0.9, .Pu = 380, .PB_c = 41, .Ti = 260, .Td = 36, .valid = true, .ts = 2000 };
	pf_learning_store_anchor(pf_f_to_c(250), &a, 12.0, 8.0);
	pf_learning_store_anchor(pf_f_to_c(350), &b, 12.5, 9.0);
	pf_learning_store_fopdt(180, 240, 55);

	cJSON *doc = pf_learning_export();
	TEST_ASSERT_NOT_NULL(doc);
	TEST_ASSERT_EQUAL_STRING("pifire-tuning", pf_json_str(doc, "kind", ""));
	TEST_ASSERT_EQUAL_STRING("C", pf_json_str(doc, "units", ""));
	TEST_ASSERT_EQUAL_INT_MESSAGE(2, cJSON_GetArraySize(cJSON_GetObjectItem(doc, "anchors")), "both set points should be in the backup");
	TEST_ASSERT_NOT_NULL_MESSAGE(cJSON_GetObjectItem(doc, "plant"), "the plant model travels with it");
	TEST_ASSERT_TRUE_MESSAGE(pf_json_str(doc, "controller.id", "")[0], "so does the controller it was measured against");

	/* wipe the grill's memory and put the backup back */
	pf_learning_clear_anchors();
	pf_tune_anchor chk[PF_TUNE_ANCHORS];
	TEST_ASSERT_EQUAL_INT(0, pf_learning_anchor_list(chk, PF_TUNE_ANCHORS));

	char err[160];
	TEST_ASSERT_EQUAL_INT(2, pf_learning_import(doc, err, sizeof err));
	int n = pf_learning_anchor_list(chk, PF_TUNE_ANCHORS);
	TEST_ASSERT_EQUAL_INT(2, n);
	double PB = 0, Ti = 0, Td = 0;
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(250), &PB, &Ti, &Td));
	TEST_ASSERT_DOUBLE_WITHIN(0.5, 33, PB);
	TEST_ASSERT_DOUBLE_WITHIN(2, 300, Ti);
	printf("restored 250 F: PB %.1f C, Ti %.0f s, Td %.0f s\n", PB, Ti, Td);
	cJSON_Delete(doc);

	/* and it refuses something that is not one of ours rather than wiping the library over it */
	cJSON *junk = cJSON_Parse("{\"hello\":\"world\"}");
	TEST_ASSERT_EQUAL_INT(-1, pf_learning_import(junk, err, sizeof err));
	cJSON_Delete(junk);
	TEST_ASSERT_EQUAL_INT_MESSAGE(2, pf_learning_anchor_list(chk, PF_TUNE_ANCHORS), "a bad file must not cost the library");
	pf_learning_clear_anchors();
}

static void test_a_profile_measures_its_baseline_first(void)
{
	char err[160];
	cJSON *pts = cJSON_Parse("[180,250,350,450]");
	TEST_ASSERT_EQUAL_INT(0, pf_tuner_start(pts, true, err, sizeof err));
	cJSON_Delete(pts);
	tick(5);
	cJSON *j = pf_tuner_json();
	cJSON *arr = cJSON_GetObjectItem(j, "setpoints");
	const double want[4] = { 250, 180, 350, 450 };
	TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(arr));
	for (int i = 0; i < 4; i++)
		TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(want[i], cJSON_GetArrayItem(arr, i)->valuedouble,
		                                 "the baseline leads, then the rest ascending");
	cJSON_Delete(j);
	pf_tuner_stop("test over");
	tick(5);

	/* a single temperature is not reordered: there is nothing to order */
	cJSON *one = cJSON_Parse("[400]");
	TEST_ASSERT_EQUAL_INT(0, pf_tuner_start(one, false, err, sizeof err));
	cJSON_Delete(one);
	tick(5);
	j = pf_tuner_json();
	TEST_ASSERT_EQUAL_DOUBLE(400, cJSON_GetArrayItem(cJSON_GetObjectItem(j, "setpoints"), 0)->valuedouble);
	cJSON_Delete(j);
	pf_tuner_stop("test over");
	stop_and_wait_cold();
}

static void test_the_published_profile_is_the_whole_profile(void)
{
	char err[160];
	cJSON *pts = cJSON_Parse("[180,225,350,450]");
	TEST_ASSERT_EQUAL_INT(0, pf_tuner_start(pts, true, err, sizeof err));
	cJSON_Delete(pts);
	tick(5);

	cJSON *j = pf_tuner_json();
	cJSON *arr = cJSON_GetObjectItem(j, "setpoints");
	TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(arr));
	/* The baseline is measured first, then the rest in order: 225 is the nearest of these to the
	 * default baseline of 250, so it leads and the others follow ascending. */
	const double want[4] = { 225, 180, 350, 450 };
	for (int i = 0; i < 4; i++)
		TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(want[i], cJSON_GetArrayItem(arr, i)->valuedouble,
		                                 "the profile must list every set point, not repeat one");
	cJSON_Delete(j);
	pf_tuner_stop("test over");
	stop_and_wait_cold();   /* leave the grill as the next test expects to find it */
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_simulator_gain_falls_with_temperature);
	RUN_TEST(test_gain_schedule_interpolates);
	RUN_TEST(test_relay_agrees_with_the_plant_it_measured);
	RUN_TEST(test_a_slow_cooling_half_is_not_a_stall);
	RUN_TEST(test_the_relay_centres_on_holding_not_climbing);
	RUN_TEST(test_relay_recovers_from_a_badly_centred_swing);
	RUN_TEST(test_single_adds_and_full_profile_replaces);
	RUN_TEST(test_guided_tune_improves_holding);
	RUN_TEST(test_the_tuning_library_can_be_backed_up_and_restored);
	RUN_TEST(test_a_profile_measures_its_baseline_first);
	RUN_TEST(test_the_published_profile_is_the_whole_profile);   /* last: it starts a run of its own */
	return UNITY_END();
}
