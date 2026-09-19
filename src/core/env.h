#pragma once
#include "pifire/common.h"

/* Build a plugin environment whose kv store is namespaced under `ns` (copied). */
void pf_env_init(pf_env *e, const char *ns);
