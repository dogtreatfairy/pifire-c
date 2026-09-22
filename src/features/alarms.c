#define _GNU_SOURCE
/* The alarm state table. See alarms.h for what an alarm is and why a notice is not one. */
#include "features/alarms.h"
#include "core/log.h"
#include "core/util.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define TAG "alarms"

/* Chatter suppression. An alarm on the boundary of its threshold can raise and clear every few
 * seconds, and a phone that buzzes twenty times in ten minutes teaches its owner to ignore it,
 * which is the one failure an alarm system cannot recover from. Past this many activations in the
 * window, it is shelved: still listed, still visible, but silent for a while. */
#define CHATTER_RAISES 5
#define CHATTER_WINDOW_S 600.0
#define CHATTER_SHELF_S 1800.0

typedef struct {
	bool used;
	char key[96], code[40], name[64];
	char title[120], body[256];
	int crit;
	unsigned sinks;
	bool active;      /* the condition is true right now */
	bool acked;       /* somebody has seen it */
	bool notice;      /* a moment, not a condition: it has no return to normal */
	/* Off the list, but not forgotten. Chatter is counted across activations, so the record of how
	 * often this condition has come and gone has to outlive the clearing that ends each one --
	 * wiping the slot was what let a condition flapping every other second stay silent about it. */
	bool retired;
	double raised_ts, cleared_ts, acked_ts;
	unsigned raises;
	double window_start;
	unsigned window_raises;
	double shelved_until;
} alarm_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static alarm_t g_tab[PF_ALARMS_MAX];
static unsigned g_gen;

void pf_alarms_init(void)
{
	pthread_mutex_lock(&g_mu);
	memset(g_tab, 0, sizeof g_tab);
	g_gen++;
	pthread_mutex_unlock(&g_mu);
}

/* caller holds the lock */
static alarm_t *find(const char *key)
{
	for (int i = 0; i < PF_ALARMS_MAX; i++)
		if (g_tab[i].used && !strcmp(g_tab[i].key, key)) return &g_tab[i];
	return NULL;
}

/* caller holds the lock: forget a retired entry once it can no longer tell us anything about
 * chattering, so the table does not fill with the history of conditions that behaved. */
static void reap(double now)
{
	for (int i = 0; i < PF_ALARMS_MAX; i++)
		if (g_tab[i].used && g_tab[i].retired && now - g_tab[i].cleared_ts > CHATTER_WINDOW_S * 2 &&
		    g_tab[i].shelved_until < now)
			memset(&g_tab[i], 0, sizeof g_tab[i]);
}

/* caller holds the lock. Evicts the oldest acknowledged entry when full, never an unacknowledged
 * one: a table that drops what nobody has read yet is worse than one that refuses to grow. */
static alarm_t *slot_for(const char *key)
{
	alarm_t *a = find(key);
	if (a) return a;
	for (int i = 0; i < PF_ALARMS_MAX; i++)
		if (!g_tab[i].used) return &g_tab[i];
	alarm_t *oldest = NULL;
	for (int i = 0; i < PF_ALARMS_MAX; i++) {
		if (g_tab[i].retired) return &g_tab[i];   /* history first: nobody is waiting on it */
		if (g_tab[i].active || !g_tab[i].acked) continue;
		if (!oldest || g_tab[i].raised_ts < oldest->raised_ts) oldest = &g_tab[i];
	}
	if (!oldest) LOGW(TAG, "alarm table full and nothing acknowledged to drop; '%s' ignored", key);
	return oldest;
}

bool pf_alarms_raise(const char *key, const char *code, const char *name, int crit, unsigned sinks,
                     const char *title, const char *body)
{
	double now = pf_wall();
	bool is_new = false, chattering = false;
	pthread_mutex_lock(&g_mu);
	reap(now);
	alarm_t *a = slot_for(key);
	if (!a) { pthread_mutex_unlock(&g_mu); return false; }

	if (!a->used) {
		memset(a, 0, sizeof *a);
		a->used = true;
		pf_strlcpy(a->key, key, sizeof a->key);
		a->window_start = now;
	}
	pf_strlcpy(a->code, code, sizeof a->code);
	pf_strlcpy(a->name, name ? name : code, sizeof a->name);
	/* The message is refreshed even when the condition was already standing, so a list read an
	 * hour later shows what the grill is doing now rather than what it was doing when it started. */
	pf_strlcpy(a->title, title, sizeof a->title);
	pf_strlcpy(a->body, body ? body : "", sizeof a->body);
	a->crit = crit;
	a->sinks = sinks;
	a->notice = false;
	a->retired = false;

	if (!a->active) {
		is_new = true;
		a->active = true;
		a->acked = false;
		a->raised_ts = now;
		a->cleared_ts = 0;
		a->raises++;
		if (now - a->window_start > CHATTER_WINDOW_S) { a->window_start = now; a->window_raises = 0; }
		a->window_raises++;
		if (a->window_raises >= CHATTER_RAISES && a->shelved_until < now) {
			a->shelved_until = now + CHATTER_SHELF_S;
			chattering = true;
		}
	}
	if (a->shelved_until > now) is_new = false;   /* listed, but it has said enough for now */
	g_gen++;
	pthread_mutex_unlock(&g_mu);

	if (chattering)
		LOGW(TAG, "'%s' has come and gone %u times in %.0f minutes; silencing it for %.0f minutes",
		     key, (unsigned)CHATTER_RAISES, CHATTER_WINDOW_S / 60, CHATTER_SHELF_S / 60);
	return is_new;
}

void pf_alarms_clear(const char *key)
{
	pthread_mutex_lock(&g_mu);
	alarm_t *a = find(key);
	if (a && a->active) {
		a->active = false;
		a->cleared_ts = pf_wall();
		/* Fixing something is not the same as having seen that it broke. A warning or worse that
		 * nobody acknowledged stays on the list, marked as over, so the record of it survives the
		 * repair. Everything else leaves the list at once -- which is what "the probe is back"
		 * should mean -- while the slot is kept a while longer so its activations keep counting. */
		if (a->acked || a->crit < PF_CRIT_HIGH) { a->retired = true; a->acked = true; }
		g_gen++;
	}
	pthread_mutex_unlock(&g_mu);
}

void pf_alarms_note(const char *code, const char *name, int crit, unsigned sinks,
                    const char *title, const char *body)
{
	double now = pf_wall();
	/* A moment is its own occurrence: two tuning runs finishing are two entries, so the key
	 * carries the time rather than only the code. */
	char key[96];
	snprintf(key, sizeof key, "%.40s@%.0f", code, now);
	pthread_mutex_lock(&g_mu);
	alarm_t *a = slot_for(key);
	if (a) {
		memset(a, 0, sizeof *a);
		a->used = true;
		a->notice = true;
		a->active = false;
		a->acked = false;
		a->raised_ts = now;
		a->crit = crit;
		a->sinks = sinks;
		a->raises = 1;
		pf_strlcpy(a->key, key, sizeof a->key);
		pf_strlcpy(a->code, code, sizeof a->code);
		pf_strlcpy(a->name, name ? name : code, sizeof a->name);
		pf_strlcpy(a->title, title, sizeof a->title);
		pf_strlcpy(a->body, body ? body : "", sizeof a->body);
		g_gen++;
	}
	pthread_mutex_unlock(&g_mu);
}

int pf_alarms_ack(const char *key)
{
	int rc = -1;
	pthread_mutex_lock(&g_mu);
	alarm_t *a = find(key);
	if (a) {
		a->acked = true;
		a->acked_ts = pf_wall();
		/* Still standing: it stays until the grill says otherwise. Over, or never a condition in
		 * the first place: acknowledging it is the end of it. */
		if (!a->active) { a->retired = true; if (a->cleared_ts == 0) a->cleared_ts = a->acked_ts; }
		g_gen++;
		rc = 0;
	}
	pthread_mutex_unlock(&g_mu);
	return rc;
}

int pf_alarms_ack_all(void)
{
	int n = 0;
	double now = pf_wall();
	pthread_mutex_lock(&g_mu);
	for (int i = 0; i < PF_ALARMS_MAX; i++) {
		if (!g_tab[i].used || g_tab[i].retired) continue;
		if (!g_tab[i].acked) n++;
		g_tab[i].acked = true;
		g_tab[i].acked_ts = now;
		if (!g_tab[i].active) { g_tab[i].retired = true; if (g_tab[i].cleared_ts == 0) g_tab[i].cleared_ts = now; }
	}
	g_gen++;
	pthread_mutex_unlock(&g_mu);
	return n;
}

int pf_alarms_shelve(const char *key, double seconds)
{
	int rc = -1;
	pthread_mutex_lock(&g_mu);
	alarm_t *a = find(key);
	if (a) {
		a->shelved_until = pf_wall() + (seconds > 0 ? seconds : CHATTER_SHELF_S);
		a->acked = true;
		a->acked_ts = pf_wall();
		g_gen++;
		rc = 0;
	}
	pthread_mutex_unlock(&g_mu);
	return rc;
}

int pf_alarms_unacked(void)
{
	int n = 0;
	pthread_mutex_lock(&g_mu);
	for (int i = 0; i < PF_ALARMS_MAX; i++) if (g_tab[i].used && !g_tab[i].retired && !g_tab[i].acked) n++;
	pthread_mutex_unlock(&g_mu);
	return n;
}

cJSON *pf_alarms_json(void)
{
	double now = pf_wall();
	cJSON *o = cJSON_CreateObject();
	cJSON *arr = cJSON_AddArrayToObject(o, "alarms");
	int unacked = 0, active = 0, worst = -1;
	pthread_mutex_lock(&g_mu);
	for (int i = 0; i < PF_ALARMS_MAX; i++) {
		const alarm_t *a = &g_tab[i];
		if (!a->used || a->retired) continue;
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "key", a->key);
		cJSON_AddStringToObject(e, "code", a->code);
		cJSON_AddStringToObject(e, "name", a->name);
		cJSON_AddStringToObject(e, "title", a->title);
		cJSON_AddStringToObject(e, "body", a->body);
		cJSON_AddNumberToObject(e, "crit", a->crit);
		cJSON_AddBoolToObject(e, "active", a->active);
		cJSON_AddBoolToObject(e, "acked", a->acked);
		cJSON_AddBoolToObject(e, "notice", a->notice);
		cJSON_AddNumberToObject(e, "ts", a->raised_ts);
		if (a->cleared_ts > 0) cJSON_AddNumberToObject(e, "cleared_ts", a->cleared_ts);
		if (a->raises > 1) cJSON_AddNumberToObject(e, "raises", a->raises);
		if (a->shelved_until > now) cJSON_AddNumberToObject(e, "shelved_for", round(a->shelved_until - now));
		cJSON_AddItemToArray(arr, e);
		if (!a->acked) unacked++;
		if (a->active) active++;
		if (!a->acked && a->crit > worst) worst = a->crit;
	}
	pthread_mutex_unlock(&g_mu);
	cJSON_AddNumberToObject(o, "unacked", unacked);
	cJSON_AddNumberToObject(o, "active", active);
	cJSON_AddNumberToObject(o, "worst", worst);
	return o;
}

unsigned pf_alarms_generation(void) { return __atomic_load_n(&g_gen, __ATOMIC_RELAXED); }
