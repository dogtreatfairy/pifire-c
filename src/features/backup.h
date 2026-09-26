#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <cJSON.h>

/* One file with the whole picture of this grill -- settings, the tuning library and everything
 * learned, recipes, pellets, notification rules, push subscriptions, and every saved cook -- sent
 * to every backup location the user has set up (Google Drive, OneDrive, a network share, a
 * folder), by hand or on a schedule, and brought back from any of them. */
void   pf_backup_init(const char *data_dir, const char *config_path, bool sim);
void   pf_backup_tick(double now);                        /* services thread: runs the schedule */
int    pf_backup_run(char *err, size_t n);                /* start a backup now, in the background */
cJSON *pf_backup_status_json(void);
cJSON *pf_backup_list(char *err, size_t n);              /* what is at the locations, merged, newest first */
int    pf_backup_test(const char *loc_id, char *msg, size_t n);   /* can that location be reached? 0 = yes */
int    pf_backup_restore_named(const char *name, const char *loc_id, char *err, size_t n); /* from a location ("" = any that has it) */
int    pf_backup_restore_bytes(const void *data, size_t len, char *err, size_t n); /* from an upload */
int    pf_backup_connect(const char *loc_id, char *err, size_t n);   /* begin a cloud sign-in (device code) */
void   pf_backup_disconnect(const char *loc_id);
/* What is at a path: the folders of a directory on the grill, or of a share (or the shares of a
 * host, when no share is named). req: {type, path, host, share, user, password}. */
cJSON *pf_backup_browse(cJSON *req, char *err, size_t n);

/* Called by main() before the database is opened: if a restore was staged, put it in place. */
int    pf_backup_apply_staged(const char *data_dir, const char *config_path);

/* The directory names in an smbclient "ls" listing (strdup'd into names); for the tests. */
int    pf_backup_parse_smb_ls(const char *listing, char **names, int max);
/* The archive itself, for the worker and the tests: writes out_path (a .tar.gz). */
int    pf_backup_make(const char *out_path, char *err, size_t n);
/* Unpacks and checks an archive into a staging directory; 0 when it is a PiFire backup. */
int    pf_backup_stage(const char *archive, const char *stage_dir, char *err, size_t n);
