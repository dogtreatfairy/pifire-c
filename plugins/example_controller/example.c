/* Minimal out-of-tree PiFire controller: a bang-bang controller with a dead band.
 *
 *   cc -shared -fPIC -O2 -I/path/to/pifire-c/include -o bangbang.so example.c
 *   sudo install -m 644 bangbang.so /usr/lib/pifire/controllers/
 *   systemctl restart pifired    -> "Bang-bang" appears under Settings > Controller
 *
 * Only the headers under include/pifire/ are needed; there is no link dependency on the daemon. */
#include <pifire/controller.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const pf_env *env; double band_c, u_high, u_low; bool heating; } bb_t;

static double num_opt(const char *json, const char *key, double dflt)
{
	/* tiny JSON number lookup so the example has no cJSON dependency */
	const char *p = json ? strstr(json, key) : NULL;
	if (!p) return dflt;
	p = strchr(p, ':');
	return p ? strtod(p + 1, NULL) : dflt;
}

static void *create(const char *config_json, const pf_env *env)
{
	bb_t *s = calloc(1, sizeof *s);
	s->env = env;
	bool celsius = config_json && strstr(config_json, "\"_units\":\"C\"");
	double band = num_opt(config_json, "\"band\"", 5.0);
	s->band_c = celsius ? band : band * 5.0 / 9.0;
	s->u_high = num_opt(config_json, "\"u_high\"", 0.7);
	s->u_low = num_opt(config_json, "\"u_low\"", 0.15);
	env->log(PF_LVL_INFO, "bangbang", "created: band %.1f C, high %.2f, low %.2f", s->band_c, s->u_high, s->u_low);
	return s;
}
static void destroy(void *self) { free(self); }
static void reset(void *self, const pf_ctrl_in *in) { bb_t *s = self; s->heating = in->pit_c < in->setpoint_c; }
static double update(void *self, const pf_ctrl_in *in, pf_ctrl_dbg *dbg)
{
	bb_t *s = self;
	double e = in->pit_c - in->setpoint_c;
	if (e > s->band_c) s->heating = false;
	if (e < -s->band_c) s->heating = true;
	double u = s->heating ? s->u_high : s->u_low;
	if (dbg) { dbg->error = e; snprintf(dbg->note, sizeof dbg->note, s->heating ? "heating" : "coasting"); }
	return u;
}
static void configure(void *self, const char *config_json)
{
	bb_t *s = self;
	s->u_high = num_opt(config_json, "\"u_high\"", s->u_high);
	s->u_low = num_opt(config_json, "\"u_low\"", s->u_low);
}
static int state_json(void *self, char *out, size_t n) { bb_t *s = self; return snprintf(out, n, "{\"heating\":%s}", s->heating ? "true" : "false"); }

static const pf_controller_ops ops = {
	.abi = PF_CONTROLLER_ABI, .id = "bangbang", .name = "Bang-bang (example plugin)",
	.description = "Example out-of-tree controller: full feed below the band, minimum feed above it.",
	.author = "you",
	.config_schema_json =
		"[{\"option_name\":\"band\",\"option_friendly_name\":\"Dead band\",\"option_description\":\"Half-width of the band around the set point.\",\"option_type\":\"float\",\"option_default\":5,\"option_step\":0.5,\"units\":\"temp_delta\"},"
		"{\"option_name\":\"u_high\",\"option_friendly_name\":\"Feed when heating\",\"option_description\":\"\",\"option_type\":\"float\",\"option_default\":0.7,\"option_step\":0.05},"
		"{\"option_name\":\"u_low\",\"option_friendly_name\":\"Feed when coasting\",\"option_description\":\"\",\"option_type\":\"float\",\"option_default\":0.15,\"option_step\":0.05}]",
	.recommend = { 20, 0.1, 0.9 },
	.create = create, .destroy = destroy, .reset = reset, .update = update, .configure = configure, .state_json = state_json,
};

const pf_controller_ops *pf_controller_export(void) { return &ops; }
