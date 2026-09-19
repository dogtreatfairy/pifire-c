#define _GNU_SOURCE
#include "features/webhook.h"
#include "core/events.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "webhook"

#if PF_WITH_CURL
#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>

#define QLEN 16
typedef struct { char code[40], title[96], body[256]; double ts; } item_t;
static item_t g_q[QLEN];
static int g_head, g_len;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_tid;
static atomic_bool g_run;

static bool wanted(const char *code)
{
	if (!pf_set_bool("notify.webhook.enabled", false)) return false;
	cJSON *ev = pf_set_dup("notify.webhook.events");
	bool ok = true;
	if (cJSON_IsArray(ev) && cJSON_GetArraySize(ev) > 0) {
		ok = false;
		cJSON *it;
		cJSON_ArrayForEach(it, ev) if (cJSON_IsString(it) && (!strcmp(it->valuestring, "*") || !strcmp(it->valuestring, code))) { ok = true; break; }
	}
	cJSON_Delete(ev);
	return ok;
}

static void sink(const char *code, const char *title, const char *body, void *ctx)
{
	(void)ctx;
	if (!wanted(code)) return;
	pthread_mutex_lock(&g_mu);
	if (g_len < QLEN) {
		item_t *it = &g_q[(g_head + g_len) % QLEN];
		pf_strlcpy(it->code, code, sizeof it->code);
		pf_strlcpy(it->title, title, sizeof it->title);
		pf_strlcpy(it->body, body, sizeof it->body);
		it->ts = pf_wall();
		g_len++;
		pthread_cond_signal(&g_cv);
	}
	pthread_mutex_unlock(&g_mu);
}

static size_t discard(char *p, size_t s, size_t n, void *ud) { (void)p; (void)ud; return s * n; }

static int post(const char *url, const char *json)
{
	CURL *c = curl_easy_init();
	if (!c) return -1;
	struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, json);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "PiFire/" PF_VERSION);
	CURLcode rc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK) { LOGW(TAG, "%s: %s", url, curl_easy_strerror(rc)); return -1; }
	if (code >= 400) { LOGW(TAG, "%s: HTTP %ld", url, code); return -1; }
	return 0;
}

static void *worker(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-webhook");
	while (atomic_load(&g_run)) {
		pthread_mutex_lock(&g_mu);
		while (atomic_load(&g_run) && g_len == 0) pthread_cond_wait(&g_cv, &g_mu);
		if (!atomic_load(&g_run)) { pthread_mutex_unlock(&g_mu); break; }
		item_t it = g_q[g_head];
		g_head = (g_head + 1) % QLEN;
		g_len--;
		pthread_mutex_unlock(&g_mu);

		char url[512], grill[64];
		pf_set_str("notify.webhook.url", url, sizeof url, "");
		pf_set_str("globals.grill_name", grill, sizeof grill, "PiFire");
		if (!url[0]) continue;
		pf_status st;
		pf_status_get(&st);
		cJSON *j = cJSON_CreateObject();
		cJSON_AddStringToObject(j, "event", it.code);
		cJSON_AddStringToObject(j, "title", it.title);
		cJSON_AddStringToObject(j, "body", it.body);
		cJSON_AddNumberToObject(j, "ts", it.ts);
		cJSON_AddStringToObject(j, "grill", grill[0] ? grill : "PiFire");
		/* IFTTT-style convenience values */
		cJSON_AddStringToObject(j, "value1", it.title);
		cJSON_AddStringToObject(j, "value2", it.body);
		cJSON_AddStringToObject(j, "value3", grill[0] ? grill : "PiFire");
		cJSON_AddItemToObject(j, "status", pf_status_to_json(&st, pf_settings_units()));
		char *txt = cJSON_PrintUnformatted(j);
		cJSON_Delete(j);
		if (!txt) continue;
		if (post(url, txt)) { pf_sleep_ms(3000); post(url, txt); }
		free(txt);
	}
	return NULL;
}

void pf_webhook_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	atomic_store(&g_run, true);
	pthread_create(&g_tid, NULL, worker, NULL);
	pf_events_add_sink(sink, NULL);
}

void pf_webhook_shutdown(void)
{
	atomic_store(&g_run, false);
	pthread_mutex_lock(&g_mu);
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
	pthread_join(g_tid, NULL);
	curl_global_cleanup();
}
#else
void pf_webhook_init(void) { if (pf_set_bool("notify.webhook.enabled", false)) LOGW(TAG, "webhook requested but this build has no libcurl support"); }
void pf_webhook_shutdown(void) {}
#endif
