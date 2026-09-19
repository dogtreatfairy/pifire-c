#include "core/events.h"
#include "core/db.h"
#include "core/log.h"
#include "core/util.h"
#include "pifire/common.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RING 32
#define MAX_SINKS 8

typedef struct { double ts; char code[40]; char title[96]; char body[256]; } ev_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static ev_t g_ring[RING];
static unsigned g_head, g_len, g_gen;
static struct { pf_event_sink_fn fn; void *ctx; } g_sinks[MAX_SINKS];
static int g_nsinks;

void pf_events_init(void)
{
	pthread_mutex_lock(&g_mu);
	g_head = g_len = 0;
	g_nsinks = 0;
	pthread_mutex_unlock(&g_mu);
}

int pf_events_add_sink(pf_event_sink_fn fn, void *ctx)
{
	pthread_mutex_lock(&g_mu);
	int rc = -1;
	if (g_nsinks < MAX_SINKS) { g_sinks[g_nsinks].fn = fn; g_sinks[g_nsinks].ctx = ctx; g_nsinks++; rc = 0; }
	pthread_mutex_unlock(&g_mu);
	return rc;
}

void pf_events_emit(const char *code, const char *title, const char *fmt, ...)
{
	ev_t e;
	e.ts = pf_wall();
	pf_strlcpy(e.code, code, sizeof e.code);
	pf_strlcpy(e.title, title, sizeof e.title);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(e.body, sizeof e.body, fmt, ap);
	va_end(ap);

	int level = strncmp(code, "E", 1) == 0 && code[1] >= '0' && code[1] <= '9' ? PF_LVL_ERROR :
	            strncmp(code, "W", 1) == 0 && code[1] >= '0' && code[1] <= '9' ? PF_LVL_WARN : PF_LVL_INFO;
	pf_log((pf_log_level)level, "event", "%s: %s - %s", code, title, e.body);
	if (pf_db_handle()) {
		char msg[360];
		snprintf(msg, sizeof msg, "%s%s%s", e.title, e.body[0] ? " - " : "", e.body);
		pf_db_event(level, code, msg);
	}

	struct { pf_event_sink_fn fn; void *ctx; } sinks[MAX_SINKS];
	int n;
	pthread_mutex_lock(&g_mu);
	g_ring[g_head] = e;
	g_head = (g_head + 1) % RING;
	if (g_len < RING) g_len++;
	g_gen++;
	n = g_nsinks;
	memcpy(sinks, g_sinks, sizeof sinks);
	pthread_mutex_unlock(&g_mu);
	for (int i = 0; i < n; i++) sinks[i].fn(e.code, e.title, e.body, sinks[i].ctx);
}

cJSON *pf_events_recent_json(int max)
{
	cJSON *arr = cJSON_CreateArray();
	pthread_mutex_lock(&g_mu);
	unsigned count = g_len < (unsigned)max ? g_len : (unsigned)max;
	unsigned start = (g_head + RING - count) % RING;
	for (unsigned i = 0; i < count; i++) {
		const ev_t *e = &g_ring[(start + i) % RING];
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "ts", e->ts);
		cJSON_AddStringToObject(o, "code", e->code);
		cJSON_AddStringToObject(o, "title", e->title);
		cJSON_AddStringToObject(o, "body", e->body);
		cJSON_AddItemToArray(arr, o);
	}
	pthread_mutex_unlock(&g_mu);
	return arr;
}

unsigned pf_events_generation(void) { return __atomic_load_n(&g_gen, __ATOMIC_RELAXED); }
