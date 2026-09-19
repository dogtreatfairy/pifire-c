#include "core/cycle.h"
#include "core/util.h"
#include <math.h>

void pf_cycle_begin(pf_cycle *c, const pf_cycle_cfg *cfg, double now, double u_raw)
{
	double u = u_raw;
	if (isnan(u) || isinf(u)) u = cfg->u_min;
	c->saturated = 0;
	if (u < cfg->u_min) { u = cfg->u_min; c->saturated = -1; }
	if (u > cfg->u_max) { u = cfg->u_max; c->saturated = +1; }
	double on = cfg->cycle_s * u;
	if (cfg->max_on_s > 0 && on > cfg->max_on_s) { on = cfg->max_on_s; u = on / cfg->cycle_s; c->saturated = +1; }
	c->start = now;
	c->cycle_s = cfg->cycle_s;
	c->on_s = on;
	c->u_raw = u_raw;
	c->u_applied = u;
	c->active = true;
}

void pf_cycle_begin_fixed(pf_cycle *c, const pf_cycle_cfg *cfg, double now, double on_s, double off_s)
{
	pf_cycle_cfg fixed = { .cycle_s = on_s + off_s, .u_min = 0, .u_max = 1, .max_on_s = cfg->max_on_s };
	pf_cycle_begin(c, &fixed, now, on_s / (on_s + off_s));
}

bool pf_cycle_auger_on(const pf_cycle *c, double now)
{
	return c->active && (now - c->start) < c->on_s;
}

bool pf_cycle_done(const pf_cycle *c, double now)
{
	return !c->active || (now - c->start) >= c->cycle_s;
}

void pf_cycle_stop(pf_cycle *c)
{
	c->active = false;
	c->on_s = 0;
}
