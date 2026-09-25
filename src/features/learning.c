#include "features/learning.h"
#include "core/db.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include "pifire/common.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "learning"
#define PRIOR_B 0.004        /* feed ratio per degree C of (setpoint - ambient), roughly 0.35 at 225F/70F */
#define PRIOR_A 0.0
#define RIDGE   5.0          /* strength of the prior, in "virtual observations" */
#define MAX_OBS 2000

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pf_ff_fit g_fit;
static bool g_fit_dirty = true;
static pf_fopdt g_fopdt;
static pf_tune_anchor g_anchors[PF_TUNE_ANCHORS];
static unsigned g_at_gen;
static pf_autotune_result g_at;

static void anchors_save(void);
static bool heal_anchor_plant(pf_tune_anchor *a);

static void load_kv(void)
{
	char buf[512];
	/* What is in memory afterwards is what is in THIS database, and nothing else. Loading on top of
	 * whatever happened to be here already meant a fresh database left the previous one's plant and
	 * anchors in place, because a missing row simply left the variable alone -- so a grill whose
	 * learning had been wiped went on predicting from the model it was supposed to have forgotten. */
	memset(&g_fopdt, 0, sizeof g_fopdt);
	memset(&g_at, 0, sizeof g_at);
	memset(g_anchors, 0, sizeof g_anchors);
	if (pf_db_kv_get("learning", "fopdt", buf, sizeof buf) == 0) {
		cJSON *j = cJSON_Parse(buf);
		g_fopdt.K = pf_json_num(j, "K", 0); g_fopdt.tau = pf_json_num(j, "tau", 0); g_fopdt.theta = pf_json_num(j, "theta", 0);
		g_fopdt.ts = pf_json_num(j, "ts", 0); g_fopdt.method = pf_json_int(j, "m", 1);
		g_fopdt.valid = g_fopdt.tau > 0;
		cJSON_Delete(j);
	}
	if (pf_db_kv_get("learning", "anchors", buf, sizeof buf) == 0) {
		cJSON *j = cJSON_Parse(buf), *it;
		int i = 0;
		cJSON_ArrayForEach(it, j) {
			if (i >= PF_TUNE_ANCHORS) break;
			g_anchors[i].setpoint_c = pf_json_num(it, "sp", 0);
			g_anchors[i].Ku = pf_json_num(it, "Ku", 0);
			g_anchors[i].Pu = pf_json_num(it, "Pu", 0);
			g_anchors[i].PB_c = pf_json_num(it, "PB", 0);
			g_anchors[i].Ti = pf_json_num(it, "Ti", 0);
			g_anchors[i].Td = pf_json_num(it, "Td", 0);
			g_anchors[i].K = pf_json_num(it, "K", 0);
			g_anchors[i].tau = pf_json_num(it, "tau", 0);
			g_anchors[i].theta = pf_json_num(it, "theta", 0);
			g_anchors[i].plant_src = pf_json_int(it, "psrc", PF_PLANT_FROM_CAPTURE);
			g_anchors[i].ts = pf_json_num(it, "ts", 0);
			g_anchors[i].ambient_c = pf_json_num(it, "amb", NAN);
			g_anchors[i].wind = pf_json_num(it, "wind", 0);
			g_anchors[i].runs = pf_json_int(it, "runs", 1);
			g_anchors[i].valid = g_anchors[i].PB_c > 0 && g_anchors[i].Ti > 0;
			i++;
		}
		cJSON_Delete(j);
	}
	if (pf_db_kv_get("learning", "autotune", buf, sizeof buf) == 0) {
		cJSON *j = cJSON_Parse(buf);
		g_at.Ku = pf_json_num(j, "Ku", 0); g_at.Pu = pf_json_num(j, "Pu", 0); g_at.PB_c = pf_json_num(j, "PB_c", 0);
		g_at.Ti = pf_json_num(j, "Ti", 0); g_at.Td = pf_json_num(j, "Td", 0); g_at.amplitude_c = pf_json_num(j, "amplitude_c", 0);
		g_at.ts = pf_json_num(j, "ts", 0); g_at.valid = g_at.Pu > 0;
		cJSON_Delete(j);
	}
}

void pf_learning_init(void)
{
	pf_db_exec("CREATE TABLE IF NOT EXISTS observations(id INTEGER PRIMARY KEY, ts REAL, controller TEXT, setpoint_c REAL, ambient_c REAL, u_mean REAL, pit_stdev REAL, pellet TEXT);");
	load_kv();
	/* An entry measured by a build that took its plant from the capture, or blended the two, is put
	 * right here rather than waiting for somebody to be asked for another hour-long run. */
	bool healed = false;
	for (int i = 0; i < PF_TUNE_ANCHORS; i++) healed |= heal_anchor_plant(&g_anchors[i]);
	if (healed) anchors_save();
	g_fit_dirty = true;
}

bool pf_learning_enabled(void) { return pf_set_bool("learning.enabled", true); }

void pf_learning_observe(const char *controller, double setpoint_c, double ambient_c, double u_mean, double pit_stdev_c, const char *pellet)
{
	if (!pf_learning_enabled() || !pf_db_handle()) return;
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "INSERT INTO observations(ts,controller,setpoint_c,ambient_c,u_mean,pit_stdev,pellet) VALUES(?,?,?,?,?,?,?)", -1, &st, NULL) != SQLITE_OK) return;
	sqlite3_bind_double(st, 1, pf_wall());
	sqlite3_bind_text(st, 2, controller, -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(st, 3, setpoint_c);
	sqlite3_bind_double(st, 4, ambient_c);
	sqlite3_bind_double(st, 5, u_mean);
	sqlite3_bind_double(st, 6, pit_stdev_c);
	sqlite3_bind_text(st, 7, pellet ? pellet : "", -1, SQLITE_TRANSIENT);
	sqlite3_step(st);
	sqlite3_finalize(st);
	pthread_mutex_lock(&g_mu);
	g_fit_dirty = true;
	pthread_mutex_unlock(&g_mu);
	LOGI(TAG, "observation: setpoint %.1f C ambient %.1f C u=%.3f", setpoint_c, ambient_c, u_mean);
}

/* weighted ridge regression of u on dT with a prior, newest observations weigh most */
static void refit(void)
{
	/* The recency half-life divides, so a zero or a negative from a hand-edited settings file does
	 * not merely give a poor fit: it makes every weight NaN or makes older observations count for
	 * more than new ones, and the model then silently falls back to its prior for good. */
	double half = pf_set_num("learning.half_life_obs", 60);
	if (!(half >= 1)) half = 60;
	if (half < 5) half = 5;
	double sw = RIDGE, sx = 0, sy = RIDGE * PRIOR_A, sxx = 0, sxy = 0;
	/* prior: RIDGE virtual points at dT = 0 (a) plus RIDGE points expressing slope b */
	double xp = 100.0; /* a virtual point at dT=100 with u = a0 + b0*100 */
	sw += RIDGE; sx += RIDGE * xp; sy += RIDGE * (PRIOR_A + PRIOR_B * xp); sxx += RIDGE * xp * xp; sxy += RIDGE * xp * (PRIOR_A + PRIOR_B * xp);
	int n = 0;
	double ss = 0;
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(pf_db_handle(), "SELECT setpoint_c-ambient_c, u_mean FROM observations ORDER BY id DESC LIMIT ?", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int(st, 1, MAX_OBS);
		int rank = 0;
		while (sqlite3_step(st) == SQLITE_ROW) {
			double x = sqlite3_column_double(st, 0), y = sqlite3_column_double(st, 1);
			double w = pow(0.5, rank / half);
			sw += w; sx += w * x; sy += w * y; sxx += w * x * x; sxy += w * x * y;
			rank++; n++;
		}
		sqlite3_finalize(st);
	}
	/* The determinant of a two-by-two normal-equation system, which is the weighted variance of x
	 * times the total weight. Observations clustered at one set point make it the difference of two
	 * nearly equal large numbers, so testing it against exactly zero is no test at all: it comes
	 * back at 1e-16, the slope divides by it and goes to infinity, and an infinite feed-forward
	 * reaches the cycle engine as a duty. Compare it against the scale of the numbers that made it,
	 * and fall back to the prior when the data cannot support a slope. */
	double det = sw * sxx - sx * sx;
	double scale = sw * sxx;
	double b = (scale > 0 && det > scale * 1e-9) ? (sw * sxy - sx * sy) / det : PRIOR_B;
	double a = sw > 0 ? (sy - b * sx) / sw : 0;
	if (!isfinite(b) || !isfinite(a)) { b = PRIOR_B; a = 0; }
	if (b < 0) { b = 0; a = sw > 0 ? sy / sw : 0; }
	/* residual rms over real observations */
	if (n && sqlite3_prepare_v2(pf_db_handle(), "SELECT setpoint_c-ambient_c, u_mean FROM observations ORDER BY id DESC LIMIT ?", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int(st, 1, MAX_OBS);
		while (sqlite3_step(st) == SQLITE_ROW) { double e = sqlite3_column_double(st, 1) - (a + b * sqlite3_column_double(st, 0)); ss += e * e; }
		sqlite3_finalize(st);
	}
	g_fit.a = a; g_fit.b = b; g_fit.n = n; g_fit.rms = n ? sqrt(ss / n) : 0;
	g_fit_dirty = false;
}

pf_ff_fit pf_learning_fit(void)
{
	pthread_mutex_lock(&g_mu);
	if (g_fit_dirty && pf_db_handle()) refit();
	pf_ff_fit f = g_fit;
	pthread_mutex_unlock(&g_mu);
	return f;
}

double pf_learning_uff(double setpoint_c, double ambient_c, double u_min, double u_max, int *n_out)
{
	pf_ff_fit f = pf_learning_fit();
	if (n_out) *n_out = f.n;
	if (isnan(ambient_c)) ambient_c = 20;
	double u = f.a + f.b * (setpoint_c - ambient_c);

	/* Until the fit has seen this grill, that number is the built-in prior: a guess about pellet
	 * grills in general, and on a real one it came out about twice what the grill actually needed.
	 * The two directions of error are not equal. Feeding too much sends the pit past the target
	 * and a grill has no way to cool itself, so it sits there for as long as the integrator takes
	 * to unwind. Feeding too little is corrected by the integrator within minutes. So an unproven
	 * feed-forward deliberately errs low, and earns its full weight as observations accumulate. */
	int n = f.n < 0 ? 0 : f.n;
	double trust = (double)n / (n + 4.0);              /* 0 with nothing, 0.6 by six cooks' worth */
	u *= 0.85 + 0.15 * trust;

	return pf_clamp(u, u_min, fmax(u_min, u_max - 0.15));
}

void pf_learning_store_fopdt(double K, double tau, double theta)
{
	pthread_mutex_lock(&g_mu);
	/* Blend with the previous estimate so one odd capture does not dominate -- but only when the
	 * previous estimate was measured the same way. The two-point 28/63 fit that came before took
	 * the set point as the step's final value, which it is not, and on a real grill it returned a
	 * time constant of 534 s where that grill's own cooks fit 930 to 1110 s. Averaging a proper
	 * least-squares fit with a number from the broken method halves how much of the measurement
	 * arrives and leaves the stale figure in the model for cooks afterwards. A fit from a newer
	 * method replaces outright; two fits from the same method still average. */
	if (g_fopdt.valid && g_fopdt.method >= PF_FOPDT_METHOD) {
		K = 0.5 * (K + g_fopdt.K); tau = 0.5 * (tau + g_fopdt.tau); theta = 0.5 * (theta + g_fopdt.theta);
	} else if (g_fopdt.valid) {
		LOGI(TAG, "replacing the stored plant (K=%.0f tau=%.0f theta=%.0f) outright: it was fitted by an older method",
		     g_fopdt.K, g_fopdt.tau, g_fopdt.theta);
	}
	g_fopdt.K = K; g_fopdt.tau = tau; g_fopdt.theta = theta; g_fopdt.ts = pf_wall(); g_fopdt.valid = true;
	g_fopdt.method = PF_FOPDT_METHOD;
	char buf[160];
	snprintf(buf, sizeof buf, "{\"K\":%.4f,\"tau\":%.1f,\"theta\":%.1f,\"ts\":%.0f,\"m\":%d}", K, tau, theta, g_fopdt.ts, PF_FOPDT_METHOD);
	if (pf_db_handle()) pf_db_kv_put("learning", "fopdt", buf);
	pthread_mutex_unlock(&g_mu);
	LOGI(TAG, "plant estimate: K=%.3f C per unit feed, tau=%.0f s, theta=%.0f s", K, tau, theta);
}

pf_fopdt pf_learning_fopdt(void) { pthread_mutex_lock(&g_mu); pf_fopdt f = g_fopdt; pthread_mutex_unlock(&g_mu); return f; }

void pf_learning_store_autotune(const pf_autotune_result *r)
{
	pthread_mutex_lock(&g_mu);
	g_at = *r;
	g_at.ts = pf_wall();
	g_at.valid = r->Pu > 0;
	char buf[256];
	snprintf(buf, sizeof buf, "{\"Ku\":%.5f,\"Pu\":%.1f,\"PB_c\":%.2f,\"Ti\":%.1f,\"Td\":%.1f,\"amplitude_c\":%.2f,\"ts\":%.0f}", g_at.Ku, g_at.Pu, g_at.PB_c, g_at.Ti, g_at.Td, g_at.amplitude_c, g_at.ts);
	g_at_gen++;
	if (pf_db_handle()) pf_db_kv_put("learning", "autotune", buf);
	pthread_mutex_unlock(&g_mu);
}


/* ---------------- gain schedule ---------------- */

/* caller holds g_mu */
static void anchors_save(void)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < PF_TUNE_ANCHORS; i++) {
		if (!g_anchors[i].valid) continue;
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "sp", g_anchors[i].setpoint_c);
		cJSON_AddNumberToObject(o, "Ku", g_anchors[i].Ku);
		cJSON_AddNumberToObject(o, "Pu", g_anchors[i].Pu);
		cJSON_AddNumberToObject(o, "PB", g_anchors[i].PB_c);
		cJSON_AddNumberToObject(o, "Ti", g_anchors[i].Ti);
		cJSON_AddNumberToObject(o, "Td", g_anchors[i].Td);
		if (g_anchors[i].K > 0) {
			cJSON_AddNumberToObject(o, "K", g_anchors[i].K);
			cJSON_AddNumberToObject(o, "tau", g_anchors[i].tau);
			cJSON_AddNumberToObject(o, "theta", g_anchors[i].theta);
			cJSON_AddNumberToObject(o, "psrc", g_anchors[i].plant_src);
		}
		cJSON_AddNumberToObject(o, "ts", g_anchors[i].ts);
		cJSON_AddNumberToObject(o, "runs", g_anchors[i].runs);
		if (!isnan(g_anchors[i].ambient_c)) cJSON_AddNumberToObject(o, "amb", g_anchors[i].ambient_c);
		cJSON_AddNumberToObject(o, "wind", g_anchors[i].wind);
		cJSON_AddItemToArray(arr, o);
	}
	char *txt = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);
	if (txt && pf_db_handle()) pf_db_kv_put("learning", "anchors", txt);
	free(txt);
}

/* A plant measured at a set point the library has no entry for yet.
 *
 * During a tuning run the step INTO a set point happens before the relay measures the band there,
 * so the model is always ready before the entry it belongs to exists. Dropping it on the floor for
 * that reason meant a full profile -- the one run whose whole job is to measure the grill at each
 * temperature -- produced no models at all. It waits here instead until the anchor appears. */
static struct { double setpoint_c, K, tau, theta; bool valid; } g_pending_plant;

/* Called with g_mu held. */
/* The plant a relay result implies, given a time constant. The relay fixes the point where the
 * phase reaches -pi; tau is the one thing it cannot see, and the capture measures that. */
static void plant_from_relay(double Ku, double Pu, double tau, double *K, double *theta)
{
	double wu = 2.0 * M_PI / Pu;
	if (theta) *theta = (M_PI - atan(wu * tau)) / wu;
	if (K) *K = sqrt(1.0 + wu * tau * wu * tau) / Ku;
}

/* An anchor that already holds a relay measurement holds everything needed to work its plant out,
 * so one whose plant came from a capture -- or from a build that blended the two -- can be put
 * right where it stands, without waiting for another hour-long run to be asked for. */
static bool heal_anchor_plant(pf_tune_anchor *a)
{
	if (!a->valid || a->plant_src == PF_PLANT_FROM_RELAY) return false;
	if (!(a->Ku > 0) || !(a->Pu > 0) || !(a->tau > 0)) return false;
	double K = 0, theta = 0;
	plant_from_relay(a->Ku, a->Pu, a->tau, &K, &theta);
	if (!(K > 0) || !(theta > 0)) return false;
	LOGI(TAG, "%.0f C: plant recomputed from the relay this entry already holds -- K %.0f (was %.0f), dead time %.0f s (was %.0f)",
	     a->setpoint_c, K, a->K, theta, a->theta);
	a->K = K; a->theta = theta; a->plant_src = PF_PLANT_FROM_RELAY;
	return true;
}

static void anchor_take_plant(pf_tune_anchor *a, double K, double tau, double theta, int src)
{
	/* Two measurements of the same grill average; two measurements of different quality do not.
	 *
	 * A relay is a designed experiment that locates the critical point exactly. A capture is a fit
	 * to whatever the cook happened to do, and it trades dead time against time constant freely --
	 * on this grill it returned 15 s where the relay beside it said 104. Averaging those gave 78,
	 * and since the prediction scales as K*theta/tau it left the loop predicting three quarters of
	 * what the measurement says: the first tune after the relay fix still overshot 11 F where it
	 * should manage five. So a relay result REPLACES a capture's guess outright, a capture never
	 * dilutes a relay result, and only two of a kind are averaged -- which is the same rule the
	 * stored plant already follows for the fit that superseded the two-point method. */
	if (a->K > 0 && src == PF_PLANT_FROM_CAPTURE && a->plant_src == PF_PLANT_FROM_RELAY) return;
	bool replace = a->K <= 0 || (src == PF_PLANT_FROM_RELAY && a->plant_src != PF_PLANT_FROM_RELAY);
	double w = replace ? 1.0 : fmax(1.0 / (a->runs > 0 ? a->runs : 1), 0.25);
	a->plant_src = src;
	a->K += (K - a->K) * w;
	a->tau += (tau - a->tau) * w;
	a->theta += (theta - a->theta) * w;
	LOGI(TAG, "%.0f C: plant %.0f C per unit feed, time constant %.0f s, dead time %.0f s",
	     a->setpoint_c, a->K, a->tau, a->theta);
}

void pf_learning_store_anchor(double setpoint_c, const pf_autotune_result *r, double ambient_c, double wind)
{
	if (!r || r->PB_c <= 0 || r->Ti <= 0) return;
	pthread_mutex_lock(&g_mu);
	/* one entry per set point: a repeat measurement replaces the nearest within 5 C */
	int slot = -1;
	for (int i = 0; i < PF_TUNE_ANCHORS; i++)
		if (g_anchors[i].valid && fabs(g_anchors[i].setpoint_c - setpoint_c) < 5) { slot = i; break; }
	if (slot < 0) for (int i = 0; i < PF_TUNE_ANCHORS; i++) if (!g_anchors[i].valid) { slot = i; break; }
	if (slot < 0) {
		/* Full. Give up the least useful entry, not the furthest one: an anchor sitting close to a
		 * neighbour is nearly free to lose, because the interpolation between the two either side
		 * will land near where it was, while the entry at the end of the range is the only thing
		 * describing the grill out there. Among equally redundant ones, the shallowest goes --
		 * fewest runs behind it. */
		double least = HUGE_VAL;
		slot = 0;
		for (int i = 0; i < PF_TUNE_ANCHORS; i++) {
			double nearest = 1e9;
			for (int j = 0; j < PF_TUNE_ANCHORS; j++) {
				if (j == i || !g_anchors[j].valid) continue;
				double d = fabs(g_anchors[i].setpoint_c - g_anchors[j].setpoint_c);
				if (d < nearest) nearest = d;
			}
			/* worth = how alone it is, weighted by how well measured it is */
			double worth = nearest * (1.0 + 0.25 * (g_anchors[i].runs > 4 ? 4 : g_anchors[i].runs));
			if (worth < least) { least = worth; slot = i; }
		}
		LOGW(TAG, "the tuning library is full; %.0f C makes way for %.0f C",
		     g_anchors[slot].setpoint_c, setpoint_c);
	}
	pf_tune_anchor *a = &g_anchors[slot];
	/* How far a new measurement moves an entry that already exists. The first repeat moves it
	 * half way, the next a third, and from the fourth onwards a quarter -- successive runs average
	 * out the noise of any one afternoon. The floor is deliberate: a grill that has been
	 * re-gasketed, or is burning a different pellet, has genuinely changed, and a library that
	 * kept averaging in years of old evidence could never follow it. */
	double w = a->valid && fabs(a->setpoint_c - setpoint_c) < 5 ? fmax(1.0 / (a->runs + 1), 0.25) : 1.0;
	if (w >= 1.0) {
		a->Ku = r->Ku; a->Pu = r->Pu; a->PB_c = r->PB_c; a->Ti = r->Ti; a->Td = r->Td;
		a->runs = 1;
	} else {
		a->Ku += (r->Ku - a->Ku) * w;
		a->Pu += (r->Pu - a->Pu) * w;
		a->PB_c += (r->PB_c - a->PB_c) * w;
		a->Ti += (r->Ti - a->Ti) * w;
		a->Td += (r->Td - a->Td) * w;
		a->runs++;
		LOGI(TAG, "refined %.0f C with run %d (weight %.2f): PB %.1f C, Ti %.0f s, Td %.0f s",
		     setpoint_c, a->runs, w, a->PB_c, a->Ti, a->Td);
	}
	/* The conditions describe the most recent evidence, not an average of weather. */
	a->setpoint_c = setpoint_c;
	a->ts = pf_wall();
	a->ambient_c = ambient_c;
	a->wind = wind;
	a->valid = true;
	/* The plant for this set point, from the two measurements that are each good at one half of it.
	 *
	 * The relay is a designed experiment and it locates one point of the grill's frequency response
	 * exactly: the frequency where the phase reaches -pi, and the gain there. For a first-order plant
	 * with a dead time that fixes the dead time and the static gain outright, given the time
	 * constant -- which is the one thing a relay cannot see, because it never waits for the pit to
	 * finish arriving anywhere. The step into the set point is what measures that.
	 *
	 *     w = 2*pi/Pu      theta = (pi - atan(w*tau)) / w      K = sqrt(1 + (w*tau)^2) / Ku
	 *
	 * Taking the whole plant from the step fit instead was leaving the prediction far weaker than
	 * the grill needs. On this grill's own baseline run the step fit returned theta = 15 s and
	 * K = 466 C per unit feed where the relay beside it says 97 s and 336 -- and the second pair is
	 * what four of that grill's real cooks fit (70-80 s, 313-348). The Smith prediction scales as
	 * K*theta/tau, so the difference is a prediction about five times too small: the loop would have
	 * gone on feeding through the dead time and sailed past the set point, which is the whole fault
	 * the prediction exists to stop.
	 *
	 * Note theta always lands between Pu/4 and Pu/2, whatever tau is, because atan is bounded. The
	 * relay therefore brackets the dead time on its own, and a bad tau can only move it inside that
	 * bracket -- which is a far better failure than a grid search sliding to the end of its range. */
	double tau_src = g_pending_plant.valid && fabs(g_pending_plant.setpoint_c - setpoint_c) < 5
	               ? g_pending_plant.tau : a->tau > 0 ? a->tau : g_fopdt.valid ? g_fopdt.tau : 0;
	if (r->Ku > 0 && r->Pu > 0 && tau_src > 0) {
		double theta = 0, K = 0;
		plant_from_relay(r->Ku, r->Pu, tau_src, &K, &theta);
		LOGI(TAG, "%.0f C: plant from the relay -- K %.0f C per unit feed, dead time %.0f s (time constant %.0f s from the capture)",
		     setpoint_c, K, theta, tau_src);
		anchor_take_plant(a, K, tau_src, theta, PF_PLANT_FROM_RELAY);
		g_pending_plant.valid = false;
	} else if (g_pending_plant.valid && fabs(g_pending_plant.setpoint_c - setpoint_c) < 5) {
		/* no usable relay result: the step fit is all there is */
		anchor_take_plant(a, g_pending_plant.K, g_pending_plant.tau, g_pending_plant.theta, PF_PLANT_FROM_CAPTURE);
		g_pending_plant.valid = false;
	}
	anchors_save();
	pthread_mutex_unlock(&g_mu);
}

void pf_learning_put_anchor(const pf_tune_anchor *in)
{
	if (!in || !in->valid || in->PB_c <= 0 || in->Ti <= 0) return;
	pthread_mutex_lock(&g_mu);
	int slot = -1;
	for (int i = 0; i < PF_TUNE_ANCHORS; i++)
		if (g_anchors[i].valid && fabs(g_anchors[i].setpoint_c - in->setpoint_c) < 5) { slot = i; break; }
	if (slot < 0) for (int i = 0; i < PF_TUNE_ANCHORS; i++) if (!g_anchors[i].valid) { slot = i; break; }
	if (slot >= 0) { g_anchors[slot] = *in; g_anchors[slot].valid = true; anchors_save(); }
	pthread_mutex_unlock(&g_mu);
}

void pf_learning_store_anchor_plant(double setpoint_c, double K, double tau, double theta)
{
	if (!(K > 0) || !(tau > 0)) return;
	pthread_mutex_lock(&g_mu);
	bool filed = false;
	for (int i = 0; i < PF_TUNE_ANCHORS; i++) {
		pf_tune_anchor *a = &g_anchors[i];
		if (!a->valid || fabs(a->setpoint_c - setpoint_c) >= 5) continue;
		anchor_take_plant(a, K, tau, theta, PF_PLANT_FROM_CAPTURE);
		anchors_save();
		filed = true;
		break;
	}
	if (!filed) {
		g_pending_plant.setpoint_c = setpoint_c; g_pending_plant.K = K;
		g_pending_plant.tau = tau; g_pending_plant.theta = theta; g_pending_plant.valid = true;
	}
	pthread_mutex_unlock(&g_mu);
}

bool pf_learning_plant(double setpoint_c, double *K, double *tau, double *theta)
{
	pthread_mutex_lock(&g_mu);
	const pf_tune_anchor *lo = NULL, *hi = NULL;
	for (int i = 0; i < PF_TUNE_ANCHORS; i++) {
		const pf_tune_anchor *a = &g_anchors[i];
		if (!a->valid || !(a->K > 0) || !(a->tau > 0)) continue;
		if (a->setpoint_c <= setpoint_c && (!lo || a->setpoint_c > lo->setpoint_c)) lo = a;
		if (a->setpoint_c >= setpoint_c && (!hi || a->setpoint_c < hi->setpoint_c)) hi = a;
	}
	const pf_tune_anchor *one = lo ? lo : hi;
	bool ok = one != NULL;
	if (ok) {
		double f = lo && hi && hi->setpoint_c > lo->setpoint_c
		         ? (setpoint_c - lo->setpoint_c) / (hi->setpoint_c - lo->setpoint_c) : -1;
		if (f >= 0) {
			if (K) *K = lo->K + f * (hi->K - lo->K);
			if (tau) *tau = lo->tau + f * (hi->tau - lo->tau);
			if (theta) *theta = lo->theta + f * (hi->theta - lo->theta);
		} else {
			if (K) *K = one->K;
			if (tau) *tau = one->tau;
			if (theta) *theta = one->theta;
		}
	}
	pthread_mutex_unlock(&g_mu);
	if (!ok) {
		/* Nothing in the library yet: the last rise from cold is the whole of what is known -- as
		 * long as it was fitted by a method still in use. A model left behind by the two-point fit
		 * has a known, one-directional error in it (gain understated, time constant roughly
		 * halved), and the prediction is built on the model, so the controller's own prior is the
		 * better answer until a proper capture replaces it. A measurement with a known bias is not
		 * evidence. It stays on record and on the learning page; it just does not drive the loop. */
		pf_fopdt p = pf_learning_fopdt();
		if (!p.valid || p.method < PF_FOPDT_METHOD) return false;
		if (K) *K = p.K;
		if (tau) *tau = p.tau;
		if (theta) *theta = p.theta;
		ok = true;
	}
	return ok;
}

/* An estimate of the climb, from the two things the grill teaches itself.
 *
 * The feed-forward fit says what duty holds what temperature -- u = a + b*(setpoint - ambient) --
 * and read backwards it says the opposite: the temperature that a given duty would eventually hold
 * is ambient + (u - a)/b. At full feed that is where the pit is heading, and the plant model says
 * how fast it gets there and how long before it starts.
 *
 *     T_inf = ambient + (u_max - a) / b
 *     t     = theta + tau * ln((T_inf - T0) / (T_inf - T1))
 *
 * Both halves improve with every cook, which is why the answer gets better: the feed-forward fit
 * gains an observation every five minutes of steady holding, and the plant is re-measured on every
 * step between set points. Before either exists there is nothing honest to say, so it says nothing
 * rather than inventing a number. */
double pf_learning_time_to(double from_c, double to_c, double ambient_c, double u_max)
{
	if (!(to_c > from_c)) return -1;                  /* cooling is not a climb the fire controls */
	double K = 0, tau = 0, theta = 0;
	if (!pf_learning_plant(to_c, &K, &tau, &theta) || !(tau > 0)) return -1;
	pf_ff_fit f = pf_learning_fit();
	if (f.n < 3 || !(f.b > 1e-6)) return -1;          /* no idea yet what duty holds what */
	if (!(u_max > 0)) u_max = 0.9;
	double t_inf = ambient_c + (u_max - f.a) / f.b;
	/* The pit has to be heading somewhere past where it is going, or the sum says nothing: a grill
	 * that cannot reach the temperature asked for has no time to give. */
	if (!(t_inf > to_c + 1.0) || !(t_inf > from_c)) return -1;
	double t = theta + tau * log((t_inf - from_c) / (t_inf - to_c));
	return (t > 0 && t < 24 * 3600 && isfinite(t)) ? t : -1;
}

bool pf_learning_gains(double setpoint_c, double *PB_c, double *Ti, double *Td)
{
	pthread_mutex_lock(&g_mu);
	/* the two anchors that bracket this set point, or the single nearest one outside their range */
	const pf_tune_anchor *lo = NULL, *hi = NULL;
	for (int i = 0; i < PF_TUNE_ANCHORS; i++) {
		const pf_tune_anchor *a = &g_anchors[i];
		if (!a->valid) continue;
		if (a->setpoint_c <= setpoint_c && (!lo || a->setpoint_c > lo->setpoint_c)) lo = a;
		if (a->setpoint_c >= setpoint_c && (!hi || a->setpoint_c < hi->setpoint_c)) hi = a;
	}
	const pf_tune_anchor *one = lo ? lo : hi;
	bool ok = one != NULL;
	if (ok) {
		if (lo && hi && hi->setpoint_c > lo->setpoint_c) {
			double f = (setpoint_c - lo->setpoint_c) / (hi->setpoint_c - lo->setpoint_c);
			if (PB_c) *PB_c = lo->PB_c + f * (hi->PB_c - lo->PB_c);
			if (Ti) *Ti = lo->Ti + f * (hi->Ti - lo->Ti);
			if (Td) *Td = lo->Td + f * (hi->Td - lo->Td);
		} else {
			if (PB_c) *PB_c = one->PB_c;
			if (Ti) *Ti = one->Ti;
			if (Td) *Td = one->Td;
		}
	}
	pthread_mutex_unlock(&g_mu);
	return ok;
}

int pf_learning_anchor_list(pf_tune_anchor *out, int max)
{
	pthread_mutex_lock(&g_mu);
	int n = 0;
	for (int i = 0; i < PF_TUNE_ANCHORS && n < max; i++) if (g_anchors[i].valid) out[n++] = g_anchors[i];
	pthread_mutex_unlock(&g_mu);
	/* ascending set point, so the UI and any interpolation read naturally */
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && out[j - 1].setpoint_c > out[j].setpoint_c; j--) {
			pf_tune_anchor t = out[j - 1]; out[j - 1] = out[j]; out[j] = t;
		}
	return n;
}

void pf_learning_clear_anchors(void)
{
	pthread_mutex_lock(&g_mu);
	memset(g_anchors, 0, sizeof g_anchors);
	anchors_save();
	pthread_mutex_unlock(&g_mu);
}

unsigned pf_learning_autotune_gen(void) { pthread_mutex_lock(&g_mu); unsigned v = g_at_gen; pthread_mutex_unlock(&g_mu); return v; }

pf_autotune_result pf_learning_autotune(void) { pthread_mutex_lock(&g_mu); pf_autotune_result r = g_at; pthread_mutex_unlock(&g_mu); return r; }

void pf_learning_forget(void)
{
	/* What the grill worked out for itself, and nothing it was told or measured on purpose: the
	 * steady-state observations behind the feed-forward, and (through the controller) the
	 * per-temperature corrections it has settled on. The tuning library is a measurement -- someone
	 * lit the grill and it oscillated the loop deliberately to get it -- so it stays. */
	pf_db_exec("DELETE FROM observations");
	pthread_mutex_lock(&g_mu);
	g_fit_dirty = true;
	pthread_mutex_unlock(&g_mu);
	LOGI(TAG, "learning cleared; the measured tuning kept");
}

void pf_learning_clear_tuning(void)
{
	/* Everything measured, so the grill goes back to the numbers that were typed on the controller
	 * page: the tuning library, the last relay result, and the plant fitted from startup rises,
	 * which is what would otherwise hand the controller a tuning again on the next cook. */
	pf_db_kv_delete("learning", "fopdt");
	pf_db_kv_delete("learning", "autotune");
	pthread_mutex_lock(&g_mu);
	memset(&g_fopdt, 0, sizeof g_fopdt);
	memset(&g_at, 0, sizeof g_at);
	memset(g_anchors, 0, sizeof g_anchors);
	anchors_save();
	pthread_mutex_unlock(&g_mu);
	LOGI(TAG, "measured tuning cleared; the grill is back to the values that were typed");
}

void pf_learning_reset(void)
{
	pf_learning_clear_tuning();
	pf_learning_forget();
}

cJSON *pf_learning_json(void)
{
	pf_units u = pf_settings_units();
	pf_ff_fit f = pf_learning_fit();
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "enabled", pf_learning_enabled());
	cJSON *ff = cJSON_AddObjectToObject(o, "feedforward");
	cJSON_AddNumberToObject(ff, "a", f.a);
	cJSON_AddNumberToObject(ff, "b_per_degC", f.b);
	cJSON_AddNumberToObject(ff, "observations", f.n);
	cJSON_AddNumberToObject(ff, "rms", f.rms);
	/* example: what the model predicts for a few set points at the current ambient */
	cJSON *ex = cJSON_AddArrayToObject(ff, "examples");
	double amb = 20;
	for (double spf = 180; spf <= 350; spf += 45) {
		cJSON *e = cJSON_CreateObject();
		double spc = pf_f_to_c(spf);
		cJSON_AddNumberToObject(e, "setpoint", round(pf_from_c(spc, u)));
		cJSON_AddNumberToObject(e, "u", round((f.a + f.b * (spc - amb)) * 1000) / 1000);
		cJSON_AddItemToArray(ex, e);
	}
	cJSON_AddNumberToObject(ff, "example_ambient", round(pf_from_c(amb, u)));
	pf_fopdt p = pf_learning_fopdt();
	cJSON *pl = cJSON_AddObjectToObject(o, "plant");
	cJSON_AddBoolToObject(pl, "valid", p.valid);
	cJSON_AddNumberToObject(pl, "K", p.valid ? pf_delta_from_c(p.K, u) : 0);
	cJSON_AddNumberToObject(pl, "tau", p.tau);
	cJSON_AddNumberToObject(pl, "theta", p.theta);
	cJSON_AddNumberToObject(pl, "ts", p.ts);
	pf_autotune_result a = pf_learning_autotune();
	cJSON *at = cJSON_AddObjectToObject(o, "autotune");
	cJSON_AddBoolToObject(at, "valid", a.valid);
	cJSON_AddNumberToObject(at, "Ku", a.Ku);
	cJSON_AddNumberToObject(at, "Pu", a.Pu);
	cJSON_AddNumberToObject(at, "PB", a.valid ? round(pf_delta_from_c(a.PB_c, u) * 10) / 10 : 0);
	cJSON_AddNumberToObject(at, "Ti", round(a.Ti));
	cJSON_AddNumberToObject(at, "Td", round(a.Td));
	cJSON_AddNumberToObject(at, "amplitude", a.valid ? round(pf_delta_from_c(a.amplitude_c, u) * 10) / 10 : 0);
	cJSON_AddNumberToObject(at, "ts", a.ts);
	/* recent observations */
	cJSON *obs = cJSON_AddArrayToObject(o, "recent");
	sqlite3_stmt *st;
	if (pf_db_handle() && sqlite3_prepare_v2(pf_db_handle(), "SELECT ts,controller,setpoint_c,ambient_c,u_mean,pellet FROM observations ORDER BY id DESC LIMIT 20", -1, &st, NULL) == SQLITE_OK) {
		while (sqlite3_step(st) == SQLITE_ROW) {
			cJSON *e = cJSON_CreateObject();
			cJSON_AddNumberToObject(e, "ts", sqlite3_column_double(st, 0));
			cJSON_AddStringToObject(e, "controller", (const char *)sqlite3_column_text(st, 1));
			cJSON_AddNumberToObject(e, "setpoint", round(pf_from_c(sqlite3_column_double(st, 2), u)));
			cJSON_AddNumberToObject(e, "ambient", round(pf_from_c(sqlite3_column_double(st, 3), u)));
			cJSON_AddNumberToObject(e, "u", round(sqlite3_column_double(st, 4) * 1000) / 1000);
			cJSON_AddStringToObject(e, "pellet", sqlite3_column_text(st, 5) ? (const char *)sqlite3_column_text(st, 5) : "");
			cJSON_AddItemToArray(obs, e);
		}
		sqlite3_finalize(st);
	}
	return o;
}

/* ------------------------------------------------------------------ backup */

#define PF_TUNE_EXPORT_KIND "pifire-tuning"
#define PF_TUNE_EXPORT_VER  1

cJSON *pf_learning_export(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "kind", PF_TUNE_EXPORT_KIND);
	cJSON_AddNumberToObject(o, "version", PF_TUNE_EXPORT_VER);
	cJSON_AddNumberToObject(o, "exported_at", pf_wall());
	cJSON_AddStringToObject(o, "units", "C");   /* canonical: a backup outlives a unit change */
	char name[64];
	pf_set_str("globals.grill_name", name, sizeof name, "PiFire");
	cJSON_AddStringToObject(o, "grill", name);
#ifdef PF_VERSION
	cJSON_AddStringToObject(o, "daemon", PF_VERSION);
#endif

	/* The controller this tuning was measured against, and the numbers it is running on now.
	 * PB/Ti/Td mean nothing without knowing which controller they belong to. */
	char cid[40];
	pf_set_str("controller.selected", cid, sizeof cid, "adaptive");
	cJSON *ctl = cJSON_AddObjectToObject(o, "controller");
	cJSON_AddStringToObject(ctl, "id", cid);
	char path[96];
	snprintf(path, sizeof path, "controller.config.%.40s", cid);
	cJSON *cfg = pf_set_dup(path);
	if (cfg) cJSON_AddItemToObject(ctl, "config", cfg);

	pf_ff_fit f = pf_learning_fit();
	cJSON *ff = cJSON_AddObjectToObject(o, "feedforward");
	cJSON_AddNumberToObject(ff, "a", f.a);
	cJSON_AddNumberToObject(ff, "b", f.b);
	cJSON_AddNumberToObject(ff, "n", f.n);
	cJSON_AddNumberToObject(ff, "rms", f.rms);

	pf_fopdt m = pf_learning_fopdt();
	if (m.valid) {
		cJSON *pl = cJSON_AddObjectToObject(o, "plant");
		cJSON_AddNumberToObject(pl, "K", m.K);
		cJSON_AddNumberToObject(pl, "tau", m.tau);
		cJSON_AddNumberToObject(pl, "theta", m.theta);
		cJSON_AddNumberToObject(pl, "ts", m.ts);
	}

	/* The library itself: one measured anchor per set point, with the conditions it was taken in,
	 * because a tune measured in a 20 F wind is not the same evidence as one taken on a still day. */
	pf_tune_anchor a[PF_TUNE_ANCHORS];
	int na = pf_learning_anchor_list(a, PF_TUNE_ANCHORS);
	cJSON *arr = cJSON_AddArrayToObject(o, "anchors");
	for (int i = 0; i < na; i++) {
		if (!a[i].valid) continue;
		cJSON *e = cJSON_CreateObject();
		cJSON_AddNumberToObject(e, "setpoint_c", a[i].setpoint_c);
		cJSON_AddNumberToObject(e, "Ku", a[i].Ku);
		cJSON_AddNumberToObject(e, "Pu", a[i].Pu);
		cJSON_AddNumberToObject(e, "PB_c", a[i].PB_c);
		cJSON_AddNumberToObject(e, "Ti", a[i].Ti);
		cJSON_AddNumberToObject(e, "Td", a[i].Td);
		if (a[i].K > 0) {
			/* The plant fitted at this set point travels with the tuning: the controller's
			 * prediction is built from it, so a library restored without it is not the library
			 * that was backed up. */
			cJSON_AddNumberToObject(e, "K", a[i].K);
			cJSON_AddNumberToObject(e, "tau", a[i].tau);
			cJSON_AddNumberToObject(e, "theta", a[i].theta);
		}
		cJSON_AddNumberToObject(e, "ts", a[i].ts);
		cJSON_AddNumberToObject(e, "runs", a[i].runs);
		if (!isnan(a[i].ambient_c)) cJSON_AddNumberToObject(e, "ambient_c", a[i].ambient_c);
		cJSON_AddNumberToObject(e, "wind", a[i].wind);
		cJSON_AddItemToArray(arr, e);
	}
	return o;
}

int pf_learning_import(const cJSON *doc, char *err, size_t n)
{
	if (!cJSON_IsObject(doc) || strcmp(pf_json_str((cJSON *)doc, "kind", ""), PF_TUNE_EXPORT_KIND)) {
		snprintf(err, n, "that is not a PiFire tuning backup");
		return -1;
	}
	if (pf_json_num((cJSON *)doc, "version", 0) > PF_TUNE_EXPORT_VER) {
		snprintf(err, n, "that backup was written by a newer version of PiFire");
		return -1;
	}
	const cJSON *arr = cJSON_GetObjectItemCaseSensitive((cJSON *)doc, "anchors");
	if (!cJSON_IsArray(arr)) { snprintf(err, n, "the backup has no tuning library in it"); return -1; }

	/* All or nothing: a half-restored library would interpolate between one grill's measurements
	 * and another's, which is worse than either. */
	pf_learning_clear_anchors();
	int k = 0;
	const cJSON *e;
	cJSON_ArrayForEach(e, arr) {
		/* A backup is not new evidence about the grill: it is put back exactly as it was, runs
		 * count and all, rather than averaged into anything. */
		pf_tune_anchor a = {
			.setpoint_c = pf_json_num((cJSON *)e, "setpoint_c", 0),
			.Ku = pf_json_num((cJSON *)e, "Ku", 0), .Pu = pf_json_num((cJSON *)e, "Pu", 0),
			.PB_c = pf_json_num((cJSON *)e, "PB_c", 0), .Ti = pf_json_num((cJSON *)e, "Ti", 0),
			.Td = pf_json_num((cJSON *)e, "Td", 0),
			.K = pf_json_num((cJSON *)e, "K", 0), .tau = pf_json_num((cJSON *)e, "tau", 0),
			.theta = pf_json_num((cJSON *)e, "theta", 0), .ts = pf_json_num((cJSON *)e, "ts", pf_wall()),
			.ambient_c = pf_json_num((cJSON *)e, "ambient_c", NAN),
			.wind = pf_json_num((cJSON *)e, "wind", 0),
			.runs = pf_json_int((cJSON *)e, "runs", 1),
			.valid = true,
		};
		if (a.setpoint_c <= 0 || a.PB_c <= 0 || a.Ti <= 0) continue;   /* not a measurement */
		pf_learning_put_anchor(&a);
		k++;
	}
	const cJSON *pl = cJSON_GetObjectItemCaseSensitive((cJSON *)doc, "plant");
	if (cJSON_IsObject(pl))
		pf_learning_store_fopdt(pf_json_num((cJSON *)pl, "K", 0), pf_json_num((cJSON *)pl, "tau", 0),
		                        pf_json_num((cJSON *)pl, "theta", 0));
	LOGI(TAG, "restored %d tuning anchor%s from a backup", k, k == 1 ? "" : "s");
	return k;
}
