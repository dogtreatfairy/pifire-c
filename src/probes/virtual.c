/* Virtual probe device: its single port is computed by probes.c from other probes' readings
 * (average / highest / lowest / median of config.probes_list). The driver itself only carries the
 * configuration; read() always reports "no sample" and probes.c fills the value in afterwards. */
#include "core/settings.h"
#include "core/util.h"
#include "probes/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char mode[16]; char labels[8][PF_LABEL_LEN]; int n; } virt_t;
static const char *port_names[1] = { "VIRT0" };

static void *create(const char *device_json, const pf_env *env)
{
	(void)env;
	cJSON *d = cJSON_Parse(device_json);
	virt_t *v = calloc(1, sizeof *v);
	if (!v) { cJSON_Delete(d); return NULL; }
	pf_strlcpy(v->mode, pf_json_str(d, "config.mode", "average"), sizeof v->mode);
	cJSON *list = pf_json_path(d, "config.probes_list"), *it;
	cJSON_ArrayForEach(it, list) if (cJSON_IsString(it) && v->n < 8) pf_strlcpy(v->labels[v->n++], it->valuestring, PF_LABEL_LEN);
	cJSON_Delete(d);
	return v;
}
static void destroy(void *self) { free(self); }
static int ports(void *self, const char **names, int max) { (void)self; if (max > 0) names[0] = port_names[0]; return 1; }
static int read_(void *self, pf_probe_sample *out, int nports) { (void)self; if (nports > 0) out[0].kind = PF_SAMPLE_INVALID; return 0; }
static int status_json(void *self, char *out, size_t n) { virt_t *v = self; return snprintf(out, n, "{\"connected\":true,\"mode\":\"%s\",\"inputs\":%d}", v->mode, v->n); }

static const pf_probe_ops ops = {
	.abi = PF_PROBE_ABI, .id = "virtual", .name = "Virtual probe", .transient = false, .poll_ms = 250,
	.create = create, .destroy = destroy, .ports = ports, .read = read_, .status_json = status_json,
};
const pf_probe_ops *pf_probe_virtual(void) { return &ops; }

/* used by probes.c */
const char *pf_virtual_mode(void *inst) { return ((virt_t *)inst)->mode; }
int pf_virtual_inputs(void *inst, const char **labels, int max)
{
	virt_t *v = inst;
	int n = v->n < max ? v->n : max;
	for (int i = 0; i < n; i++) labels[i] = v->labels[i];
	return n;
}
