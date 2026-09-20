#pragma once
/* Analysis export of a cook: samples with controller terms, events, settings, learning state. */
#include <cJSON.h>
#include <stddef.h>

cJSON *pf_cooklog_json(double from, double to, const char *name);
/* running cook, else the latest finished cook, else the last 6 hours */
int    pf_cooklog_default_window(double *from, double *to, char *name, size_t n);
int    pf_cooklog_cook_window(int id, double *from, double *to, char *name, size_t n);
