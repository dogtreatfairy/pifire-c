#pragma once
#include "pifire/probe.h"

void pf_probe_drivers_init(const char *plugin_dir);
const pf_probe_ops *pf_probe_driver_find(const char *id);
int pf_probe_driver_count(void);
const pf_probe_ops *pf_probe_driver_at(int i);

/* built-ins */
const pf_probe_ops *pf_probe_sim(void);
const pf_probe_ops *pf_probe_ads1x15(void);
const pf_probe_ops *pf_probe_max31865(void);
const pf_probe_ops *pf_probe_mcp9600(void);
const pf_probe_ops *pf_probe_ds18b20(void);
const pf_probe_ops *pf_probe_virtual(void);
const char *pf_virtual_mode(void *inst);
int pf_virtual_inputs(void *inst, const char **labels, int max);
