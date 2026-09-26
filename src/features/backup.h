#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <cJSON.h>

/* One file with the whole picture of this grill -- settings, the tuning library and everything
 * learned, recipes, pellets, notification rules, push subscriptions, and every saved cook -- sent
 * to Google Drive, a network share or a folder, by hand or on a schedule, and brought back from
 * any of them. */
void   pf_backup_init(const char *data_dir, const char *config_path, bool sim);
void   pf_backup_tick(double now);                        /* services thread: runs the schedule */
int    pf_backup_run(char *err, size_t n);                /* start a backup now, in the background */
cJSON *pf_backup_status_json(void);
cJSON *pf_backup_list(char *err, size_t n);              /* what is at the destination, newest first */
int    pf_backup_test(char *msg, size_t n);              /* can the destination be reached? 0 = yes */
int    pf_backup_restore_named(const char *name, char *err, size_t n);          /* from the destination */
int    pf_backup_restore_bytes(const void *data, size_t len, char *err, size_t n); /* from an upload */
int    pf_backup_gdrive_connect(char *err, size_t n);     /* begin Google's device sign-in */
void   pf_backup_gdrive_disconnect(void);

/* Called by main() before the database is opened: if a restore was staged, put it in place. */
int    pf_backup_apply_staged(const char *data_dir, const char *config_path);

/* The archive itself, for the worker and the tests: writes out_path (a .tar.gz). */
int    pf_backup_make(const char *out_path, char *err, size_t n);
/* Unpacks and checks an archive into a staging directory; 0 when it is a PiFire backup. */
int    pf_backup_stage(const char *archive, const char *stage_dir, char *err, size_t n);
