#include "core/cmdq.h"
#include <pthread.h>
#include <string.h>

#define QLEN 64

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pf_cmd g_q[QLEN];
static int g_head, g_len;

void pf_cmdq_init(void)
{
	pthread_mutex_lock(&g_mu);
	g_head = g_len = 0;
	pthread_mutex_unlock(&g_mu);
}

int pf_cmdq_push(const pf_cmd *c)
{
	pthread_mutex_lock(&g_mu);
	int rc = -1;
	if (g_len < QLEN) {
		g_q[(g_head + g_len) % QLEN] = *c;
		g_len++;
		rc = 0;
	}
	pthread_mutex_unlock(&g_mu);
	return rc;
}

bool pf_cmdq_pop(pf_cmd *out)
{
	pthread_mutex_lock(&g_mu);
	bool ok = g_len > 0;
	if (ok) {
		*out = g_q[g_head];
		g_head = (g_head + 1) % QLEN;
		g_len--;
	}
	pthread_mutex_unlock(&g_mu);
	return ok;
}

int pf_cmd_mode(pf_mode m, double setpoint_user)
{
	pf_cmd c = { .type = PF_CMD_MODE, .mode = m, .num = setpoint_user };
	return pf_cmdq_push(&c);
}

int pf_cmd_simple(pf_cmd_type t)
{
	pf_cmd c = { .type = t };
	return pf_cmdq_push(&c);
}
