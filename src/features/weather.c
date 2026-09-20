#define _GNU_SOURCE
#include "features/weather.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "weather"
#define REFRESH_S 900.0
#define STALE_S   7200.0

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pf_weather g_w;
static char g_err[128];
static double g_next_fetch;
static bool g_sim;

#if PF_WITH_CURL
#include <curl/curl.h>
static pthread_t g_tid;
static atomic_bool g_run, g_kick;

typedef struct { char *buf; size_t len; } mem_t;
static size_t collect(char *p, size_t s, size_t n, void *ud)
{
	mem_t *m = ud;
	size_t add = s * n;
	char *nb = realloc(m->buf, m->len + add + 1);
	if (!nb) return 0;
	m->buf = nb;
	memcpy(m->buf + m->len, p, add);
	m->len += add;
	m->buf[m->len] = 0;
	return add;
}

static cJSON *get_json(const char *url, char *err, size_t errn)
{
	CURL *c = curl_easy_init();
	if (!c) { snprintf(err, errn, "curl init failed"); return NULL; }
	mem_t m = { 0 };
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "PiFire/" PF_VERSION);
	CURLcode rc = curl_easy_perform(c);
	long http = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK) { snprintf(err, errn, "%s", curl_easy_strerror(rc)); free(m.buf); return NULL; }
	if (http >= 400) { snprintf(err, errn, "HTTP %ld", http); free(m.buf); return NULL; }
	cJSON *j = m.buf ? cJSON_Parse(m.buf) : NULL;
	free(m.buf);
	if (!j) snprintf(err, errn, "bad reply");
	return j;
}

/* postal code -> latitude/longitude/place, cached in settings so it is resolved once */
static bool geocode(char *err, size_t errn)
{
	char country[8], code[24];
	pf_set_str("weather.country", country, sizeof country, "us");
	pf_set_str("weather.postal_code", code, sizeof code, "");
	if (!code[0]) { snprintf(err, errn, "no postal code"); return false; }
	for (char *q = country; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
	char url[160];
	snprintf(url, sizeof url, "https://api.zippopotam.us/%s/%s", country, code);
	cJSON *j = get_json(url, err, errn);
	if (!j) { char e2[128]; snprintf(e2, sizeof e2, "postal code lookup failed (%.90s)", err); pf_strlcpy(err, e2, errn); return false; }
	cJSON *place = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "places"), 0);
	double lat = atof(pf_json_str(place, "latitude", "0")), lon = atof(pf_json_str(place, "longitude", "0"));
	if (!place || (lat == 0 && lon == 0)) { cJSON_Delete(j); snprintf(err, errn, "postal code %s not found", code); return false; }
	char name[64];
	snprintf(name, sizeof name, "%.40s, %.10s", pf_json_str(place, "place name", code), pf_json_str(place, "state abbreviation", pf_json_str(j, "country abbreviation", "")));
	cJSON_Delete(j);
	pf_set_put_num("weather.latitude", lat);
	pf_set_put_num("weather.longitude", lon);
	pf_set_put_str("weather.place", name);
	LOGI(TAG, "%s -> %s (%.3f, %.3f)", code, name, lat, lon);
	return true;
}

static void fetch(void)
{
	char err[128] = "";
	double lat = pf_set_num("weather.latitude", 0), lon = pf_set_num("weather.longitude", 0);
	if ((lat == 0 && lon == 0) && !geocode(err, sizeof err)) goto fail;
	lat = pf_set_num("weather.latitude", 0); lon = pf_set_num("weather.longitude", 0);
	char url[300];
	snprintf(url, sizeof url, "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,relative_humidity_2m,wind_speed_10m,wind_gusts_10m&wind_speed_unit=kmh&timezone=UTC", lat, lon);
	cJSON *j = get_json(url, err, sizeof err);
	if (!j) goto fail;
	cJSON *cur = cJSON_GetObjectItem(j, "current");
	if (!cJSON_IsNumber(cJSON_GetObjectItem(cur, "temperature_2m"))) { cJSON_Delete(j); snprintf(err, sizeof err, "no current conditions in reply"); goto fail; }
	pthread_mutex_lock(&g_mu);
	g_w.valid = true;
	g_w.temp_c = pf_json_num(cur, "temperature_2m", NAN);
	g_w.humidity_pct = pf_json_num(cur, "relative_humidity_2m", NAN);
	g_w.wind_kmh = pf_json_num(cur, "wind_speed_10m", NAN);
	g_w.gust_kmh = pf_json_num(cur, "wind_gusts_10m", NAN);
	g_w.ts = pf_wall();
	pf_set_str("weather.place", g_w.place, sizeof g_w.place, "");
	g_err[0] = 0;
	pthread_mutex_unlock(&g_mu);
	cJSON_Delete(j);
	LOGI(TAG, "%s: %.1f C, wind %.0f km/h, humidity %.0f%%", g_w.place, g_w.temp_c, g_w.wind_kmh, g_w.humidity_pct);
	return;
fail:
	pthread_mutex_lock(&g_mu);
	pf_strlcpy(g_err, err, sizeof g_err);
	pthread_mutex_unlock(&g_mu);
	LOGW(TAG, "%s", err);
}

static void *worker(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-weather");
	double next = pf_now() + 20;   /* let the network come up first */
	while (atomic_load(&g_run)) {
		pf_sleep_ms(500);
		bool kick = atomic_exchange(&g_kick, false);
		if (!kick && pf_now() < next) continue;
		next = pf_now() + REFRESH_S;
		if (!pf_set_bool("weather.enabled", false)) continue;
		if (kick) { pf_set_put_num("weather.latitude", 0); pf_set_put_num("weather.longitude", 0); }   /* re-geocode */
		fetch();
	}
	return NULL;
}
#endif

void pf_weather_init(bool sim)
{
	g_sim = sim;
	(void)g_next_fetch;
#if PF_WITH_CURL
	atomic_store(&g_run, true);
	pthread_create(&g_tid, NULL, worker, NULL);
#endif
}

void pf_weather_shutdown(void)
{
#if PF_WITH_CURL
	atomic_store(&g_run, false);
	pthread_join(g_tid, NULL);
#endif
}

void pf_weather_refresh(void)
{
#if PF_WITH_CURL
	atomic_store(&g_kick, true);
#endif
}

void pf_weather_get(pf_weather *out)
{
	pthread_mutex_lock(&g_mu);
	*out = g_w;
	pthread_mutex_unlock(&g_mu);
	if (!pf_set_bool("weather.enabled", false) || pf_wall() - out->ts > STALE_S) out->valid = false;
}

cJSON *pf_weather_json(void)
{
	pf_weather w;
	pf_weather_get(&w);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "enabled", pf_set_bool("weather.enabled", false));
	cJSON_AddBoolToObject(o, "valid", w.valid);
	char place[64];
	pf_set_str("weather.place", place, sizeof place, "");
	cJSON_AddStringToObject(o, "place", place);
	if (w.ts > 0) {
		cJSON_AddNumberToObject(o, "temp_c", round(w.temp_c * 10) / 10);
		cJSON_AddNumberToObject(o, "wind_kmh", round(w.wind_kmh));
		cJSON_AddNumberToObject(o, "gust_kmh", round(w.gust_kmh));
		cJSON_AddNumberToObject(o, "humidity", round(w.humidity_pct));
		cJSON_AddNumberToObject(o, "age_s", round(pf_wall() - w.ts));
	}
	pthread_mutex_lock(&g_mu);
	cJSON_AddStringToObject(o, "error", g_err);
	pthread_mutex_unlock(&g_mu);
	return o;
}
