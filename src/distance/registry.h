#pragma once
#include "pifire/distance.h"

const pf_distance_ops *pf_distance_find(const char *id);   /* "none", "hcsr04", "sim" */
const pf_distance_ops *pf_distance_none(void);
const pf_distance_ops *pf_distance_hcsr04(void);
const pf_distance_ops *pf_distance_sim(void);
