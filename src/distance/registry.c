#include "distance/registry.h"
#include <string.h>

const pf_distance_ops *pf_distance_find(const char *id)
{
	if (!id) return pf_distance_none();
	if (!strcmp(id, "hcsr04")) return pf_distance_hcsr04();
	if (!strcmp(id, "vl53l0x")) return pf_distance_vl53l0x();
	if (!strcmp(id, "sim") || !strcmp(id, "prototype")) return pf_distance_sim();
	return pf_distance_none();
}

/* ---- none ---- */
static void *none_create(const char *cfg, const pf_env *env) { (void)cfg; (void)env; return (void *)1; }
static void none_destroy(void *self) { (void)self; }
static double none_read(void *self) { (void)self; return -1; }
static const pf_distance_ops none_ops = { .abi = PF_DISTANCE_ABI, .id = "none", .name = "None", .create = none_create, .destroy = none_destroy, .read_cm = none_read };
const pf_distance_ops *pf_distance_none(void) { return &none_ops; }

/* ---- sim: hopper drains slowly ---- */
static double g_sim_cm = 6.0;
static void *sim_create(const char *cfg, const pf_env *env) { (void)cfg; (void)env; g_sim_cm = 6.0; return (void *)1; }
static double sim_read(void *self) { (void)self; g_sim_cm += 0.05; if (g_sim_cm > 22) g_sim_cm = 6.0; return g_sim_cm; }
static const pf_distance_ops sim_ops = { .abi = PF_DISTANCE_ABI, .id = "sim", .name = "Simulator", .create = sim_create, .destroy = none_destroy, .read_cm = sim_read };
const pf_distance_ops *pf_distance_sim(void) { return &sim_ops; }
