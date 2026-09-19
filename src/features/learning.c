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
	double half = pf_set_num("learning.half_life_obs", 60);
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
	double det = sw * sxx - sx * sx;
	double b = det != 0 ? (sw * sxy - sx * sy) / det : PRIOR_B;
	double a = (sy - b * sx) / sw;
	if (b < 0) { b = 0; a = sy / sw; }
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
	if (pf_db_handle()) pf_db_kv_put("learning", "autotune", buf);
	pthread_mutex_unlock(&g_mu);
}

pf_autotune_result pf_learning_autotune(void) { pthread_mutex_lock(&g_mu); pf_autotune_result r = g_at; pthread_mutex_unlock(&g_mu); return r; }

void pf_learning_reset(void)
{
	pf_db_exec("DELETE FROM observations");
	pf_db_kv_delete_ns("learning");
	pthread_mutex_lock(&g_mu);
	memset(&g_fopdt, 0, sizeof g_fopdt);
	memset(&g_at, 0, sizeof g_at);
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
