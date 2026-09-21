#pragma once
#include <stdbool.h>
#include <stddef.h>

double pf_now(void);   /* CLOCK_MONOTONIC seconds */
double pf_wall(void);  /* CLOCK_REALTIME seconds  */
void pf_sleep_ms(unsigned ms);

size_t pf_strlcpy(char *dst, const char *src, size_t n);
/* Escape a string for embedding inside a JSON string literal (no surrounding quotes). */
size_t pf_json_escape(const char *in, char *out, size_t n);

/* Read whole file into a malloc'd NUL-terminated buffer; returns NULL on failure. */
char *pf_read_file(const char *path, size_t *len_out);
/* Write file atomically (tmp + fsync + rename). Returns 0 on success. */
int pf_write_file_atomic(const char *path, const void *data, size_t len);
int pf_mkdir_p(const char *path);
bool pf_file_exists(const char *path);

static inline double pf_clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Run a program (no shell) and capture stdout+stderr. Returns the exit status, -1 on exec
 * failure, -2 on timeout (child killed). `out` may be NULL. */
int pf_run_capture(const char *const argv[], char *out, size_t n, int timeout_s);
/* Reboot or power off the machine (systemctl, detached). The caller stops the grill first. */
void pf_system_power(bool reboot);
