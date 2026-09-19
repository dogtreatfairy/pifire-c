#pragma once
#include <stddef.h>

typedef struct { const char *name; const unsigned char *data; size_t len; } pf_embedded_file;

/* Files from share/ embedded at build time (settings.default.json, manifest.json, ...). */
const pf_embedded_file *pf_embedded_share(const char *name);
