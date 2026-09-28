#pragma once
/* Updates, of PiFire and of the operating system under it.
 *
 * PiFire comes from GitHub Releases of settings.update.repo. On the default branch that is the
 * tagged releases; any other branch is the rolling build CI publishes for that branch's head as the
 * release "branch-<name>". A release archive for this architecture is downloaded, verified against
 * SHA256SUMS and handed to pifire-update-apply (root, via sudo), which reinstalls and restarts.
 *
 * The system is apt, through pifire-system-update (root, via sudo): refresh and list what can be
 * upgraded, then upgrade the packages the person ticked.
 *
 * Everything either job prints goes to one console, kept on disk so the page that shows it can
 * pick the story up again after the daemon has restarted underneath it. */
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>

void   pf_update_init(const char *data_dir, bool sim);
void   pf_update_tick(double now);           /* periodic checks and the install schedule, from the services thread */
/* start a check in the background: PiFire, the system packages, or both; 0 if started */
int    pf_update_check(bool pifire, bool system);
/* install: PiFire (when newer or a branch switch is waiting) and/or the named system packages,
 * system first; 0 if started */
int    pf_update_install_ex(bool pifire, const cJSON *packages, char *err, size_t n);
int    pf_update_install(char *err, size_t n);   /* PiFire only */
cJSON *pf_update_status_json(void);
/* the console: lines after `since` (a sequence number), with the job's id so a reader can tell a
 * new story from the one it was following */
cJSON *pf_update_log_json(unsigned since);
/* where the updater is, for the status: state name and download progress 0..1 */
void   pf_update_stage(char *state, size_t n, double *progress);
/* what the notification rules ask: a PiFire release waiting, which, and how many packages */
void   pf_update_summary(bool *available, char *latest, size_t n, int *system_count);
/* somebody has read the "Updated to X" announcement: it is shown once, on one device */
void   pf_update_installed_seen(void);
const char *pf_update_arch(void);
/* "v1.2.3" vs "1.2.4" -> <0, 0, >0 (exposed for tests) */
int    pf_version_compare(const char *a, const char *b);
/* a branch name as it appears in a release tag: "feature/x" -> "feature-x" (exposed for tests) */
void   pf_update_branch_slug(const char *branch, char *out, size_t n);
/* one line of `apt list --upgradable` -> name, new version, old version; 0 on success (for tests) */
int    pf_update_parse_apt_line(const char *line, char *name, size_t nn, char *to, size_t tn, char *from, size_t fn);
/* is local time (weekday 0 = Sunday, minutes since midnight) inside the install window that
 * starts at `start_min` on the days in `days_mask` (bit 0 = Sunday) and lasts an hour? */
bool   pf_update_in_window(int wday, int minute, int start_min, unsigned days_mask);
