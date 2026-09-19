#pragma once
#include "pifire/controller.h"

/* Register built-ins and scan `plugin_dir` (may be NULL) for *.so exporting pf_controller_export. */
void pf_controllers_init(const char *plugin_dir);
const pf_controller_ops *pf_controller_find(const char *id);
int pf_controller_count(void);
const pf_controller_ops *pf_controller_at(int i);
