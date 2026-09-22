/* Simulated 4-port probe device reading the sim thermal model: ADC0 = pit, ADC1..3 = food. */
#include "platform/sim.h"
#include "probes/registry.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define NPORTS 4
static const char *port_names[NPORTS] = { "ADC0", "ADC1", "ADC2", "ADC3" };

typedef struct { double noise; unsigned seed; } simp_t;

static void *create(const char *device_json, const pf_env *env)
{
	(void)device_json; (void)env;
	simp_t *s = calloc(1, sizeof *s);
	if (!s) return NULL;
	s->noise = 0.15;
	s->seed = 12345;
	return s;
}
static void destroy(void *self) { free(self); }

static int ports(void *self, const char **names, int max)
{
	(void)self;
	int n = max < NPORTS ? max : NPORTS;
	for (int i = 0; i < n; i++) names[i] = port_names[i];
	return NPORTS;
}

static double noise(simp_t *s)
{
	s->seed = s->seed * 1103515245u + 12345u;
	return ((double)((s->seed >> 8) & 0xffff) / 65535.0 - 0.5) * 2.0 * s->noise;
}

static int read_(void *self, pf_probe_sample *out, int nports)
{
	simp_t *s = self;
	pf_sim_state *m = pf_sim_model();
	for (int i = 0; i < nports; i++) out[i].kind = PF_SAMPLE_INVALID;
	if (!m) return -1;
	if (nports > 0) { out[0].kind = PF_SAMPLE_CELSIUS; out[0].value = m->pit_c + noise(s); }
	for (int i = 1; i < nports && i < NPORTS; i++) { out[i].kind = PF_SAMPLE_CELSIUS; out[i].value = m->food_c[i - 1] + noise(s); }
	return 0;
}

static int status_json(void *self, char *out, size_t n)
{
	(void)self;
	return snprintf(out, n, "{\"connected\":true,\"sim\":true}");
}

static const pf_probe_ops ops = {
	.abi = PF_PROBE_ABI, .id = "sim", .name = "Simulator", .transient = false, .poll_ms = 250,
	.create = create, .destroy = destroy, .ports = ports, .read = read_, .status_json = status_json,
};
const pf_probe_ops *pf_probe_sim(void) { return &ops; }
