#define _GNU_SOURCE
/* Autotune runs: a full profile across the grill's range, or one temperature on its own.
 *
 * A pellet grill loses more heat the hotter it runs, so the loop it presents at 180 F is not the
 * loop it presents at 450 F. One proportional band cannot suit both. This walks a list of anchor
 * set points, measures the loop at each with the relay test the daemon already has, and stores the
 * result as a gain schedule the controller interpolates across. The run is hands off: it starts the
 * grill, waits for each set point to settle, runs the test, moves on, and shuts down at the end.
 *
 * It drives the grill through the ordinary command queue and watches the ordinary status, so it can
 * do nothing a person could not do from the web app, and a user Stop ends it at any point. */
#include "features/tuner.h"
#include "core/cmdq.h"
#include "core/db.h"
#include "core/events.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "features/learning.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define TAG "tuner"
#define MAX_POINTS 6

/* How long each stage may take before the run gives up on it. Settling from cold at a high set
 * point is the slow one; the relay test bounds itself at seven crossings. */
#define T_START_S   1800.0
#define T_SETTLE_S  3600.0
#define T_TEST_S    5400.0   /* a slow grill needs seven crossings plus room to re-centre */
#define STABLE_S      90.0   /* inside the band this long before the test begins */

typedef enum { PH_IDLE = 0, PH_STARTING, PH_SETTLING, PH_TESTING, PH_NEXT, PH_FINISHING, PH_DONE, PH_FAILED } phase;

static const char *PHASE_NAME[] = { "idle", "starting", "settling", "testing", "next", "finishing", "done", "failed" };

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
	bool running;
	phase ph;
	double points_c[MAX_POINTS];
	int n, step;
	double phase_start, run_start, last_now;
	double stable_since;
	unsigned at_gen;           /* stored-result counter, to tell a fresh measurement from the last */
	int measured;
	int tries;                 /* attempts at the current set point */
	bool full;                 /* a full profile, which replaces the library, or a single set point */
	double amb_c, wind_kmh;    /* conditions this run is being measured in */
	double run_start_wall;     /* wall clock, to tell this run's anchors from older ones */
	double settle_from_c;      /* where the pit was when this set point's settle began */
	char skipped[64];          /* set points that gave nothing usable, for the finishing message */
	char message[120];
} g;

/* The tuning each set point produced. A run takes hours and nobody is standing over it, so the
 * message that says it finished should carry the numbers it found, both because they are the point
 * of the exercise and because they can be typed back in by hand if a later run goes wrong. */
static int fmt_results(char *out, size_t n, double since_wall)
{
	pf_units u = pf_settings_units();
	const char *deg = u == PF_UNITS_C ? "\xC2\xB0" "C" : "\xC2\xB0" "F";
	pf_tune_anchor a[PF_TUNE_ANCHORS];
	int na = pf_learning_anchor_list(a, PF_TUNE_ANCHORS), written = 0;
	size_t o = 0;
	for (int i = 0; i < na && o + 1 < n; i++) {
		if (!a[i].valid || a[i].ts < since_wall - 1) continue;
		int k = snprintf(out + o, n - o, "%s%.0f%s PB %.0f Ti %.0f Td %.0f", written ? " \xC2\xB7 " : "",
		                 pf_from_c(a[i].setpoint_c, u), deg, pf_delta_from_c(a[i].PB_c, u), a[i].Ti, a[i].Td);
		if (k < 0 || (size_t)k >= n - o) break;
		o += (size_t)k;
		written++;
	}
	out[o < n ? o : n - 1] = 0;
	return written;
}

static void set_phase(phase p, double now, const char *msg)
{
	g.ph = p;
	g.phase_start = now;
	g.stable_since = 0;
	g.settle_from_c = NAN;
	if (msg) pf_strlcpy(g.message, msg, sizeof g.message);
}

/* A run lasts hours, so a restart in the middle of one must not pass unnoticed. The flag is
 * written when a run begins and cleared when it ends; finding it set at boot means the daemon went
 * down mid-run. Nothing is resumed: the grill's state after a restart is not the run's to assume. */
static void mark_inflight(bool yes)
{
	if (pf_db_handle()) pf_db_kv_put("tuner", "inflight", yes ? "true" : "false");
}

static void finish(bool ok, const char *why, double now)
{
	bool was = g.running;
	g.running = false;
	set_phase(ok ? PH_DONE : PH_FAILED, now, why);
	if (was) mark_inflight(false);
	if (!was) return;
	char vals[200];
	int nv = fmt_results(vals, sizeof vals, g.run_start_wall);
	if (ok && g.full)
		pf_events_emit("Tune_Done", "Full profile tune finished",
		               "%d of %d set points measured%s%s. This is the grill's new baseline.%s%s",
		               g.measured, g.n, g.skipped[0] ? ", nothing usable at " : "", g.skipped[0] ? g.skipped : "",
		               nv ? " " : "", nv ? vals : "");
	else if (ok)
		pf_events_emit("Tune_Done", "Tuning finished", "Added to the tuning library. %s",
		               nv ? vals : "The run produced no usable measurement.");
	else
		pf_events_emit("Tune_Failed", "Tuning stopped", "%s%s%s", why,
		               nv ? " Measured so far: " : " Nothing was measured before it stopped.", nv ? vals : "");
}

void pf_tuner_init(void)
{
	pthread_mutex_lock(&g_mu);
	memset(&g, 0, sizeof g);
	pf_strlcpy(g.message, "Not running", sizeof g.message);
	pthread_mutex_unlock(&g_mu);

	char buf[16] = "";
	if (pf_db_kv_get("tuner", "inflight", buf, sizeof buf) == 0 && !strcmp(buf, "true")) {
		mark_inflight(false);
		pthread_mutex_lock(&g_mu);
		g.ph = PH_FAILED;
		pf_strlcpy(g.message, "The grill restarted part way through. Start it again when you are ready.", sizeof g.message);
		pthread_mutex_unlock(&g_mu);
		pf_events_emit("Tune_Failed", "Tuning did not finish",
		               "The grill restarted part way through a tuning run. Any set points it had already measured were kept.");
		LOGW(TAG, "a tuning run was interrupted by a restart");
	}
}

int pf_tuner_start(const cJSON *setpoints_json, bool full_profile, char *err, size_t n)
{
	pthread_mutex_lock(&g_mu);
	if (g.running) { snprintf(err, n, "a tuning run is already going"); pthread_mutex_unlock(&g_mu); return -1; }

	/* the set points to visit: what was asked for, else what settings say, else a sensible spread */
	double pts[MAX_POINTS];
	int np = 0;
	const cJSON *arr = setpoints_json;
	cJSON *owned = NULL;
	if (!cJSON_IsArray(arr)) { owned = pf_set_dup("learning.tune_setpoints"); arr = owned; }
	const cJSON *it;
	cJSON_ArrayForEach(it, arr) {
		if (np >= MAX_POINTS || !cJSON_IsNumber(it)) continue;
		pts[np++] = pf_to_c(it->valuedouble, pf_settings_units());
	}
	cJSON_Delete(owned);
	if (np == 0) {
		static const double def_f[] = { 250, 180, 350, 450 };
		for (int i = 0; i < 4; i++) pts[np++] = pf_to_c(def_f[i], PF_UNITS_F);
	}
	/* Ascending, so the run climbs and never waits for the grill to cool -- except for the first
	 * point, which is the baseline and is measured before the rest.
	 *
	 * The bottom of the range is the worst place to start. A grill holds 180 F on very little
	 * fuel, so close to the minimum feed that the relay has almost no room to swing below its
	 * centre: the swing gets clamped on one side, and the describing function behind the result
	 * assumes a symmetric square wave, so a lopsided one reports an ultimate gain that is too
	 * high. Around 250 F there is real room either side, which makes it the most trustworthy
	 * measurement of the four and the right one to set the grill's baseline from. Measuring it
	 * first also means the rest of the run, and a run that is interrupted, already has honest
	 * tuning to work with rather than the untuned defaults someone typed in. */
	for (int i = 1; i < np; i++)
		for (int j = i; j > 0 && pts[j - 1] > pts[j]; j--) { double t = pts[j - 1]; pts[j - 1] = pts[j]; pts[j] = t; }
	if (full_profile && np > 1) {
		double want = pf_to_c(pf_set_num("learning.tune_baseline", 250), pf_settings_units());
		int base = 0;
		for (int i = 1; i < np; i++)
			if (fabs(pts[i] - want) < fabs(pts[base] - want)) base = i;
		if (base > 0) {   /* move it to the front, leaving the others in order */
			double b = pts[base];
			for (int i = base; i > 0; i--) pts[i] = pts[i - 1];
			pts[0] = b;
		}
	}

	memset(&g, 0, sizeof g);
	memcpy(g.points_c, pts, sizeof pts);
	g.n = np;
	g.step = 0;
	g.full = full_profile;
	g.amb_c = NAN;              /* until the first status says otherwise */
	g.running = true;
	g.at_gen = pf_learning_autotune_gen();
	/* Phase timing runs on the control clock, which arrives with the first tick. Taking it from
	 * this thread instead would make the first phase look hours old before it began. */
	set_phase(PH_STARTING, 0, "Starting the grill");
	mark_inflight(true);
	pthread_mutex_unlock(&g_mu);

	/* A full profile is a fresh baseline: the old library described a grill that may since have
	 * been cleaned, re-gasketed or moved, so it is cleared rather than merged into. */
	if (full_profile) pf_learning_clear_anchors();

	pf_cmd c = { .type = PF_CMD_MODE, .mode = PF_MODE_HOLD, .num = pf_from_c(pts[0], pf_settings_units()) };
	pf_cmdq_push(&c);
	if (full_profile)
		pf_events_emit("Tune_Started", "Full profile tune started",
		               "The grill will hold %d set points in turn and oscillate a few degrees at each, then shut down. This replaces the tuning library. Leave it empty; it takes a few hours.", np);
	else
		pf_events_emit("Tune_Started", "Tuning started",
		               "The grill will hold %.0f and oscillate a few degrees around it, then shut down. Leave it empty.",
		               pf_from_c(pts[0], pf_settings_units()));
	LOGW(TAG, "%s tuning started over %d set point%s", full_profile ? "full profile" : "single", np, np == 1 ? "" : "s");
	return 0;
}

void pf_tuner_stop(const char *why)
{
	pthread_mutex_lock(&g_mu);
	bool was = g.running;
	if (was) finish(false, why && *why ? why : "Stopped.", pf_now());
	pthread_mutex_unlock(&g_mu);
	if (was) {
		pf_cmd a = { .type = PF_CMD_AUTOTUNE_STOP };
		pf_cmdq_push(&a);
		pf_cmd c = { .type = PF_CMD_MODE, .mode = PF_MODE_SHUTDOWN };
		pf_cmdq_push(&c);
	}
}

/* ---------------- the run ---------------- */

void pf_tuner_tick(const cJSON *status, double now)
{
	pthread_mutex_lock(&g_mu);
	if (!g.running) { pthread_mutex_unlock(&g_mu); return; }

	/* The first tick sets both clocks: the run is timed on the control loop's clock, not on the
	 * wall clock of whichever thread pressed the button. */
	if (g.phase_start == 0) g.phase_start = now;
	if (g.run_start == 0) { g.run_start = now; g.run_start_wall = pf_wall(); }
	g.last_now = now;

	/* the conditions this measurement is being taken in, kept with the anchor */
	const cJSON *amb = cJSON_GetObjectItem((cJSON *)status, "ambient");
	if (cJSON_IsNumber(amb)) g.amb_c = pf_to_c(amb->valuedouble, pf_settings_units());
	if (pf_json_bool((cJSON *)status, "weather.valid", false)) g.wind_kmh = pf_json_num((cJSON *)status, "weather.wind_kmh", 0);

	const char *mode = pf_json_str((cJSON *)status, "mode", "Stop");
	bool at_active = pf_json_bool((cJSON *)status, "autotune.active", false);
	double sp = pf_json_num((cJSON *)status, "setpoint", 0);
	double elapsed = now - g.phase_start;

	/* the grill going to Error, or anyone pressing Stop, ends the run wherever it is */
	pf_units tu = pf_settings_units();
	double at_sp = g.n > 0 ? pf_from_c(g.points_c[g.step < g.n ? g.step : g.n - 1], tu) : 0;
	const char *tdeg = tu == PF_UNITS_C ? "\xC2\xB0" "C" : "\xC2\xB0" "F";
	char why[180];
	if (!strcmp(mode, "Error")) {
		snprintf(why, sizeof why, "The grill went into Error (%s) while working on %.0f%s.",
		         pf_json_str((cJSON *)status, "safety.error_code", "no code"), at_sp, tdeg);
		finish(false, why, now);
		pthread_mutex_unlock(&g_mu);
		return;
	}
	/* Anyone pressing Stop ends the run. The exceptions are the first seconds, before the start
	 * command has been picked up, and the shutdown at the end, which is the run's own doing. */
	if (!strcmp(mode, "Stop") || !strcmp(mode, "Monitor")) {
		bool starting_up = g.ph == PH_STARTING && elapsed < 30.0;
		if (g.ph != PH_FINISHING && !starting_up) {
			snprintf(why, sizeof why, "The grill was stopped while working on %.0f%s.", at_sp, tdeg);
			finish(false, why, now);
			pthread_mutex_unlock(&g_mu);
			return;
		}
	}

	switch (g.ph) {
	case PH_STARTING:
		/* the mode request routes through Startup on its own; wait for it to land in Hold */
		if (!strcmp(mode, "Hold")) { set_phase(PH_SETTLING, now, "Waiting for the grill to settle"); break; }
		if (elapsed > T_START_S) {
			snprintf(why, sizeof why, "The grill never reached Hold; it was still in %s %.0f minutes after the run asked it to start.",
			         mode, T_START_S / 60);
			finish(false, why, now);
		}
		break;

	case PH_SETTLING: {
		const cJSON *probes = cJSON_GetObjectItem((cJSON *)status, "probes"), *p;
		double pit = NAN;
		cJSON_ArrayForEach(p, probes)
			if (!strcmp(pf_json_str((cJSON *)p, "role", ""), "Primary")) {
				const cJSON *t = cJSON_GetObjectItem((cJSON *)p, "temp");
				if (cJSON_IsNumber(t)) pit = t->valuedouble;
				break;
			}
		/* Near the set point and no longer climbing towards it. The band is deliberately wide: an
		 * untuned grill swings about its target, and waiting for tight holding here would mean
		 * waiting for the very thing this run exists to produce. The pit must also have actually
		 * touched the set point, which is the same condition the relay test itself insists on. */
		double band = pf_settings_units() == PF_UNITS_C ? 8.0 : 15.0;
		bool reached = pf_json_bool((cJSON *)status, "target_reached", false);
		bool close = reached && !isnan(pit) && sp > 0 && fabs(pit - sp) <= band;
		if (!close) g.stable_since = 0;
		else if (g.stable_since == 0) g.stable_since = now;
		if (g.stable_since > 0 && now - g.stable_since >= STABLE_S) {
			set_phase(PH_TESTING, now, "Measuring the loop");
			g.at_gen = pf_learning_autotune_gen();
			pthread_mutex_unlock(&g_mu);
			pf_cmd c = { .type = PF_CMD_AUTOTUNE_START };
			pf_cmdq_push(&c);
			return;
		}
		/* How long this step is allowed depends on how far the grill has to travel and which way.
		 * A flat hour suited a run that climbed in even steps; measuring the baseline first means
		 * one step down and then the longest climb of the run, and neither fits the same figure.
		 * Cooling is passive -- the grill can only stop feeding and wait -- so it is reckoned far
		 * slower than a climb. The allowance is the travel plus the old hour to settle once there. */
		if (isnan(g.settle_from_c) && !isnan(pit)) g.settle_from_c = pit;
		double allow = T_SETTLE_S;
		if (!isnan(g.settle_from_c) && sp > 0) {
			double gap_f = fabs(pf_delta_from_c(g.settle_from_c - sp, PF_UNITS_F));
			double per_min = g.settle_from_c > sp ? 1.5 : 5.0;   /* degrees F a minute */
			allow += gap_f / per_min * 60.0;
		}
		if (elapsed > allow) {
			if (!isnan(pit) && sp > 0)
				snprintf(why, sizeof why, "The grill never settled at %.0f%s: after %.0f minutes it was %.0f%s away and still moving.",
				         at_sp, tdeg, allow / 60, fabs(pf_delta_from_c(pit - sp, tu)), tdeg);
			else
				snprintf(why, sizeof why, "The grill never settled at %.0f%s and the pit probe was not reading.", at_sp, tdeg);
			finish(false, why, now);
		}
		break;
	}

	case PH_TESTING: {
		if (at_active) {
			if (elapsed > T_TEST_S) {
				snprintf(why, sizeof why, "The measurement at %.0f%s ran past %.0f minutes without completing its swings.",
				         at_sp, tdeg, T_TEST_S / 60);
				finish(false, why, now);
			}
			break;
		}
		/* the test is over: a fresh result means it succeeded */
		pf_autotune_result r = pf_learning_autotune();
		if (elapsed < 5) break;                 /* give the command a moment to be picked up */
		unsigned gen = pf_learning_autotune_gen();
		if (gen != g.at_gen && r.PB_c > 0) {
			pf_learning_store_anchor(g.points_c[g.step], &r, g.amb_c, g.wind_kmh);
			g.measured++;
			g.at_gen = gen;
			LOGI(TAG, "set point %.0f C measured: PB %.1f C, Ti %.0f s, Td %.0f s", g.points_c[g.step], r.PB_c, r.Ti, r.Td);
			/* Storing the anchor is what adopts it: the daemon interpolates the library for
			 * whichever set point is being held and hands the result to the controller, which
			 * prefers it over the untuned numbers in its own configuration. So from the moment the
			 * baseline is measured the rest of the run, and any cook after an interrupted run, is
			 * governed by a real measurement of this grill rather than by a typed-in guess --
			 * without writing a single measurement into the saved settings, where a bad one would
			 * outlive the run that produced it. The finishing message reports the numbers instead,
			 * so adopting them permanently stays a decision rather than a side effect. */
			set_phase(PH_NEXT, now, "Moving to the next set point");
		} else if (elapsed < 60 && g.tries < 2) {
			/* The test never got going, which means the grill had drifted off the set point by
			 * the time the command was picked up. Settle again and have one more go. */
			g.tries++;
			LOGW(TAG, "relay test at %.0f C did not start; settling again", g.points_c[g.step]);
			set_phase(PH_SETTLING, now, "Waiting for the grill to settle");
		} else {
			/* one set point failing should not waste the rest of the run */
			LOGW(TAG, "set point %.0f C produced no usable measurement, moving on", g.points_c[g.step]);
			/* Named in the finishing message: a run that quietly measured two of four and called
			 * itself done would leave the gap to be discovered during a cook. */
			snprintf(g.skipped + strlen(g.skipped), sizeof g.skipped - strlen(g.skipped), "%s%.0f%s",
			         g.skipped[0] ? ", " : "", at_sp, tdeg);
			set_phase(PH_NEXT, now, "That set point gave nothing usable; moving on");
		}
		break;
	}

	case PH_NEXT:
		g.step++;
		if (g.step >= g.n) {
			set_phase(PH_FINISHING, now, "Shutting the grill down");
			pthread_mutex_unlock(&g_mu);
			pf_cmd c = { .type = PF_CMD_MODE, .mode = PF_MODE_SHUTDOWN };
			pf_cmdq_push(&c);
			return;
		}
		g.tries = 0;
		set_phase(PH_SETTLING, now, "Waiting for the grill to settle");
		pthread_mutex_unlock(&g_mu);
		{
			pf_cmd c = { .type = PF_CMD_SETPOINT, .num = pf_from_c(g.points_c[g.step], pf_settings_units()) };
			pf_cmdq_push(&c);
		}
		return;

	case PH_FINISHING:
		if (!strcmp(mode, "Stop") || elapsed > 900) finish(g.measured > 0, g.measured > 0 ? "" : "No set point produced a measurement.", now);
		break;

	default:
		break;
	}
	pthread_mutex_unlock(&g_mu);
}

bool pf_tuner_active(double *setpoint_user, int *step, int *steps)
{
	pthread_mutex_lock(&g_mu);
	bool on = g.running;
	if (on) {
		int i = g.step < g.n ? g.step : (g.n > 0 ? g.n - 1 : 0);
		if (setpoint_user) *setpoint_user = pf_from_c(g.points_c[i], pf_settings_units());
		if (step) *step = g.step + 1;
		if (steps) *steps = g.n;
	}
	pthread_mutex_unlock(&g_mu);
	return on;
}

cJSON *pf_tuner_json(void)
{
	cJSON *o = cJSON_CreateObject();
	pf_units u = pf_settings_units();
	pthread_mutex_lock(&g_mu);
	cJSON_AddBoolToObject(o, "running", g.running);
	cJSON_AddStringToObject(o, "phase", PHASE_NAME[g.ph]);
	cJSON_AddStringToObject(o, "message", g.message);
	cJSON_AddNumberToObject(o, "step", g.running ? g.step + 1 : g.step);
	cJSON_AddNumberToObject(o, "steps", g.n);
	cJSON_AddNumberToObject(o, "measured", g.measured);
	cJSON_AddBoolToObject(o, "full_profile", g.full);
	/* the configured full profile, so the app can name the temperatures a full run would visit */
	{
		cJSON *prof = cJSON_AddArrayToObject(o, "profile");
		cJSON *cfg = pf_set_dup("learning.tune_setpoints"), *it;
		cJSON_ArrayForEach(it, cfg)
			if (cJSON_IsNumber(it)) cJSON_AddItemToArray(prof, cJSON_CreateNumber(round(it->valuedouble)));
		cJSON_Delete(cfg);
		if (cJSON_GetArraySize(prof) == 0) {
			static const double def_f[] = { 180, 225, 350, 450 };
			for (int i = 0; i < 4; i++) cJSON_AddItemToArray(prof, cJSON_CreateNumber(round(pf_from_c(pf_to_c(def_f[i], PF_UNITS_F), u))));
		}
	}
	if (g.running) {
		int at = g.n > 0 ? (g.step < g.n ? g.step : g.n - 1) : 0;   /* never index behind the array */
		cJSON_AddNumberToObject(o, "setpoint", round(pf_from_c(g.points_c[at], u)));
		cJSON_AddNumberToObject(o, "elapsed_s", round(g.run_start > 0 ? g.last_now - g.run_start : 0));
		cJSON *pts = cJSON_AddArrayToObject(o, "setpoints");
		for (int i = 0; i < g.n; i++) cJSON_AddItemToArray(pts, cJSON_CreateNumber(round(pf_from_c(g.points_c[i], u))));
	}
	pthread_mutex_unlock(&g_mu);

	/* the model the measurements built, so the numbers behind the tuning are visible too */
	pf_fopdt m = pf_learning_fopdt();
	if (m.valid) {
		cJSON *pl = cJSON_AddObjectToObject(o, "plant");
		cJSON_AddNumberToObject(pl, "K", round(pf_delta_from_c(m.K, u)));
		cJSON_AddNumberToObject(pl, "tau", round(m.tau));
		cJSON_AddNumberToObject(pl, "theta", round(m.theta));
		cJSON_AddNumberToObject(pl, "ts", m.ts);
	}

	/* the schedule as it stands, which is what the run is building */
	pf_tune_anchor a[PF_TUNE_ANCHORS];
	int n = pf_learning_anchor_list(a, PF_TUNE_ANCHORS);
	cJSON *arr = cJSON_AddArrayToObject(o, "anchors");
	for (int i = 0; i < n; i++) {
		cJSON *e = cJSON_CreateObject();
		cJSON_AddNumberToObject(e, "setpoint", round(pf_from_c(a[i].setpoint_c, u)));
		cJSON_AddNumberToObject(e, "PB", round(pf_delta_from_c(a[i].PB_c, u)));
		cJSON_AddNumberToObject(e, "Ti", round(a[i].Ti));
		cJSON_AddNumberToObject(e, "Td", round(a[i].Td));
		cJSON_AddNumberToObject(e, "ts", a[i].ts);
		if (!isnan(a[i].ambient_c)) cJSON_AddNumberToObject(e, "ambient", round(pf_from_c(a[i].ambient_c, u)));
		if (a[i].wind > 0) cJSON_AddNumberToObject(e, "wind_kmh", round(a[i].wind));
		cJSON_AddItemToArray(arr, e);
	}
	return o;
}
