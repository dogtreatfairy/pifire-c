#define _GNU_SOURCE
/* Push notifications to a phone. Two services with first-class iOS apps and a one-call HTTP API:
 *   Pushover  POST https://api.pushover.net/1/messages.json (form: token, user, title, message, priority)
 *   ntfy      POST <server>/<topic> with the message as body and Title / Priority / Tags headers
 * Events are grouped into categories the user can switch per sink: targets (probe target reached,
 * about-N-minutes-to-target, cook timer, recipe steps), alarms (limit alarms, grill errors), pellets
 * (hopper low) and system (autotune, tuning). Delivery runs on a worker thread with a retry. */
#include "features/push.h"
#include "core/events.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "push"

#if PF_WITH_CURL
#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>

#define QLEN 24
typedef struct { char sink[12], code[40], title[96], body[256]; bool force; } item_t;
static item_t g_q[QLEN];
static int g_head, g_len;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_tid;
static atomic_bool g_run;
static char g_last_err[160];

static const char *category(const char *code)
{
	if (!strcmp(code, "Test_Notify")) return "test";
	if (!strcmp(code, "Probe_Temp_Achieved") || !strcmp(code, "Probe_ETA") || !strcmp(code, "Timer_Expired") || !strncmp(code, "Recipe_", 7)) return "targets";
	if (!strcmp(code, "Probe_Temp_Limit_Alarm") || (code[0] == 'E' && code[1] >= '0' && code[1] <= '9')) return "alarms";
	if (!strcmp(code, "Pellet_Level_Low")) return "pellets";
	return "system";
}

static bool wanted(const char *sink, const char *code)
{
	char key[96];
	snprintf(key, sizeof key, "notify.%s.enabled", sink);
	if (!pf_set_bool(key, false)) return false;
	const char *cat = category(code);
	if (!cat || !strcmp(cat, "test")) return true;
	snprintf(key, sizeof key, "notify.%s.%.12s", sink, cat);
	return pf_set_bool(key, strcmp(cat, "system") != 0);
}

static void enqueue(const char *sink, const char *code, const char *title, const char *body, bool force)
{
	pthread_mutex_lock(&g_mu);
	if (g_len < QLEN) {
		item_t *it = &g_q[(g_head + g_len) % QLEN];
		pf_strlcpy(it->sink, sink, sizeof it->sink);
		pf_strlcpy(it->code, code, sizeof it->code);
		pf_strlcpy(it->title, title, sizeof it->title);
		pf_strlcpy(it->body, body, sizeof it->body);
		it->force = force;
		g_len++;
		pthread_cond_signal(&g_cv);
	}
	pthread_mutex_unlock(&g_mu);
}

static void sink(const char *code, const char *title, const char *body, void *ctx)
{
	(void)ctx;
	if (wanted("pushover", code)) enqueue("pushover", code, title, body, false);
	if (wanted("ntfy", code)) enqueue("ntfy", code, title, body, false);
}

static size_t discard(char *p, size_t s, size_t n, void *ud) { (void)p; (void)ud; return s * n; }

/* the grill name leads the title so several grills can share one phone */
static void titled(char *out, size_t n, const char *title)
{
	char grill[64];
	pf_set_str("globals.grill_name", grill, sizeof grill, "PiFire");
	snprintf(out, n, "%.60s: %.90s", grill[0] ? grill : "PiFire", title);
}

static int deliver_pushover(const item_t *it, char *err, size_t errn)
{
	char token[64], user[64], sound[32];
	pf_set_str("notify.pushover.app_token", token, sizeof token, "");
	pf_set_str("notify.pushover.user_key", user, sizeof user, "");
	pf_set_str("notify.pushover.sound", sound, sizeof sound, "");
	if (!token[0] || !user[0]) { snprintf(err, errn, "Pushover: app token and user key are required"); return -1; }
	int prio = pf_set_int("notify.pushover.priority", 0);
	if (!strcmp(category(it->code), "alarms")) prio = pf_set_int("notify.pushover.alarm_priority", 1);
	if (prio < -2) prio = -2;
	if (prio > 2) prio = 2;
	CURL *c = curl_easy_init();
	if (!c) { snprintf(err, errn, "curl init failed"); return -1; }
	char title[160];
	titled(title, sizeof title, it->title);
	char *et = curl_easy_escape(c, token, 0), *eu = curl_easy_escape(c, user, 0), *eti = curl_easy_escape(c, title, 0), *em = curl_easy_escape(c, it->body, 0), *es = curl_easy_escape(c, sound, 0);
	char *form = NULL;
	if (asprintf(&form, "token=%s&user=%s&title=%s&message=%s&priority=%d%s%s%s", et, eu, eti, em, prio,
	             prio == 2 ? "&retry=60&expire=1800" : "", sound[0] ? "&sound=" : "", sound[0] ? es : "") < 0) form = NULL;
	curl_free(et); curl_free(eu); curl_free(eti); curl_free(em); curl_free(es);
	if (!form) { curl_easy_cleanup(c); snprintf(err, errn, "out of memory"); return -1; }
	curl_easy_setopt(c, CURLOPT_URL, "https://api.pushover.net/1/messages.json");
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, form);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "PiFire/" PF_VERSION);
	CURLcode rc = curl_easy_perform(c);
	long http = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
	curl_easy_cleanup(c);
	free(form);
	if (rc != CURLE_OK) { snprintf(err, errn, "Pushover: %s", curl_easy_strerror(rc)); return -1; }
	if (http >= 400) { snprintf(err, errn, "Pushover: HTTP %ld (%s)", http, http == 400 ? "check the app token and user key" : "rejected"); return -1; }
	return 0;
}

static int deliver_ntfy(const item_t *it, char *err, size_t errn)
{
	char server[200], topic[100], token[120];
	pf_set_str("notify.ntfy.server", server, sizeof server, "https://ntfy.sh");
	pf_set_str("notify.ntfy.topic", topic, sizeof topic, "");
	pf_set_str("notify.ntfy.token", token, sizeof token, "");
	if (!topic[0]) { snprintf(err, errn, "ntfy: a topic is required"); return -1; }
	size_t sl = strlen(server);
	while (sl > 0 && server[sl - 1] == '/') server[--sl] = 0;
	const char *cat = category(it->code);
	int prio = !strcmp(cat, "alarms") ? pf_set_int("notify.ntfy.alarm_priority", 5) : pf_set_int("notify.ntfy.priority", 3);
	if (prio < 1) prio = 1;
	if (prio > 5) prio = 5;
	const char *tags = !strcmp(cat, "alarms") ? "rotating_light" : !strcmp(cat, "pellets") ? "package" : !strcmp(cat, "targets") ? "meat_on_bone" : "gear";
	CURL *c = curl_easy_init();
	if (!c) { snprintf(err, errn, "curl init failed"); return -1; }
	char url[320], h1[200], h2[32], h3[64], h4[160], title[160];
	snprintf(url, sizeof url, "%s/%s", server, topic);
	titled(title, sizeof title, it->title);
	snprintf(h1, sizeof h1, "Title: %s", title);
	snprintf(h2, sizeof h2, "Priority: %d", prio);
	snprintf(h3, sizeof h3, "Tags: %s", tags);
	struct curl_slist *h = curl_slist_append(NULL, h1);
	h = curl_slist_append(h, h2);
	h = curl_slist_append(h, h3);
	h = curl_slist_append(h, "Content-Type: text/plain; charset=utf-8");
	if (token[0]) { snprintf(h4, sizeof h4, "Authorization: Bearer %s", token); h = curl_slist_append(h, h4); }
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, it->body);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "PiFire/" PF_VERSION);
	CURLcode rc = curl_easy_perform(c);
	long http = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK) { snprintf(err, errn, "ntfy: %s", curl_easy_strerror(rc)); return -1; }
	if (http >= 400) { snprintf(err, errn, "ntfy: HTTP %ld (%s)", http, http == 401 || http == 403 ? "token rejected" : "rejected"); return -1; }
	return 0;
}

static int deliver(const item_t *it, char *err, size_t errn)
{
	return !strcmp(it->sink, "pushover") ? deliver_pushover(it, err, errn) : deliver_ntfy(it, err, errn);
}

static void *worker(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-push");
	while (atomic_load(&g_run)) {
		pthread_mutex_lock(&g_mu);
		while (atomic_load(&g_run) && g_len == 0) pthread_cond_wait(&g_cv, &g_mu);
		if (!atomic_load(&g_run)) { pthread_mutex_unlock(&g_mu); break; }
		item_t it = g_q[g_head];
		g_head = (g_head + 1) % QLEN;
		g_len--;
		pthread_mutex_unlock(&g_mu);
		char err[160];
		if (deliver(&it, err, sizeof err) == 0) { LOGI(TAG, "%s: sent '%s'", it.sink, it.title); continue; }
		LOGW(TAG, "%s (retrying in 5 s)", err);
		pf_sleep_ms(5000);
		if (deliver(&it, err, sizeof err) != 0) {
			LOGW(TAG, "%s (gave up)", err);
			pthread_mutex_lock(&g_mu);
			pf_strlcpy(g_last_err, err, sizeof g_last_err);
			pthread_mutex_unlock(&g_mu);
		}
	}
	return NULL;
}

void pf_push_init(void)
{
	atomic_store(&g_run, true);
	pthread_create(&g_tid, NULL, worker, NULL);
	pf_events_add_sink(sink, NULL);
}

void pf_push_shutdown(void)
{
	atomic_store(&g_run, false);
	pthread_mutex_lock(&g_mu);
	pthread_cond_broadcast(&g_cv);
	pthread_mutex_unlock(&g_mu);
	pthread_join(g_tid, NULL);
}

int pf_push_test(const char *sink_name, char *err, size_t n)
{
	if (strcmp(sink_name, "pushover") && strcmp(sink_name, "ntfy")) { snprintf(err, n, "unknown sink"); return -1; }
	item_t it = { .force = true };
	pf_strlcpy(it.sink, sink_name, sizeof it.sink);
	pf_strlcpy(it.code, "Test_Notify", sizeof it.code);
	pf_strlcpy(it.title, "Test notification", sizeof it.title);
	pf_strlcpy(it.body, "If you can read this on your phone, PiFire can reach you.", sizeof it.body);
	return deliver(&it, err, n);   /* synchronous so the UI can show the result */
}

#else
void pf_push_init(void) {}
void pf_push_shutdown(void) {}
int  pf_push_test(const char *s, char *err, size_t n) { (void)s; snprintf(err, n, "built without libcurl"); return -1; }
#endif
