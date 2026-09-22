#define _GNU_SOURCE
#include "core/outputs.h"
#include "core/log.h"
#include "core/util.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define TAG "outputs"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static const pf_platform_ops *g_ops;
static void *g_inst;
static bool g_state[PF_OUT_COUNT];
static int g_fan_pct;
static atomic_bool g_latched;

int pf_outputs_init(const pf_platform_ops *ops, void *inst)
{
	pthread_mutex_lock(&g_mu);
	g_ops = ops;
	g_inst = inst;
	memset(g_state, 0, sizeof g_state);
	g_fan_pct = 0;
	if (g_ops && g_inst) g_ops->all_off(g_inst);
	pthread_mutex_unlock(&g_mu);
	return 0;
}

void pf_outputs_shutdown(void)
{
	pthread_mutex_lock(&g_mu);
	if (g_ops && g_inst) { g_ops->all_off(g_inst); g_ops->destroy(g_inst); }
	g_ops = NULL;
	g_inst = NULL;
	pthread_mutex_unlock(&g_mu);
}

int pf_outputs_set(pf_output o, bool on)
{
	if (atomic_load(&g_latched)) return -EPERM;
	pthread_mutex_lock(&g_mu);
	int rc = -ENODEV;
	if (g_ops && g_inst) {
		if (g_state[o] != on) {
			rc = g_ops->set_output(g_inst, o, on);
			if (rc == 0) { g_state[o] = on; LOGD(TAG, "%s %s", pf_output_name(o), on ? "ON" : "OFF"); }
			if (o == PF_OUT_FAN && !on) g_fan_pct = 0;
		} else rc = 0;
	}
	pthread_mutex_unlock(&g_mu);
	return rc;
}

int pf_outputs_fan_pct(int pct)
{
	if (atomic_load(&g_latched)) return -EPERM;
	pthread_mutex_lock(&g_mu);
	int rc = -ENODEV;
	if (g_ops && g_inst) {
		rc = g_ops->set_fan_pct(g_inst, pct);
		if (rc == 0 && g_fan_pct != pct) { g_fan_pct = pct; LOGD(TAG, "fan %d%%", pct); }
	}
	pthread_mutex_unlock(&g_mu);
	return rc;
}

int pf_outputs_pwm_frequency(int hz)
{
	pthread_mutex_lock(&g_mu);
	int rc = (g_ops && g_inst) ? g_ops->set_pwm_frequency(g_inst, hz) : -ENODEV;
	pthread_mutex_unlock(&g_mu);
	return rc;
}

void pf_outputs_all_off(void)
{
	pthread_mutex_lock(&g_mu);
	if (g_ops && g_inst) g_ops->all_off(g_inst);
	memset(g_state, 0, sizeof g_state);
	g_fan_pct = 0;
	pthread_mutex_unlock(&g_mu);
}

bool pf_outputs_get(pf_output o) { return g_state[o]; }
int pf_outputs_get_fan_pct(void) { return g_fan_pct; }

unsigned pf_outputs_mask(void)
{
	unsigned m = 0;
	for (int i = 0; i < PF_OUT_COUNT; i++) if (g_state[i]) m |= 1u << i;
	return m;
}

bool pf_outputs_read_input(pf_input in)
{
	pthread_mutex_lock(&g_mu);
	bool v = (g_ops && g_inst) ? g_ops->read_input(g_inst, in) : false;
	pthread_mutex_unlock(&g_mu);
	return v;
}

int pf_outputs_platform_status(char *out, size_t n)
{
	pthread_mutex_lock(&g_mu);
	int rc = (g_ops && g_inst && g_ops->status_json) ? g_ops->status_json(g_inst, out, n) : (int)pf_strlcpy(out, "{}", n);
	pthread_mutex_unlock(&g_mu);
	return rc;
}

int pf_outputs_emergency_off(int timeout_ms)
{
	atomic_store(&g_latched, true);
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
	if (pthread_mutex_timedlock(&g_mu, &deadline) != 0) {
		/* The lock is held by a thread that is not coming back, which is why we are here. Giving up
		 * at this point means aborting with the auger possibly still turning and a fire being fed,
		 * so drive the outputs anyway. Writing them without the lock risks racing a call already in
		 * flight on the wedged thread; leaving a grill feeding itself does not risk anything, it
		 * simply happens. The latch is already set, so nothing else of ours can turn them back on. */
		LOGE(TAG, "emergency off: HAL busy, driving outputs off without the lock");
		if (g_ops && g_inst) g_ops->all_off(g_inst);
		memset(g_state, 0, sizeof g_state);
		g_fan_pct = 0;
		return -EBUSY;
	}
	if (g_ops && g_inst) g_ops->all_off(g_inst);
	memset(g_state, 0, sizeof g_state);
	g_fan_pct = 0;
	pthread_mutex_unlock(&g_mu);
	LOGE(TAG, "emergency off: all outputs driven OFF, outputs latched");
	return 0;
}

bool pf_outputs_latched(void) { return atomic_load(&g_latched); }
