#pragma once
/* Over-the-air updates from GitHub Releases of settings.update.repo: check for a newer tag, download
 * the release archive for this architecture, verify it against SHA256SUMS and hand it to
 * pifire-update-apply (root, via sudo), which reinstalls and restarts the service. */
#include <cJSON.h>
#include <stdbool.h>

void   pf_update_init(const char *data_dir, bool sim);
void   pf_update_tick(double now);           /* periodic auto-check from the services thread */
int    pf_update_check(void);                 /* start a check in the background; 0 if started */
int    pf_update_install(char *err, size_t n);/* start download+verify+apply; 0 if started */
cJSON *pf_update_status_json(void);
const char *pf_update_arch(void);
/* "v1.2.3" vs "1.2.4" -> <0, 0, >0 (exposed for tests) */
int    pf_version_compare(const char *a, const char *b);
