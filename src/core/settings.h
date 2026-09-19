#pragma once
#include "pifire/common.h"
#include <cJSON.h>
#include <stddef.h>

/* Load settings from `path`, filling any missing keys from the embedded defaults.
 * If the file does not exist it is created. Returns 0 on success. */
int pf_settings_init(const char *path);
void pf_settings_shutdown(void);

/* Exclusive access to the live tree. Keep critical sections short; never call save() while locked. */
cJSON *pf_settings_lock(void);
void pf_settings_unlock(void);

/* Write the current tree to disk atomically. */
int pf_settings_save(void);

/* Typed readers by dotted path ("safety.maxtemp"). Thread-safe (lock internally). */
double pf_set_num(const char *path, double dflt);
int    pf_set_int(const char *path, int dflt);
bool   pf_set_bool(const char *path, bool dflt);
/* Copies string into buf; returns false (and copies dflt) if missing. */
bool   pf_set_str(const char *path, char *buf, size_t n, const char *dflt);
/* Deep copy of a subtree (caller frees), or NULL. */
cJSON *pf_set_dup(const char *path);

/* Typed writers. Create intermediate objects as needed. Do not save. */
int pf_set_put_num(const char *path, double v);
int pf_set_put_bool(const char *path, bool v);
int pf_set_put_str(const char *path, const char *v);
int pf_set_put(const char *path, cJSON *v_owned);

/* Apply a JSON patch (deep merge of objects; arrays and scalars replaced), validate, save.
 * `path` may be NULL to merge at root or a dotted path for a subtree. On error fills err. */
int pf_settings_patch(const char *path, const char *json, char *err, size_t errn);

pf_units pf_settings_units(void);
/* Change units, converting every temperature-valued setting in place, and save. */
int pf_settings_set_units(pf_units u);

/* Generation counter incremented on every successful save/patch; threads poll it to reload. */
unsigned pf_settings_generation(void);

/* cJSON helpers usable by other modules: dotted-path lookup within an arbitrary object. */
cJSON *pf_json_path(cJSON *root, const char *path);
double pf_json_num(cJSON *root, const char *path, double dflt);
int    pf_json_int(cJSON *root, const char *path, int dflt);
bool   pf_json_bool(cJSON *root, const char *path, bool dflt);
const char *pf_json_str(cJSON *root, const char *path, const char *dflt);
void   pf_json_merge(cJSON *dst, const cJSON *src); /* deep merge src into dst */
