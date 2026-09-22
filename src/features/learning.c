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

static void load_kv(void)
{
	char buf[512];
	if (pf_db_kv_get("learning", "fopdt", buf, sizeof buf) == 0) {
		cJSON *j = cJSON_Parse(buf);
		g_fopdt.K = pf_json_num(j, "K", 0); g_fopdt.tau = pf_json_num(j, "tau", 0); g_fopdt.theta = pf_json_num(j, "theta", 0);
		g_fopdt.ts = pf_json_num(j, "ts", 0); g_fopdt.valid = g_fopdt.tau > 0;
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
	/* blend with the previous estimate so one odd startup doesn't dominate */
	if (g_fopdt.valid) { K = 0.5 * (K + g_fopdt.K); tau = 0.5 * (tau + g_fopdt.tau); theta = 0.5 * (theta + g_fopdt.theta); }
	g_fopdt.K = K; g_fopdt.tau = tau; g_fopdt.theta = theta; g_fopdt.ts = pf_wall(); g_fopdt.valid = true;
	char buf[160];
	snprintf(buf, sizeof buf, "{\"K\":%.4f,\"tau\":%.1f,\"theta\":%.1f,\"ts\":%.0f}", K, tau, theta, g_fopdt.ts);
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

void pf_learning_store_anchor(double setpoint_c, const pf_autotune_result *r, double ambient_c, double wind)
{
	if (!r || r->PB_c <= 0 || r->Ti <= 0) return;
	pthread_mutex_lock(&g_mu);
	/* one entry per set point: a repeat measurement replaces the nearest within 5 C */
	int slot = -1;
	for (int i = 0; i < PF_TUNE_ANCHORS; i++)
		if (g_anchors[i].valid && fabs(g_anchors[i].setpoint_c - setpoint_c) < 5) { slot = i; break; }
	if (slot < 0) for (int i = 0; i < PF_TUNE_ANCHORS; i++) if (!g_anchors[i].valid) { slot = i; break; }
	if (slot < 0) {   /* full: replace whichever is furthest from this set point */
		double worst = -1;
		for (int i = 0; i < PF_TUNE_ANCHORS; i++) {
			double d = fabs(g_anchors[i].setpoint_c - setpoint_c);
			if (d > worst) { worst = d; slot = i; }
		}
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

void pf_learning_reset(void)
{
	pf_db_exec("DELETE FROM observations");
	pf_db_kv_delete_ns("learning");
	pthread_mutex_lock(&g_mu);
	memset(&g_fopdt, 0, sizeof g_fopdt);
	memset(&g_at, 0, sizeof g_at);
	/* the tuning library goes too, and in memory as well: deleting only the stored copy would
	 * leave the controller following a schedule the user has just asked to be rid of */
	memset(g_anchors, 0, sizeof g_anchors);
	g_fit_dirty = true;
	pthread_mutex_unlock(&g_mu);
	LOGI(TAG, "learning data cleared");
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
			.Td = pf_json_num((cJSON *)e, "Td", 0), .ts = pf_json_num((cJSON *)e, "ts", pf_wall()),
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
