#pragma once
#include <stddef.h>

typedef enum { PF_LOG_DEBUG = 0, PF_LOG_INFO, PF_LOG_WARN, PF_LOG_ERROR } pf_log_level;

void pf_log_init(pf_log_level level);
void pf_log_set_level(pf_log_level level);
pf_log_level pf_log_get_level(void);
int pf_log_level_from_name(const char *s); /* -1 if unknown */
const char *pf_log_level_name(pf_log_level l);

void pf_log(pf_log_level level, const char *tag, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/* Copy up to `max` most recent lines (newest last) into a JSON array string. Returns bytes written. */
size_t pf_log_recent_json(char *out, size_t n, int max);

#define LOGD(tag, ...) pf_log(PF_LOG_DEBUG, tag, __VA_ARGS__)
#define LOGI(tag, ...) pf_log(PF_LOG_INFO, tag, __VA_ARGS__)
#define LOGW(tag, ...) pf_log(PF_LOG_WARN, tag, __VA_ARGS__)
#define LOGE(tag, ...) pf_log(PF_LOG_ERROR, tag, __VA_ARGS__)
