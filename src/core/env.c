#include "core/env.h"
#include "core/db.h"
#include "core/log.h"
#include "core/util.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void env_log(int level, const char *tag, const char *fmt, ...)
{
	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	pf_log((pf_log_level)(level & 3), tag, "%s", msg);
}

static int env_kv_get(const pf_env *e, const char *key, char *out, size_t n)
{
	return pf_db_handle() ? pf_db_kv_get(e->ns, key, out, n) : 1;
}

static int env_kv_put(const pf_env *e, const char *key, const char *json)
{
	return pf_db_handle() ? pf_db_kv_put(e->ns, key, json) : -1;
}

void pf_env_init(pf_env *e, const char *ns)
{
	static char namespaces[16][64];
	static int used;
	memset(e, 0, sizeof *e);
	e->log = env_log;
	e->kv_get = env_kv_get;
	e->kv_put = env_kv_put;
	int slot = used < 16 ? used++ : 15;
	pf_strlcpy(namespaces[slot], ns, sizeof namespaces[slot]);
	e->ns = namespaces[slot];
}
