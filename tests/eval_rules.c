/* Which tuning rule to design from a relay measurement, decided on evidence.
 *
 * The simulator is set to this grill's own plant -- a pit time constant of 1470 s and a dead time of
 * 89 s, six times slower than the default -- then tuned once by the relay, and then held at 250 F
 * under each rule in turn through the two things a cook actually does to it: arrive from cold, and
 * open the lid for three minutes. Not a ctest: it takes a few minutes and prints a table. Run it
 * after touching anything in the tuner, and change pf_tuning_rule_selected only on what it says. */
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
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pf_control ctrl;
static double now;
static void tick(double dt)
{
	int steps = (int)(dt / 0.1 + 0.5);
	for (int i = 0; i < steps; i++) { now += 0.1; pf_sim_step(0.1); pf_probes_poll(now); pf_control_step(&ctrl, now); }
}
static double pit_f(void) { return pf_from_c(ctrl.pit_c, PF_UNITS_F); }
static double err_f(void) { return pf_from_c(ctrl.pit_c, PF_UNITS_F) - 250.0; }

static void cook_to_250(void)
{
	if (ctrl.cinst) pf_control_shutdown(&ctrl);
	pf_control_init(&ctrl, true);
	pf_sim_reset(18.0);
	pf_sim_set_plant(1471, 89);
	tick(5);
	pf_cmd_mode(PF_MODE_HOLD, 250);
	tick(2);
	pf_cmd_simple(PF_CMD_TUNING_APPLY);   /* the library's tune, under the rule now selected */
	tick(1);
	double t = 0;
	while (pit_f() < 245 && t < 4 * 3600) { tick(10); t += 10; }
}

typedef struct { double over_f, iae_hold, rms_hold, dip_f, recover_s, iae_lid; } score_t;

static score_t scenario(void)
{
	score_t sc = { 0 };
	cook_to_250();
	/* arrival: the peak over the next 40 minutes, and the integrated error over the hour */
	double t = 0, iae = 0, sq = 0; int n = 0;
	while (t < 3600) {
		tick(10); t += 10;
		double e = err_f();
		if (t <= 2400 && e > sc.over_f) sc.over_f = e;
		if (t > 1200) { iae += fabs(e) * 10; sq += e * e; n++; }
	}
	sc.iae_hold = iae / 60.0;              /* F-minutes over the last 40 minutes */
	sc.rms_hold = n ? sqrt(sq / n) : 0;
	/* the lid: three minutes open, then how far it fell and how long until it is back within 3 F */
	pf_sim_model()->lid_open = true;
	tick(180);
	pf_sim_model()->lid_open = false;
	double t0 = now, dip = 0, back = -1; iae = 0;
	while (now - t0 < 2400) {
		tick(10);
		double e = err_f();
		if (e < dip) dip = e;
		if (back < 0 && fabs(e) <= 3.0 && now - t0 > 60) back = now - t0;
		iae += fabs(e) * 10;
	}
	sc.dip_f = dip; sc.recover_s = back; sc.iae_lid = iae / 60.0;
	return sc;
}

int main(void)
{
	pf_log_init(PF_LOG_ERROR);
	char cfg[256], db[256];
	snprintf(cfg, sizeof cfg, "/tmp/pf_eval_%d.json", (int)getpid());
	snprintf(db, sizeof db, "/tmp/pf_eval_%d.db", (int)getpid());
	pf_settings_init(cfg);
	pf_settings_force_sim();
	pf_settings_patch("startup", "{\"smartstart\":{\"enabled\":false},\"startup_exit_temp\":0,\"start_to_mode\":{\"after_startup_mode\":\"Hold\",\"primary_setpoint\":250}}", NULL, 0);
	pf_settings_patch("controller", "{\"selected\":\"adaptive\"}", NULL, 0);
	pf_db_open(db);
	pf_controllers_init(NULL);
	pf_probe_drivers_init(NULL);
	pf_events_init();
	pf_learning_init();
	pf_env env; pf_env_init(&env, "platform");
	pf_outputs_init(pf_platform_sim(), pf_platform_sim()->create("{}", &env));
	pf_cmdq_init(); pf_history_init(); pf_probes_init();
	now = 1000;

	/* one relay measurement of this plant, the way the profile takes it */
	pf_tuning_rule_selected = PF_RULE_TYREUS_LUYBEN;
	pf_learning_clear_anchors();
	cook_to_250();
	tick(20 * 60);                                     /* settled: the relay will not start otherwise */
	pf_cmd_simple(PF_CMD_AUTOTUNE_START);
	tick(2);
	if (!ctrl.autotune.active) { printf("relay did not start\n"); return 1; }
	double t = 0;
	while (ctrl.autotune.active && t < 2 * 3600) { tick(10); t += 10; }
	pf_autotune_result r = pf_learning_autotune();
	printf("relay on the slow plant: Ku %.4f  Pu %.0f s  swing %.1f C  load %.3f  (%.0f min)\n", r.Ku, r.Pu, r.amplitude_c, r.load, t / 60);
	pf_learning_store_anchor(pf_f_to_c(250), &r, 18.0, 0);
	/* the plant the daemon would hold after a startup rise on this grill: K about 320 C per unit
	 * duty at this load, tau and theta as set. Cohen-Coon designs from these. */
	pf_learning_store_fopdt(320, 1471, 89);
	/* and from here nothing learns between scenarios, so each rule is judged on the same grill */
	pf_settings_patch("learning", "{\"enabled\":false}", NULL, 0);
	pf_fopdt m = pf_learning_fopdt();
	printf("plant the daemon holds: K %.0f tau %.0f theta %.0f (%s)\n\n", m.K, m.tau, m.theta, m.valid ? "valid" : "none");

	static const char *NAME[] = { "Tyreus-Luyben", "TL, Ti = Pu", "ZN some-overshoot", "Cohen-Coon" };
	printf("%-18s %7s %7s %6s | %8s %9s %8s | %8s %9s %8s\n", "rule", "PB F", "Ti s", "Td s", "arrive+F", "IAE Fmin", "rms F", "lid dip", "recover s", "IAE Fmin");
	for (int rule = 0; rule < 4; rule++) {
		pf_tuning_rule_selected = rule;
		double PB, Ti, Td;
		pf_tuning_from_relay_plant(r.Ku, r.Pu, m.valid ? m.K : 0, m.valid ? m.tau : 0, m.valid ? m.theta : 0, &PB, &Ti, &Td);
		score_t sc = scenario();
		printf("%-18s %7.0f %7.0f %6.0f | %8.1f %9.0f %8.2f | %8.1f %9.0f %8.0f\n", NAME[rule],
		       pf_delta_from_c(PB, PF_UNITS_F), Ti, Td, sc.over_f, sc.iae_hold, sc.rms_hold, sc.dip_f, sc.recover_s, sc.iae_lid);
	}

	/* The case the rule is really for: the feed-forward is wrong. A colder day, a different
	 * pellet, a set point the library has never measured -- the integral is what takes the
	 * offset back, and the rules differ five-fold in how long that takes. The library's load is
	 * pushed a quarter high here, which is about what the built-in prior was wrong by. */
	printf("\nwith the feed-forward a quarter too high:\n");
	pf_tune_anchor all[PF_TUNE_ANCHORS];
	int na = pf_learning_anchor_list(all, PF_TUNE_ANCHORS);
	for (int i = 0; i < na; i++) if (fabs(all[i].setpoint_c - pf_f_to_c(250)) < 5) { all[i].load *= 1.25; pf_learning_put_anchor(&all[i]); }
	printf("%-18s %7s %7s %6s | %8s %9s %8s | %8s %9s %8s\n", "rule", "PB F", "Ti s", "Td s", "arrive+F", "IAE Fmin", "rms F", "lid dip", "recover s", "IAE Fmin");
	for (int rule = 0; rule < 4; rule++) {
		pf_tuning_rule_selected = rule;
		double PB, Ti, Td;
		pf_tuning_from_relay_plant(r.Ku, r.Pu, m.valid ? m.K : 0, m.valid ? m.tau : 0, m.valid ? m.theta : 0, &PB, &Ti, &Td);
		score_t sc = scenario();
		printf("%-18s %7.0f %7.0f %6.0f | %8.1f %9.0f %8.2f | %8.1f %9.0f %8.0f\n", NAME[rule],
		       pf_delta_from_c(PB, PF_UNITS_F), Ti, Td, sc.over_f, sc.iae_hold, sc.rms_hold, sc.dip_f, sc.recover_s, sc.iae_lid);
	}
	pf_control_shutdown(&ctrl);
	unlink(cfg); unlink(db);
	return 0;
}
