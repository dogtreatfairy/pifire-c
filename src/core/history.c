#include "core/history.h"
#include "core/log.h"
#include <math.h>
#include <pthread.h>
#include <string.h>

#define PENDING_MAX 64        /* 64 * 3 s > 30 s flush interval with margin */
#define CTRL_CAP    3600      /* 60 min @ 1 Hz */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pf_hist_sample g_pending[PENDING_MAX];
static int g_npending;
static double g_last_sample_t;
static unsigned g_dropped;

static pf_hist_pt g_ctrl_buf[CTRL_CAP];
static pf_history g_ctrl = { .pts = g_ctrl_buf, .cap = CTRL_CAP };
static double g_last_ctrl_t;

void pf_history_init(void)
{
	pthread_mutex_lock(&g_mu);
	g_npending = 0;
	g_last_sample_t = 0;
	g_ctrl.head = g_ctrl.len = 0;
	g_last_ctrl_t = 0;
	pthread_mutex_unlock(&g_mu);
}

void pf_history_record(const pf_status *s, double now, double sample_s)
{
	/* Stop is off. There is nothing to plot and nothing being controlled, and on an SD card a row
	 * every few seconds for the days between cooks is wear spent recording that the grill is at
	 * room temperature. Monitor is the mode for watching without running, and it still logs. */
	if (s->mode == PF_MODE_STOP) return;

	/* controller view at 1 Hz (control thread only; no lock needed for the ring itself) */
	if (now - g_last_ctrl_t >= 1.0) {
		g_last_ctrl_t = now;
		pf_hist_pt *pt = &g_ctrl_buf[g_ctrl.head];
		pt->t = now;
		pt->pit_c = (s->sensors.primary >= 0 && s->sensors.p[s->sensors.primary].valid) ? s->sensors.p[s->sensors.primary].temp_c : NAN;
		pt->setpoint_c = s->mode == PF_MODE_HOLD ? s->setpoint_c : 0;
		pt->u_raw = s->u_raw;
		pt->u_applied = s->u_applied;
		pt->ambient_c = s->ambient_c;
		pt->fan_pct = (s->outputs >> PF_OUT_FAN) & 1 ? (s->fan_pct ? s->fan_pct : 100) : 0;
		pt->outputs = s->outputs;
		g_ctrl.head = (g_ctrl.head + 1) % CTRL_CAP;
		if (g_ctrl.len < CTRL_CAP) g_ctrl.len++;
	}

	if (sample_s < 1) sample_s = 1;
	if (now - g_last_sample_t < sample_s) return;
	g_last_sample_t = now;

	pthread_mutex_lock(&g_mu);
	if (g_npending >= PENDING_MAX) { g_dropped++; pthread_mutex_unlock(&g_mu); return; }
	pf_hist_sample *h = &g_pending[g_npending++];
	h->ts = s->wall;
	h->mode = s->mode;
	h->setpoint = s->mode == PF_MODE_HOLD ? s->setpoint_c : 0;
	h->u_raw = s->u_raw;
	h->u_applied = s->u_applied;
	h->fan_pct = (s->outputs >> PF_OUT_FAN) & 1 ? (s->fan_pct ? s->fan_pct : 100) : 0;
	h->outputs = s->outputs;
	h->u_ff = s->u_ff; h->p = s->ctrl_dbg.p; h->i = s->ctrl_dbg.i; h->d = s->ctrl_dbg.d; h->ff = s->ctrl_dbg.ff;
	h->ambient = s->ambient_c; h->cycle_s = s->cycle_s;
	h->flags = (s->lid_open ? 1u : 0u) | (s->s_plus ? 2u : 0u) | (s->pwm_control ? 4u : 0u) | (s->target_reached ? 8u : 0u) | (s->coldstart_active ? 16u : 0u) | (s->saturated < 0 ? 32u : 0u) | (s->saturated > 0 ? 64u : 0u);
	h->pmode = s->pmode;
	h->nprobes = 0;
	for (int i = 0; i < s->sensors.n && h->nprobes < PF_MAX_PROBES; i++) {
		const pf_probe_reading *p = &s->sensors.p[i];
		if (!p->enabled || p->role == PF_PROBE_AUX) continue;
		pf_hist_probe *hp = &h->probes[h->nprobes++];
		memcpy(hp->label, p->label, sizeof hp->label);
		hp->temp = p->valid ? p->temp_c : NAN;
		hp->target = p->target_c;
		hp->raw = p->valid ? p->raw_c : NAN;
		hp->ohms = p->ohms;
	}
	pthread_mutex_unlock(&g_mu);
}

int pf_history_flush(void)
{
	pf_hist_sample batch[PENDING_MAX];
	pthread_mutex_lock(&g_mu);
	int n = g_npending;
	if (n) memcpy(batch, g_pending, sizeof(pf_hist_sample) * (size_t)n);
	g_npending = 0;
	unsigned dropped = g_dropped;
	g_dropped = 0;
	pthread_mutex_unlock(&g_mu);
	if (dropped) LOGW("history", "%u samples dropped (flush too slow)", dropped);
	if (n && pf_db_history_write(batch, n)) return -1;
	return n;
}

const pf_history *pf_history_ctrl_view(void) { return &g_ctrl; }

void pf_history_clear(void)
{
	pthread_mutex_lock(&g_mu);
	g_npending = 0;
	pthread_mutex_unlock(&g_mu);
	g_ctrl.head = g_ctrl.len = 0;
	pf_db_history_clear();
}
