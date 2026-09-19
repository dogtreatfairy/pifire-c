#include "core/log.h"
#include "core/util.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define RING_LINES 512
#define LINE_LEN   256

static pf_log_level g_level = PF_LOG_INFO;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_ring[RING_LINES][LINE_LEN];
static double g_ring_ts[RING_LINES];
static int g_ring_lvl[RING_LINES];
static unsigned g_ring_head; /* next write index */
static unsigned g_ring_len;

static const char *level_names[] = { "debug", "info", "warn", "error" };

void pf_log_init(pf_log_level level) { g_level = level; }
void pf_log_set_level(pf_log_level level) { g_level = level; }
pf_log_level pf_log_get_level(void) { return g_level; }
const char *pf_log_level_name(pf_log_level l) { return level_names[l & 3]; }

int pf_log_level_from_name(const char *s)
{
	for (int i = 0; i < 4; i++)
		if (!strcasecmp(s, level_names[i])) return i;
	return -1;
}

void pf_log(pf_log_level level, const char *tag, const char *fmt, ...)
{
	if (level < g_level) return;
	char msg[LINE_LEN - 48];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	double wall = pf_wall();
	time_t t = (time_t)wall;
	struct tm tm;
	localtime_r(&t, &tm);
	char ts[32];
	strftime(ts, sizeof ts, "%H:%M:%S", &tm);

	pthread_mutex_lock(&g_mu);
	fprintf(stderr, "%s.%03d %-5s [%s] %s\n", ts, (int)((wall - (double)t) * 1000), level_names[level & 3], tag, msg);
	snprintf(g_ring[g_ring_head], LINE_LEN, "[%.31s] %s", tag, msg);
	g_ring_ts[g_ring_head] = wall;
	g_ring_lvl[g_ring_head] = level;
	g_ring_head = (g_ring_head + 1) % RING_LINES;
	if (g_ring_len < RING_LINES) g_ring_len++;
	pthread_mutex_unlock(&g_mu);
}

size_t pf_log_recent_json(char *out, size_t n, int max)
{
	if (max <= 0 || max > RING_LINES) max = RING_LINES;
	size_t w = 0;
	w += (size_t)snprintf(out + w, n - w, "[");
	pthread_mutex_lock(&g_mu);
	unsigned count = g_ring_len < (unsigned)max ? g_ring_len : (unsigned)max;
	unsigned start = (g_ring_head + RING_LINES - count) % RING_LINES;
	for (unsigned i = 0; i < count && w < n; i++) {
		unsigned idx = (start + i) % RING_LINES;
		char esc[LINE_LEN * 2];
		pf_json_escape(g_ring[idx], esc, sizeof esc);
		w += (size_t)snprintf(out + w, n - w, "%s{\"ts\":%.3f,\"level\":\"%s\",\"msg\":\"%s\"}",
		                      i ? "," : "", g_ring_ts[idx], level_names[g_ring_lvl[idx] & 3], esc);
	}
	pthread_mutex_unlock(&g_mu);
	if (w < n) w += (size_t)snprintf(out + w, n - w, "]");
	return w < n ? w : n - 1;
}
