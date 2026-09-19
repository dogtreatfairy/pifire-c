#pragma once
#include "pifire/probe.h"

void pf_probe_drivers_init(const char *plugin_dir);
const pf_probe_ops *pf_probe_driver_find(const char *id);
int pf_probe_driver_count(void);
const pf_probe_ops *pf_probe_driver_at(int i);

/* built-ins */
const pf_probe_ops *pf_probe_sim(void);
