/* Learning across cooks: observations, feed-forward fit across ambients, repeat-cook improvement,
 * relay autotune on the simulated grill. */
#include "core/cmdq.h"
#include "core/control.h"
#include "core/db.h"
#include "core/env.h"
#include "core/events.h"
#include "core/history.h"
#include "core/log.h"
#include "core/outputs.h"
#include "core/settings.h"
#include "controllers/registry.h"
#include "features/learning.h"
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

static void tick(double dt)
{
	int steps = (int)(dt / 0.1 + 0.5);
	for (int i = 0; i < steps; i++) { now += 0.1; pf_sim_step(0.1); pf_probes_poll(now); pf_control_step(&ctrl, now); }
}

void setUp(void)
{
	snprintf(cfg_path, sizeof cfg_path, "/tmp/pf_learn_%d.json", (int)getpid());
	snprintf(db_path, sizeof db_path, "/tmp/pf_learn_%d.db", (int)getpid());
	unlink(cfg_path); unlink(db_path);
	pf_settings_init(cfg_path);
	pf_settings_force_sim();
	pf_settings_patch("startup", "{\"smartstart\":{\"enabled\":false},\"startup_exit_temp\":0,\"start_to_mode\":{\"after_startup_mode\":\"Smoke\",\"primary_setpoint\":165}}", NULL, 0);
	pf_db_open(db_path);
	pf_controllers_init(NULL);
	pf_probe_drivers_init(NULL);
	pf_events_init();
	pf_learning_init();
	pf_env env; pf_env_init(&env, "platform");
	pf_outputs_init(pf_platform_sim(), pf_platform_sim()->create("{}", &env));
	pf_cmdq_init(); pf_history_init(); pf_probes_init();
	now = 1000;
}

void tearDown(void)
{
	pf_control_shutdown(&ctrl);
	pf_probes_shutdown();
	pf_outputs_shutdown();
	pf_db_close();
	pf_settings_shutdown();
	unlink(cfg_path); unlink(db_path);
	char p[300]; snprintf(p, sizeof p, "%s-wal", db_path); unlink(p); snprintf(p, sizeof p, "%s-shm", db_path); unlink(p);
}

/* one cook: returns integrated |error| (C*s) over the first `score_min` minutes of HOLD */
static double cook(const char *controller, double ambient_c, double minutes, double score_min)
{
	char patch[96];
	snprintf(patch, sizeof patch, "{\"selected\":\"%s\"}", controller);
	pf_settings_patch("controller", patch, NULL, 0);
	if (ctrl.cinst) pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	pf_sim_reset(ambient_c);
	tick(5);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	while (ctrl.mode != PF_MODE_HOLD) tick(1);
	double sp = pf_f_to_c(225), iae = 0;
	for (double t = 0; t < minutes * 60; t += 5) {
		tick(5);
		if (t < score_min * 60) iae += fabs(ctrl.pit_c - sp) * 5;
	}
	printf("  cook end: pit %.1f C u=%.2f tuning [%s]\n", ctrl.pit_c, ctrl.u_applied, ctrl.dbg.note);
	pf_cmd_simple(PF_CMD_STOP);
	tick(2);
	return iae;
}

/* How long the climb will take, asked the moment the set point changes -- before there is any climb
   to fit a line through, which is why it comes from what the grill has learned rather than from the
   last few minutes. The feed-forward fit read backwards says where full feed is heading; the plant
   says how fast it gets there and how long before it starts. Both improve with every cook, which is
   what makes the answer improve. Before either exists it must say nothing rather than guess. */
static void test_it_can_say_how_long_the_climb_will_take(void)
{
	pf_learning_reset();
	/* nothing learned yet: no answer */
	TEST_ASSERT_TRUE_MESSAGE(pf_learning_time_to(20, 120, 20, 0.9) < 0, "it must not guess before it knows");

	/* a plant, and enough observations to know what duty holds what */
	pf_learning_store_fopdt(400, 900, 60);
    for (int i = 0; i < 12; i++) {
		double sp = 80 + (i % 4) * 25;              /* 80..155 C */
		pf_learning_observe("adaptive", sp, 20, 0.05 + (sp - 20) * 0.0030, 0.5, "");
	}
	pf_ff_fit f = pf_learning_fit();
	TEST_ASSERT_TRUE_MESSAGE(f.n >= 3 && f.b > 0, "the feed-forward fit should have settled");

	double t = pf_learning_time_to(20, 120, 20, 0.9);
	printf("climb 20 -> 120 C: %.0f s (a=%.3f b=%.4f, tau 900, theta 60)\n", t, f.a, f.b);
	TEST_ASSERT_TRUE_MESSAGE(t > 0, "with a plant and a fit it can answer");
	/* the shape has to be right: further is longer, and the same climb from warmer is shorter */
	TEST_ASSERT_TRUE_MESSAGE(pf_learning_time_to(20, 140, 20, 0.9) > t, "further takes longer");
	TEST_ASSERT_TRUE_MESSAGE(pf_learning_time_to(90, 120, 20, 0.9) < t, "from warmer takes less");
	/* never longer than the dead time plus a few time constants, and never less than the dead time */
	TEST_ASSERT_TRUE_MESSAGE(t > 60, "it cannot beat the dead time");
	TEST_ASSERT_TRUE_MESSAGE(t < 60 + 5 * 900, "nor take forever");
	/* asking about a temperature the grill cannot reach has no answer */
	TEST_ASSERT_TRUE_MESSAGE(pf_learning_time_to(20, 400, 20, 0.9) < 0, "it cannot reach 400 C, so it says nothing");
	TEST_ASSERT_TRUE_MESSAGE(pf_learning_time_to(120, 100, 20, 0.9) < 0, "cooling is not a climb it controls");
}

/* An entry filed by an older build holds the relay's own Ku and Pu, and a time constant from the
   capture. That is everything needed to work the plant out, so it can be put right where it stands
   rather than asking for another hour-long run to be started. */
static void test_an_old_entry_puts_its_own_plant_right(void)
{
	pf_learning_clear_anchors();
	/* exactly what this grill had on disk: the relay's numbers, with a plant blended from a capture */
	pf_tune_anchor a = { .setpoint_c = pf_f_to_c(250), .Ku = 0.0667, .Pu = 405,
	                     .PB_c = 33, .Ti = 890, .Td = 64,
	                     .K = 424, .tau = 1620, .theta = 78,        /* 763 F/duty, blended */
	                     .runs = 2, .plant_src = PF_PLANT_FROM_CAPTURE, .valid = true };
	pf_learning_put_anchor(&a);
	pf_learning_init();                       /* a restart, which is when it heals */

	double K = 0, tau = 0, theta = 0;
	TEST_ASSERT_TRUE(pf_learning_plant(pf_f_to_c(250), &K, &tau, &theta));
	double wu = 2 * M_PI / 405.0;
	double theta_relay = (M_PI - atan(wu * 1620)) / wu;
	printf("healed on load: theta %.0f (was 78, relay says %.0f), K %.0f F/duty\n", theta, theta_relay, K * 1.8);
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(2.0, theta_relay, theta, "it should recompute from the relay it already holds");
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1.0, 1620.0, tau, "the time constant is the capture's and stays");
}

/* Two measurements of the same grill average; two measurements of different quality do not.
 *
 * The relay locates the critical point exactly. A capture is a fit to whatever the cook happened to
 * do, and it trades dead time against time constant freely -- on the real grill it returned 15 s
 * where the relay beside it said 104. Averaging those gave 78, and since the prediction scales as
 * K*theta/tau that left the loop predicting three quarters of what had actually been measured: the
 * first tune after the relay fix still overshot 11 F where the simulator manages five. */
static void test_a_relay_plant_is_not_diluted_by_a_capture(void)
{
	pf_learning_clear_anchors();
	/* an anchor whose plant came from a capture, with two runs behind its gains */
	pf_learning_store_anchor_plant(pf_f_to_c(250), 466, 1380, 15);
	pf_autotune_result r = { .Ku = 0.0667, .Pu = 405, .PB_c = 33, .Ti = 890, .Td = 64, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(250), &r, 20, 0);
	pf_learning_store_anchor(pf_f_to_c(250), &r, 20, 0);   /* runs = 2, so a blend would be 50/50 */

	double K = 0, tau = 0, theta = 0;
	TEST_ASSERT_TRUE(pf_learning_plant(pf_f_to_c(250), &K, &tau, &theta));
	/* what the relay itself says, from Ku, Pu and the capture's time constant */
	double wu = 2 * M_PI / r.Pu;
	double theta_relay = (M_PI - atan(wu * 1380)) / wu;
	double K_relay = sqrt(1 + wu * 1380 * wu * 1380) / r.Ku;
	printf("relay says theta %.0f K %.0f; anchor holds theta %.0f K %.0f\n", theta_relay, K_relay, theta, K);
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(2.0, theta_relay, theta, "the relay's dead time, not an average with the capture's guess");
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(15.0, K_relay, K, "and the relay's gain");

	/* and a later capture must not pull it back */
	pf_learning_store_anchor_plant(pf_f_to_c(250), 466, 1380, 15);
	double th2 = 0;
	TEST_ASSERT_TRUE(pf_learning_plant(pf_f_to_c(250), NULL, NULL, &th2));
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(2.0, theta_relay, th2, "a passive fit must not dilute a designed measurement");
}

/* Where an anchor's plant comes from.
 *
 * A relay test locates one point of the grill's frequency response exactly -- the frequency where
 * the phase reaches -pi, and the gain there -- which fixes the dead time and the static gain, given
 * a time constant. The capture that led into the set point measures the time constant, which is the
 * one thing a relay never sees. Taking the whole plant from the capture instead put theta = 15 s on
 * a real grill where the relay beside it said 97 s, and the Smith prediction scales as K*theta/tau,
 * so the loop was predicting about a fifth of the heat that was really on its way.
 *
 * The check is that the relay result and the plant filed with it describe the same grill: run the
 * stored plant through the FOPDT phase condition and the ultimate gain and period must come back. */
static void test_an_anchor_takes_its_plant_from_the_relay(void)
{
	pf_learning_clear_anchors();
	/* a time constant from the capture, as the step fit would have filed it */
	pf_learning_store_anchor_plant(pf_f_to_c(250), 466, 1380, 15);

	pf_autotune_result r = { .Ku = 0.0685, .Pu = 377, .PB_c = 32, .Ti = 829, .Td = 60, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(250), &r, 20, 0);

	double K = 0, tau = 0, theta = 0;
	TEST_ASSERT_TRUE(pf_learning_plant(pf_f_to_c(250), &K, &tau, &theta));
	printf("relay Ku %.4f Pu %.0f + capture tau %.0f -> K %.0f C/duty, theta %.0f s\n", r.Ku, r.Pu, tau, K, theta);

	/* the time constant is the capture's, untouched */
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1.0, 1380.0, tau, "the time constant comes from the capture");
	/* and theta is nothing like the 15 s the capture claimed */
	TEST_ASSERT_TRUE_MESSAGE(theta > 60.0, "the relay's dead time, not the capture's guess");

	/* round trip: what ultimate point does this plant have? */
	double lo = 1e-5, hi = 1.0;
	for (int i = 0; i < 200; i++) {
		double wm = 0.5 * (lo + hi);
		if (-wm * theta - atan(wm * tau) > -M_PI) lo = wm; else hi = wm;
	}
	double wu = 0.5 * (lo + hi);
	double Ku_back = sqrt(1 + wu * tau * wu * tau) / K, Pu_back = 2 * M_PI / wu;
	printf("  back out of the stored plant: Ku %.4f, Pu %.0f s\n", Ku_back, Pu_back);
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.002, r.Ku, Ku_back, "the stored plant must reproduce the measured gain");
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(10.0, r.Pu, Pu_back, "the stored plant must reproduce the measured period");

	/* theta always lands between Pu/4 and Pu/2, whatever the capture said the time constant was */
	for (double t_guess = 200; t_guess <= 3000; t_guess += 400) {
		pf_learning_clear_anchors();
		pf_learning_store_anchor_plant(pf_f_to_c(250), 466, t_guess, 15);
		pf_learning_store_anchor(pf_f_to_c(250), &r, 20, 0);
		double th = 0;
		TEST_ASSERT_TRUE(pf_learning_plant(pf_f_to_c(250), NULL, NULL, &th));
		TEST_ASSERT_TRUE_MESSAGE(th > r.Pu / 4 - 1 && th < r.Pu / 2 + 1,
		                         "the relay brackets the dead time however wrong the time constant is");
	}
}

/* The plant stored on a grill that has been running a while was fitted by whatever method that
 * version shipped, and the one before this used the 28 %/63 % two-point method -- which took the
 * set point as the step's final value and came back with a time constant roughly half the truth.
 * Averaging a proper fit with that leaves the old error in the model for cooks afterwards, and the
 * prediction is built on the model. A fit from a newer method replaces; same method still
 * averages, so one odd capture still cannot run away with it. */
static void test_a_plant_from_the_old_fit_is_replaced_not_averaged(void)
{
	pf_db_kv_put("learning", "fopdt", "{\"K\":229.5,\"tau\":533.5,\"theta\":87.5,\"ts\":1,\"m\":1}");
	pf_learning_init();
	pf_fopdt had = pf_learning_fopdt();
	TEST_ASSERT_TRUE(had.valid);
	TEST_ASSERT_DOUBLE_WITHIN(1.0, 533.5, had.tau);

	pf_learning_store_fopdt(400, 1000, 75);
	pf_fopdt now_p = pf_learning_fopdt();
	printf("old-method plant tau %.0f, then a proper fit of 1000 -> %.0f\n", had.tau, now_p.tau);
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1.0, 1000.0, now_p.tau, "a fit by the new method must replace one by the old, not average with it");
	TEST_ASSERT_DOUBLE_WITHIN(1.0, 400.0, now_p.K);

	pf_learning_store_fopdt(420, 900, 85);
	now_p = pf_learning_fopdt();
	printf("a second fit of 900 by the same method -> %.0f\n", now_p.tau);
	TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1.0, 950.0, now_p.tau, "two fits by the same method should still average");
}

/* And while the old one is all there is, it must not drive the loop either. The prediction is built
 * on the model, and a model whose error is known and one-directional is worse than the controller's
 * own prior -- it is still on the learning page, it just does not get a say. */
static void test_a_plant_from_the_old_fit_does_not_drive_the_loop(void)
{
	pf_db_kv_put("learning", "fopdt", "{\"K\":229.5,\"tau\":533.5,\"theta\":87.5,\"ts\":1,\"m\":1}");
	pf_learning_init();
	double K = 0, tau = 0, theta = 0;
	TEST_ASSERT_FALSE_MESSAGE(pf_learning_plant(pf_f_to_c(250), &K, &tau, &theta),
	                          "a model left by the superseded fit must not be handed to the controller");
	TEST_ASSERT_TRUE_MESSAGE(pf_learning_fopdt().valid, "it is still on record, and still shown");

	pf_learning_store_fopdt(400, 1000, 75);
	TEST_ASSERT_TRUE_MESSAGE(pf_learning_plant(pf_f_to_c(250), &K, &tau, &theta),
	                         "a proper fit is handed over");
	TEST_ASSERT_DOUBLE_WITHIN(1.0, 1000.0, tau);
}

static void test_observations_and_fit_across_ambients(void)
{
	/* long enough cooks that the pit settles and the steady windows the fit needs can be logged */
	cook("adaptive", -1.0, 100, 30);          /* 30 F day */
	pf_ff_fit f1 = pf_learning_fit();
	printf("after cold cook: n=%d a=%.3f b=%.4f\n", f1.n, f1.a, f1.b);
	TEST_ASSERT_TRUE(f1.n >= 3);
	cook("adaptive", 38.0, 100, 30);          /* 100 F day */
	pf_ff_fit f2 = pf_learning_fit();
	printf("after hot cook:  n=%d a=%.3f b=%.4f rms=%.3f\n", f2.n, f2.a, f2.b, f2.rms);
	TEST_ASSERT_TRUE(f2.n > f1.n);
	TEST_ASSERT_TRUE(f2.b > 0);
	int n;
	double cold = pf_learning_uff(pf_f_to_c(225), -1.0, 0.1, 0.9, &n), hot = pf_learning_uff(pf_f_to_c(225), 38.0, 0.1, 0.9, &n);
	printf("u_ff cold %.3f hot %.3f\n", cold, hot);
	TEST_ASSERT_TRUE(cold > hot);
	TEST_ASSERT_TRUE(cold < 0.9 && hot > 0.05);
}

static void test_repeat_cook_not_worse(void)
{
	double first = cook("adaptive", 10.0, 40, 30);
	double second = cook("adaptive", 10.0, 40, 30);
	double third = cook("adaptive", 10.0, 40, 30);
	printf("IAE first %.0f second %.0f third %.0f\n", first, second, third);
	TEST_ASSERT_TRUE(third <= first * 1.10);
	pf_fopdt p = pf_learning_fopdt();
	printf("plant: valid=%d K=%.1f tau=%.0f theta=%.0f\n", p.valid, p.K, p.tau, p.theta);
	TEST_ASSERT_TRUE(p.valid);
	TEST_ASSERT_TRUE(p.tau > 30 && p.tau < 3600);
}

static void test_autotune(void)
{
	pf_settings_patch("controller", "{\"selected\":\"pid\"}", NULL, 0);
	pf_control_init(&ctrl, true);
	pf_sim_reset(18.0);
	tick(5);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 40 * 60);
	TEST_ASSERT_TRUE(ctrl.target_reached);
	pf_cmd_simple(PF_CMD_AUTOTUNE_START);
	tick(1);
	TEST_ASSERT_TRUE(ctrl.autotune.active);
	double t = 0;
	while (ctrl.autotune.active && t < 60 * 60) { tick(10); t += 10; }
	pf_autotune_result r = pf_learning_autotune();
	printf("autotune: valid=%d Ku=%.3f Pu=%.0f amp=%.1f C -> PB %.1f C Ti %.0f Td %.0f (took %.0f min)\n", r.valid, r.Ku, r.Pu, r.amplitude_c, r.PB_c, r.Ti, r.Td, t / 60);
	TEST_ASSERT_FALSE(ctrl.autotune.active);
	TEST_ASSERT_TRUE(r.valid);
	/* The plant is known here, so the period is a number, not a range.
	 *
	 * The limit cycle sits where the loop's phase lag reaches 180 degrees. Three things set that:
	 * the plant itself (w*theta + atan(w*tau) + atan(w*pot)); the auger, which only changes what
	 * it is doing at a cycle boundary and so behaves as a further half-cycle of dead time; and the
	 * relay's own hysteresis, which switches asin(eps/A) short of the crossing and so identifies a
	 * slightly lower frequency. The first two are the loop the PID will drive through and belong in
	 * the answer; the third is the test's artefact and is small when eps is small against the
	 * swing. A relay that found some other period would be measuring its own transient. */
	double tau, theta, pot;
	pf_sim_plant(&tau, &theta, &pot);
	double eps = ctrl.autotune.hyst_c, A = r.amplitude_c;
	double theta_loop = theta + 0.5 * ctrl.ccfg.cycle_s;
	double phase_at = M_PI;   /* the relay corrects its own hysteresis, so the crossing itself */
	double w_plant = 0.001, w_loop = 0.001;
	while (w_plant * theta + atan(w_plant * tau) + atan(w_plant * pot) < M_PI) w_plant *= 1.001;
	while (w_loop * theta_loop + atan(w_loop * tau) + atan(w_loop * pot) < phase_at) w_loop *= 1.001;
	double Pu_plant = 2 * M_PI / w_plant, Pu_expected = 2 * M_PI / w_loop;
	printf("autotune: bare plant Pu %.0f s; through a %.0f s auger cycle, %.0f s; relay found %.0f s (hysteresis %.2f C on a %.2f C swing)\n",
	       Pu_plant, ctrl.ccfg.cycle_s, Pu_expected, r.Pu, eps, A);
	TEST_ASSERT_DOUBLE_WITHIN(0.20 * Pu_expected, Pu_expected, r.Pu);
	/* and the gain: the plant's own ultimate gain at that frequency, which for the simulator's
	 * near-linear middle is 1/|G(jw)| with K the convective rise per unit duty */
	double K = pf_sim_small_signal_gain(18.0, ctrl.autotune.u_center);
	double Ku_expected = sqrt(1 + w_loop * tau * w_loop * tau) * sqrt(1 + w_loop * pot * w_loop * pot) / K;
	printf("autotune: plant K %.0f C/duty at %.3f duty -> Ku about %.4f, relay found %.4f\n", K, ctrl.autotune.u_center, Ku_expected, r.Ku);
	TEST_ASSERT_DOUBLE_WITHIN(0.35 * Ku_expected, Ku_expected, r.Ku);
	TEST_ASSERT_TRUE(r.PB_c > 0 && r.Ti > 0);
	TEST_ASSERT_EQUAL(PF_MODE_HOLD, ctrl.mode);
	/* apply and keep holding */
	pf_cmd_simple(PF_CMD_TUNING_APPLY);
	tick(20 * 60);
	TEST_ASSERT_DOUBLE_WITHIN(10.0, pf_f_to_c(225), ctrl.pit_c);
}

/* ---------------- the tuning library as a model of the whole range ---------------- */

/* A pellet grill loses more heat the hotter it runs, so the loop it presents at 250 F is genuinely
 * a different loop from the one at 350 F: a good tune at one is not a good tune at the other. The
 * library exists so both can be true at once. The property that makes that worth having is that
 * measuring the grill somewhere new never disturbs somewhere already measured -- otherwise every
 * extra run would trade one good answer for two mediocre ones. */
static void test_a_tune_at_one_set_point_leaves_the_others_alone(void)
{
	pf_learning_clear_anchors();
	pf_autotune_result at250 = { .Ku = 0.070, .Pu = 400, .PB_c = 31.4, .Ti = 880, .Td = 63.5, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(250), &at250, 10, 0);

	pf_autotune_result at350 = { .Ku = 0.030, .Pu = 600, .PB_c = 73.3, .Ti = 1320, .Td = 95.2, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(350), &at350, 10, 0);
	pf_autotune_result at225 = { .Ku = 0.085, .Pu = 360, .PB_c = 25.9, .Ti = 792, .Td = 57.1, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(225), &at225, 10, 0);
	pf_autotune_result at180 = { .Ku = 0.100, .Pu = 300, .PB_c = 22.0, .Ti = 660, .Td = 47.6, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(180), &at180, 10, 0);

	double PB = 0, Ti = 0, Td = 0;
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(250), &PB, &Ti, &Td));
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 31.4, PB);
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 880.0, Ti);
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 63.5, Td);

	/* all four stand on their own evidence, each still a single run */
	pf_tune_anchor list[PF_TUNE_ANCHORS];
	int n = pf_learning_anchor_list(list, PF_TUNE_ANCHORS);
	TEST_ASSERT_EQUAL_INT(4, n);
	for (int i = 0; i < n; i++) TEST_ASSERT_EQUAL_INT(1, list[i].runs);
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 22.0, list[0].PB_c);     /* 180 */
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 73.3, list[3].PB_c);     /* 350 */
}

/* Tuning the same set point again is the one case where an entry should move, because it is more
 * evidence about the same thing -- and it moves part of the way, not all of it, so one windy
 * afternoon cannot undo a well-measured entry. */
static void test_a_repeat_run_refines_that_set_point_only(void)
{
	pf_learning_clear_anchors();
	pf_autotune_result first = { .Ku = 0.070, .Pu = 400, .PB_c = 30.0, .Ti = 880, .Td = 63.5, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(250), &first, 10, 0);
	pf_autotune_result other = { .Ku = 0.030, .Pu = 600, .PB_c = 70.0, .Ti = 1320, .Td = 95.2, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(350), &other, 10, 0);

	pf_autotune_result again = { .Ku = 0.058, .Pu = 440, .PB_c = 38.0, .Ti = 968, .Td = 69.8, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(250) + 1.0, &again, 10, 0);   /* same set point, next afternoon */

	pf_tune_anchor list[PF_TUNE_ANCHORS];
	int n = pf_learning_anchor_list(list, PF_TUNE_ANCHORS);
	TEST_ASSERT_EQUAL_INT(2, n);
	TEST_ASSERT_EQUAL_INT(2, list[0].runs);
	TEST_ASSERT_DOUBLE_WITHIN(0.01, 34.0, list[0].PB_c);   /* half way, not all the way */
	TEST_ASSERT_EQUAL_INT(1, list[1].runs);
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 70.0, list[1].PB_c);   /* 350 untouched */
}

/* Between the temperatures that were measured, the model is read by interpolation, so a set point
 * nobody has tuned at still gets an answer that belongs to this grill rather than to a default.
 * Outside the measured range it holds flat: extrapolating a straight line past 450 F or below
 * 180 F would invent a tuning nothing supports. */
static void test_an_untuned_set_point_interpolates_between_its_neighbours(void)
{
	pf_learning_clear_anchors();
	pf_autotune_result at225 = { .Ku = 0.085, .Pu = 360, .PB_c = 26.0, .Ti = 800, .Td = 57.0, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(225), &at225, 10, 0);
	pf_autotune_result at250 = { .Ku = 0.070, .Pu = 400, .PB_c = 34.0, .Ti = 900, .Td = 64.0, .valid = true };
	pf_learning_store_anchor(pf_f_to_c(250), &at250, 10, 0);

	/* 235 F sits 40 % of the way from 225 to 250 */
	double PB = 0, Ti = 0, Td = 0;
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(235), &PB, &Ti, &Td));
	TEST_ASSERT_DOUBLE_WITHIN(0.05, 29.2, PB);
	TEST_ASSERT_DOUBLE_WITHIN(0.5, 840.0, Ti);
	TEST_ASSERT_DOUBLE_WITHIN(0.1, 59.8, Td);

	/* and it is monotonic across the range: every step warmer asks for a wider band */
	double prev = 0;
	for (int f = 225; f <= 250; f += 5) {
		TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(f), &PB, NULL, NULL));
		TEST_ASSERT_TRUE(PB > prev);
		prev = PB;
	}
	/* outside the measured range, the nearest measurement stands */
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(180), &PB, NULL, NULL));
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 26.0, PB);
	TEST_ASSERT_TRUE(pf_learning_gains(pf_f_to_c(400), &PB, NULL, NULL));
	TEST_ASSERT_DOUBLE_WITHIN(1e-6, 34.0, PB);
}

/* The library holds eight entries. When a ninth arrives something has to go, and what goes should
 * be the entry the model can most nearly reconstruct without it -- one sitting right next to a
 * neighbour -- never the one at the end of the range, which is the only evidence the grill has up
 * or down there, and never a well-refined entry over a single-run one beside it. */
static void test_a_full_library_gives_up_its_most_redundant_entry(void)
{
	pf_learning_clear_anchors();
	pf_autotune_result r = { .Ku = 0.07, .Pu = 400, .PB_c = 30.0, .Ti = 880, .Td = 63.5, .valid = true };
	/* seven well spread, plus one crowded right up against a neighbour */
	const int fs[8] = { 180, 200, 250, 300, 350, 400, 445, 455 };
	for (int i = 0; i < 8; i++) { r.PB_c = 20.0 + i; pf_learning_store_anchor(pf_f_to_c(fs[i]), &r, 10, 0); }
	r.PB_c = 45.0;
	pf_learning_store_anchor(pf_f_to_c(225), &r, 10, 0);

	pf_tune_anchor list[PF_TUNE_ANCHORS];
	int n = pf_learning_anchor_list(list, PF_TUNE_ANCHORS);
	TEST_ASSERT_EQUAL_INT(8, n);
	int crowded = 0;
	bool has_180 = false, has_225 = false;
	for (int i = 0; i < n; i++) {
		double f = pf_c_to_f(list[i].setpoint_c);
		if (f > 430) crowded++;
		if (fabs(f - 180) < 2) has_180 = true;
		if (fabs(f - 225) < 2) has_225 = true;
	}
	TEST_ASSERT_TRUE_MESSAGE(has_225, "the new measurement must be in the library");
	TEST_ASSERT_TRUE_MESSAGE(has_180, "the bottom of the range must survive");
	TEST_ASSERT_EQUAL_INT_MESSAGE(1, crowded, "one of the two crowded entries is what to give up");
}

/* The same grill, measured twice, must answer the same twice.
 *
 * It did not. Each run tightened the proportional band, the tighter controller then held on a duty
 * that left less room for the next run's swing, the smaller swing produced an oscillation closer to
 * the relay's switching band, and the describing function -- which divides by sqrt(A^2 - eps^2) --
 * handed back an ultimate gain inflated by that. On the real grill it read 150, 56, 67 and then 41
 * degrees of band across four runs while nothing about the grill had changed, and the overshoot on
 * the way to the set point grew with it. */
static void test_measuring_twice_gives_the_same_answer(void)
{
	pf_settings_patch("controller", "{\"selected\":\"pid\"}", NULL, 0);
	pf_control_init(&ctrl, true);
	pf_sim_reset(18.0);
	tick(5);
	pf_cmd_mode(PF_MODE_HOLD, 225);
	tick(250 + 40 * 60);
	TEST_ASSERT_TRUE(ctrl.target_reached);

	double pb[2];
	for (int run = 0; run < 2; run++) {
		pf_cmd_simple(PF_CMD_AUTOTUNE_START);
		tick(1);
		TEST_ASSERT_TRUE(ctrl.autotune.active);
		double t = 0;
		while (ctrl.autotune.active && t < 60 * 60) { tick(10); t += 10; }
		pf_autotune_result r = pf_learning_autotune();
		TEST_ASSERT_TRUE_MESSAGE(r.valid, "the run produced no usable measurement");
		/* the swing has to stand clear of the switching band, or the gain is arithmetic noise */
		TEST_ASSERT_TRUE_MESSAGE(r.amplitude_c > 0, "no amplitude recorded");
		pb[run] = r.PB_c;
		printf("run %d: Ku %.4f Pu %.0f amp %.2f C -> PB %.1f C\n", run + 1, r.Ku, r.Pu, r.amplitude_c, r.PB_c);
		pf_cmd_simple(PF_CMD_TUNING_APPLY);
		tick(20 * 60);            /* hold on the new tuning, then measure again */
	}
	double ratio = pb[1] > pb[0] ? pb[1] / pb[0] : pb[0] / pb[1];
	printf("band moved by %.2fx between runs\n", ratio);
	TEST_ASSERT_TRUE_MESSAGE(ratio < 1.5, "the band moved more than half again between two runs on the same grill");
}

/* The overshoot on the way to a set point, from cold. This is the number the Smith prediction has
   to move: the pot keeps burning after the feed is cut, and the pit sails past the target. */
static void test_capture_overshoot(void)
{
	pf_settings_patch("controller", "{\"selected\":\"adaptive\"}", NULL, 0);
	pf_control_init(&ctrl, true);
	pf_sim_reset(18.0);
	tick(5);
	pf_cmd_mode(PF_MODE_HOLD, 250);
	double peak = -999, sp = pf_f_to_c(250);
	for (int i = 0; i < 120 * 60 / 10; i++) {
		tick(10);
		if (ctrl.pit_c > peak) peak = ctrl.pit_c;
		if (ctrl.mode != PF_MODE_HOLD && ctrl.mode != PF_MODE_STARTUP) break;
	}
	printf("capture 250F: peak %.1f F, overshoot %+.1f F\n",
	       pf_c_to_f(peak), pf_c_to_f(peak) - 250.0);
	/* the whole point: the pit must not sail past the target on the way in */
	TEST_ASSERT_TRUE_MESSAGE(pf_c_to_f(peak) - 250.0 < 8.0, "overshot the set point on capture");
	TEST_ASSERT_TRUE_MESSAGE(peak > sp - 5, "never got there at all");

	/* and it must still HOLD: a prediction that leaves an offset has traded one fault for another */
	tick(40 * 60);
	double err = pf_c_to_f(ctrl.pit_c) - 250.0;
	printf("settled at %.1f F (%+.1f)\n", pf_c_to_f(ctrl.pit_c), err);
	TEST_ASSERT_TRUE_MESSAGE(fabs(err) < 8.0, "did not settle on the set point");

	/* a step up is the same problem again, from a running grill rather than a cold one */
	pf_cmd_mode(PF_MODE_HOLD, 300);
	double peak2 = -999;
	for (int i = 0; i < 90 * 60 / 10; i++) {
		tick(10); if (ctrl.pit_c > peak2) peak2 = ctrl.pit_c;
		if (i % 12 == 0 && i < 120) {
			char st[512] = ""; if (ctrl.cops && ctrl.cops->state_json) ctrl.cops->state_json(ctrl.cinst, st, sizeof st);
			const char *sp2 = strstr(st, "surplus");
			printf("  t+%2dmin pit %5.1f u %.2f  %s\n", i / 6, pf_c_to_f(ctrl.pit_c), ctrl.u_applied, sp2 ? sp2 : "");
		}
	}
	printf("step 250->300F: peak %.1f F, overshoot %+.1f F\n", pf_c_to_f(peak2), pf_c_to_f(peak2) - 300.0);
	TEST_ASSERT_TRUE_MESSAGE(pf_c_to_f(peak2) - 300.0 < 8.0, "overshot on a set-point change");
}

int main(void)
{
	pf_log_init(PF_LOG_ERROR);
	UNITY_BEGIN();
	RUN_TEST(test_an_anchor_takes_its_plant_from_the_relay);
	RUN_TEST(test_a_relay_plant_is_not_diluted_by_a_capture);
	RUN_TEST(test_an_old_entry_puts_its_own_plant_right);
	RUN_TEST(test_it_can_say_how_long_the_climb_will_take);
	RUN_TEST(test_a_plant_from_the_old_fit_is_replaced_not_averaged);
	RUN_TEST(test_a_plant_from_the_old_fit_does_not_drive_the_loop);
	RUN_TEST(test_observations_and_fit_across_ambients);
	RUN_TEST(test_repeat_cook_not_worse);
	RUN_TEST(test_capture_overshoot);
	RUN_TEST(test_autotune);
	RUN_TEST(test_measuring_twice_gives_the_same_answer);
	RUN_TEST(test_a_tune_at_one_set_point_leaves_the_others_alone);
	RUN_TEST(test_a_repeat_run_refines_that_set_point_only);
	RUN_TEST(test_an_untuned_set_point_interpolates_between_its_neighbours);
	RUN_TEST(test_a_full_library_gives_up_its_most_redundant_entry);
	return UNITY_END();
}
