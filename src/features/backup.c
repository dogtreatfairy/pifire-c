/* One file, the whole picture, to every place it is wanted.
 *
 * A grill accumulates things worth keeping: the settings that make it this grill, the tuning
 * library and the feed-forward it has learned over months of cooks, the recipes, the pellet
 * profiles, the notification rules, every saved cook. All of it lives on an SD card in the weather.
 * This module puts the lot in one tar.gz and sends it to each backup LOCATION the user has set up
 * -- Google Drive, OneDrive, a share on the network, a folder such as a USB stick -- on a
 * schedule, and can bring it back from any of them. Several locations, as Home Assistant has its
 * backup agents: a copy in the house and a copy in the cloud is the point of a backup.
 *
 * What goes in: settings.json; a snapshot of the database with the rolling chart history left
 * out (it is the last two days of the live chart, regenerated as the grill runs, and it is most
 * of the file's size -- the cooks themselves are kept as cook files, which go in whole); the cook
 * files; and a manifest saying what made it. What does not: the resume snapshot, the update
 * stage, the running marker -- state of the process, not of the grill.
 *
 * A restore is staged, checked, and applied by the next start of the daemon before it opens the
 * database, because swapping the database under a running process is not a thing to do. The grill
 * has to be stopped, and the daemon restarts itself the way an update does. */
#include "features/backup.h"
#include "core/db.h"
#include "core/embedded.h"
#include "core/events.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/util.h"
#include "pifire/common.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if PF_WITH_CURL
#include <curl/curl.h>
#endif

#define TAG "backup"
#define MAX_LOC 8

#define GDRIVE_SCOPE "https://www.googleapis.com/auth/drive.file"
#define GDRIVE_DEVICE_URL "https://oauth2.googleapis.com/device/code"
#define GDRIVE_TOKEN_URL "https://oauth2.googleapis.com/token"
#define GDRIVE_API "https://www.googleapis.com/drive/v3/files"
#define GDRIVE_UPLOAD "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable"

/* OneDrive through Microsoft Graph, in the app's own folder (Apps/<app name>): the equivalent of
 * Google's drive.file -- PiFire sees what it made and nothing else. A public client: device
 * sign-in needs no secret. */
#define MS_SCOPE "Files.ReadWrite.AppFolder offline_access"
#define MS_DEVICE_URL "https://login.microsoftonline.com/common/oauth2/v2.0/devicecode"
#define MS_TOKEN_URL "https://login.microsoftonline.com/common/oauth2/v2.0/token"
#define MS_GRAPH "https://graph.microsoft.com/v1.0"

/* A location: where a copy goes. Read from settings each time it is needed, so an edit on the
 * page is what the next run uses. */
typedef struct {
	char id[24], type[12], name[48];
	bool enabled;
	char host[128], share[128], path[256], user[96], pass[128];   /* smb */
	char folder[256];                                             /* folder */
	char client_id[256], client_secret[256], cloud_folder[96];    /* gdrive, onedrive */
} loc_t;

static struct {
	char data_dir[512], config[512], work[560];
	bool sim;
	pthread_mutex_t mu;
	bool busy;                 /* a backup or restore is running in the worker */
	char message[240];         /* what the worker is doing or last said */
	bool msg_err;
	/* the last backup, as filed in the database */
	double last_ts; char last_name[128]; long last_size; bool last_ok; char last_results[1024];
	double last_good_ts;       /* the last one that got everywhere: what the schedule counts from */
	int fail_streak;           /* failures since the last success; the first of a streak is announced, the rest logged */
	bool have_smb;             /* smbclient is installed (checked at start, on a test, and again while missing) */
	double last_smb_check;
	double last_check;         /* schedule tick throttle */
	/* a cloud sign-in, while it is going on */
	struct { bool pending; char loc[24], type[12], user_code[32], url[160], device_code[1200]; double expires, interval; } dev;
} g = { .mu = PTHREAD_MUTEX_INITIALIZER };

/* ---- small helpers ------------------------------------------------------------------------ */

static void say(bool err, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	pthread_mutex_lock(&g.mu);
	vsnprintf(g.message, sizeof g.message, fmt, ap);
	g.msg_err = err;
	pthread_mutex_unlock(&g.mu);
	va_end(ap);
	if (err) LOGW(TAG, "%s", g.message); else LOGI(TAG, "%s", g.message);
}

static int run(const char *const argv[], char *out, size_t n, int timeout_s)
{
	char tmp[256];
	if (!out) { out = tmp; n = sizeof tmp; }
	out[0] = 0;
	return pf_run_capture(argv, out, n, timeout_s);
}

static void rm_rf(const char *path)
{
	if (!path || !path[0] || !strcmp(path, "/")) return;
	const char *argv[] = { "rm", "-rf", path, NULL };
	run(argv, NULL, 0, 30);
}

static int copy_file(const char *from, const char *to)
{
	size_t len = 0;
	char *data = pf_read_file(from, &len);
	if (!data) return -1;
	int rc = pf_write_file_atomic(to, data, len);
	free(data);
	return rc;
}

static long file_size(const char *path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_size : -1; }

/* "1.8 MB" or "640 KB": a size a person reads, not a fraction of a megabyte */
static const char *fmt_size(long b, char *out, size_t n)
{
	if (b >= 1048576) snprintf(out, n, "%.1f MB", b / 1048576.0);
	else snprintf(out, n, "%ld KB", (b + 512) / 1024);
	return out;
}

/* pifire-<grill>-YYYYMMDD-HHMM.tar.gz: sorts by time on any listing, says whose it is */
static void archive_name(char *out, size_t n, double wall)
{
	char grill[64] = "", host[64] = "pifire";
	pf_set_str("globals.grill_name", grill, sizeof grill, "");
	if (!grill[0]) gethostname(host, sizeof host);
	const char *src = grill[0] ? grill : host;
	char slug[40]; size_t k = 0;
	for (const char *p = src; *p && k < sizeof slug - 1; p++) {
		if (isalnum((unsigned char)*p)) slug[k++] = (char)tolower((unsigned char)*p);
		else if (k && slug[k - 1] != '-') slug[k++] = '-';
	}
	while (k && slug[k - 1] == '-') k--;
	slug[k] = 0;
	time_t t = (time_t)wall;
	struct tm tm; localtime_r(&t, &tm);
	char stamp[32]; strftime(stamp, sizeof stamp, "%Y%m%d-%H%M", &tm);
	snprintf(out, n, "pifire-%s-%s.tar.gz", slug[0] ? slug : "grill", stamp);
}

static bool is_ours(const char *name)
{
	size_t l = strlen(name);
	return l > 14 && !strncmp(name, "pifire-", 7) && !strcmp(name + l - 7, ".tar.gz") && !strchr(name, '/') && !strchr(name, '"');
}

/* the moment in the name, for listings that give none or give it in a locale */
static double ts_from_name(const char *name)
{
	const char *stamp = strrchr(name, '-');   /* -HHMM.tar.gz */
	const char *date = stamp ? stamp - 8 : NULL;   /* YYYYMMDD */
	struct tm tm = { 0 };
	if (!date || date <= name) return 0;
	if (sscanf(date, "%4d%2d%2d-%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min) != 5) return 0;
	tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = -1;
	return (double)mktime(&tm);
}

/* ---- locations ----------------------------------------------------------------------------- */

static void loc_from_json(cJSON *j, loc_t *L)
{
	memset(L, 0, sizeof *L);
	pf_strlcpy(L->id, pf_json_str(j, "id", ""), sizeof L->id);
	pf_strlcpy(L->type, pf_json_str(j, "type", ""), sizeof L->type);
	pf_strlcpy(L->name, pf_json_str(j, "name", ""), sizeof L->name);
	L->enabled = pf_json_bool(j, "enabled", true);
	pf_strlcpy(L->host, pf_json_str(j, "host", ""), sizeof L->host);
	pf_strlcpy(L->share, pf_json_str(j, "share", ""), sizeof L->share);
	pf_strlcpy(L->path, pf_json_str(j, "path", ""), sizeof L->path);
	pf_strlcpy(L->user, pf_json_str(j, "user", ""), sizeof L->user);
	pf_strlcpy(L->pass, pf_json_str(j, "password", ""), sizeof L->pass);
	pf_strlcpy(L->folder, pf_json_str(j, "folder", ""), sizeof L->folder);
	pf_strlcpy(L->client_id, pf_json_str(j, "client_id", ""), sizeof L->client_id);
	pf_strlcpy(L->client_secret, pf_json_str(j, "client_secret", ""), sizeof L->client_secret);
	pf_strlcpy(L->cloud_folder, pf_json_str(j, "cloud_folder", ""), sizeof L->cloud_folder);
	if (!L->name[0]) {
		pf_strlcpy(L->name, !strcmp(L->type, "gdrive") ? "Google Drive" : !strcmp(L->type, "onedrive") ? "OneDrive"
		           : !strcmp(L->type, "smb") ? "Network share" : "Folder", sizeof L->name);
	}
}

static void resolve_client(loc_t *L);
static int locations(loc_t *out, int max)
{
	cJSON *arr = pf_set_dup("backup.locations");
	int k = 0;
	cJSON *j;
	cJSON_ArrayForEach(j, arr) {
		if (k >= max) break;
		loc_from_json(j, &out[k]);
		resolve_client(&out[k]);
		if (out[k].id[0] && out[k].type[0]) k++;
	}
	cJSON_Delete(arr);
	return k;
}

static bool find_loc(const char *id, loc_t *out)
{
	loc_t all[MAX_LOC];
	int n = locations(all, MAX_LOC);
	for (int i = 0; i < n; i++) if (!strcmp(all[i].id, id)) { *out = all[i]; return true; }
	return false;
}

static bool is_cloud(const loc_t *L) { return !strcmp(L->type, "gdrive") || !strcmp(L->type, "onedrive"); }

/* The project's own clients, shipped with the daemon, so connecting a cloud is one tap and a
 * code -- nobody should have to open a developer console to back up a grill. A location that
 * carries its own client (an advanced choice) uses that instead. */
static bool builtin_client(const char *type, char *id, size_t in, char *secret, size_t sn)
{
	id[0] = 0; if (secret) secret[0] = 0;
	const pf_embedded_file *f = pf_embedded_share("oauth-clients.json");
	if (!f) return false;
	cJSON *j = cJSON_ParseWithLength((const char *)f->data, f->len);
	if (!j) return false;
	cJSON *c = cJSON_GetObjectItem(j, type);
	pf_strlcpy(id, pf_json_str(c, "client_id", ""), in);
	if (secret) pf_strlcpy(secret, pf_json_str(c, "client_secret", ""), sn);
	cJSON_Delete(j);
	return id[0] != 0;
}

/* fills in the client a location will actually use */
static void resolve_client(loc_t *L)
{
	if (!is_cloud(L) || L->client_id[0]) return;
	builtin_client(L->type, L->client_id, sizeof L->client_id, L->client_secret, sizeof L->client_secret);
}

/* cloud tokens live in the database, keyed by the location, so a restored backup brings its
 * connections with it */
static bool token_get(const loc_t *L, char *out, size_t n)
{
	char key[40], buf[3000];
	snprintf(key, sizeof key, "token:%.23s", L->id);
	out[0] = 0;
	if (pf_db_kv_get("backup", key, buf, sizeof buf) != 0) return false;
	cJSON *j = cJSON_Parse(buf);
	if (!j) return false;
	pf_strlcpy(out, pf_json_str(j, "refresh_token", ""), n);
	cJSON_Delete(j);
	return out[0] != 0;
}
static void token_put(const loc_t *L, const char *refresh)
{
	char key[40];
	snprintf(key, sizeof key, "token:%.23s", L->id);
	cJSON *j = cJSON_CreateObject();
	cJSON_AddStringToObject(j, "refresh_token", refresh);
	cJSON_AddNumberToObject(j, "connected", pf_wall());
	char *txt = cJSON_PrintUnformatted(j); cJSON_Delete(j);
	if (txt && pf_db_handle()) pf_db_kv_put("backup", key, txt);
	free(txt);
}
static void token_drop(const char *id)
{
	char key[40];
	snprintf(key, sizeof key, "token:%.23s", id);
	if (pf_db_handle()) pf_db_kv_delete("backup", key);
}
static bool connected(const loc_t *L) { char t[2000]; return is_cloud(L) && token_get(L, t, sizeof t); }

/* ---- the archive --------------------------------------------------------------------------- */

int pf_backup_make(const char *out_path, char *err, size_t n)
{
	char stage[600];
	snprintf(stage, sizeof stage, "%s/stage-%d", g.work, (int)getpid());
	rm_rf(stage);
	if (pf_mkdir_p(stage)) { snprintf(err, n, "cannot create %.150s", stage); return -1; }
	char path[700];

	/* settings, as written */
	snprintf(path, sizeof path, "%s/settings.json", stage);
	if (pf_settings_save() || copy_file(g.config, path)) { snprintf(err, n, "cannot copy settings"); rm_rf(stage); return -1; }

	/* the database, snapshotted through SQLite's own backup so a cook writing history at the same
	 * moment cannot leave it torn; then the rolling chart taken back out of the copy */
	snprintf(path, sizeof path, "%s/pifire.db", stage);
	{
		sqlite3 *dst = NULL;
		if (sqlite3_open(path, &dst) != SQLITE_OK) { snprintf(err, n, "cannot create the database copy"); rm_rf(stage); return -1; }
		sqlite3 *src = pf_db_handle();
		sqlite3_backup *b = src ? sqlite3_backup_init(dst, "main", src, "main") : NULL;
		if (!b) { snprintf(err, n, "database snapshot failed: %s", src ? sqlite3_errmsg(dst) : "no database"); sqlite3_close(dst); rm_rf(stage); return -1; }
		sqlite3_backup_step(b, -1);
		sqlite3_backup_finish(b);
		const char *trim = "DELETE FROM history; DELETE FROM history_probe; DELETE FROM history_ctrl;"
		                   "DELETE FROM kv WHERE ns='tuner' AND key='inflight'; VACUUM;";
		char *emsg = NULL;
		if (sqlite3_exec(dst, trim, NULL, NULL, &emsg) != SQLITE_OK) LOGW(TAG, "trimming the copy: %s", emsg ? emsg : "?");
		sqlite3_free(emsg);
		sqlite3_close(dst);
	}

	/* every saved cook */
	char cdir[600], sdir[620];
	snprintf(cdir, sizeof cdir, "%s/cookfiles", g.data_dir);
	snprintf(sdir, sizeof sdir, "%s/cookfiles", stage);
	pf_mkdir_p(sdir);
	int cooks = 0;
	DIR *d = opendir(cdir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
		char from[900], to[900];
		snprintf(from, sizeof from, "%s/%s", cdir, e->d_name);
		snprintf(to, sizeof to, "%s/%s", sdir, e->d_name);
		if (copy_file(from, to) == 0) cooks++;
	}
	if (d) closedir(d);

	/* what this is */
	{
		cJSON *m = cJSON_CreateObject();
		char host[64] = ""; gethostname(host, sizeof host);
		char grill[64] = ""; pf_set_str("globals.grill_name", grill, sizeof grill, "");
		cJSON_AddNumberToObject(m, "format", 1);
		cJSON_AddStringToObject(m, "app", "pifire-c");
		cJSON_AddStringToObject(m, "version", PF_VERSION);
		cJSON_AddNumberToObject(m, "created", pf_wall());
		cJSON_AddStringToObject(m, "hostname", host);
		cJSON_AddStringToObject(m, "grill_name", grill);
		cJSON_AddNumberToObject(m, "cooks", cooks);
		cJSON *files = cJSON_AddArrayToObject(m, "files");
		cJSON_AddItemToArray(files, cJSON_CreateString("settings.json"));
		cJSON_AddItemToArray(files, cJSON_CreateString("pifire.db"));
		cJSON_AddItemToArray(files, cJSON_CreateString("cookfiles/"));
		char *txt = cJSON_Print(m);
		cJSON_Delete(m);
		snprintf(path, sizeof path, "%s/manifest.json", stage);
		int rc = txt ? pf_write_file_atomic(path, txt, strlen(txt)) : -1;
		free(txt);
		if (rc) { snprintf(err, n, "cannot write the manifest"); rm_rf(stage); return -1; }
	}

	const char *tar[] = { "tar", "-C", stage, "-czf", out_path, ".", NULL };
	char out[256];
	int rc = run(tar, out, sizeof out, 300);
	rm_rf(stage);
	if (rc != 0) { snprintf(err, n, "tar failed: %.150s", out); unlink(out_path); return -1; }
	return 0;
}

int pf_backup_stage(const char *archive, const char *stage_dir, char *err, size_t n)
{
	rm_rf(stage_dir);
	if (pf_mkdir_p(stage_dir)) { snprintf(err, n, "cannot create %.150s", stage_dir); return -1; }
	const char *tar[] = { "tar", "-C", stage_dir, "-xzf", archive, NULL };
	char out[256];
	if (run(tar, out, sizeof out, 300) != 0) { snprintf(err, n, "not a readable archive: %.150s", out); rm_rf(stage_dir); return -1; }
	char path[700];
	snprintf(path, sizeof path, "%s/manifest.json", stage_dir);
	char *txt = pf_read_file(path, NULL);
	cJSON *m = txt ? cJSON_Parse(txt) : NULL;
	free(txt);
	if (!m) { snprintf(err, n, "not a PiFire backup (no manifest)"); rm_rf(stage_dir); return -1; }
	int fmt = (int)pf_json_num(m, "format", 0);
	char app[32];
	pf_strlcpy(app, pf_json_str(m, "app", ""), sizeof app);
	cJSON_Delete(m);
	if (fmt != 1 || strcmp(app, "pifire-c")) { snprintf(err, n, "not a PiFire backup this version can read (format %d)", fmt); rm_rf(stage_dir); return -1; }
	snprintf(path, sizeof path, "%s/settings.json", stage_dir);
	if (!pf_file_exists(path)) { snprintf(err, n, "the backup has no settings.json"); rm_rf(stage_dir); return -1; }
	snprintf(path, sizeof path, "%s/pifire.db", stage_dir);
	if (!pf_file_exists(path)) { snprintf(err, n, "the backup has no database"); rm_rf(stage_dir); return -1; }
	/* the database has to open and look like ours before it is allowed anywhere near the real one */
	{
		sqlite3 *db = NULL;
		bool ok = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK;
		if (ok) {
			sqlite3_stmt *st = NULL;
			ok = sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('kv','meta')", -1, &st, NULL) == SQLITE_OK
			     && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 2;
			sqlite3_finalize(st);
		}
		sqlite3_close(db);
		if (!ok) { snprintf(err, n, "the backup's database is damaged"); rm_rf(stage_dir); return -1; }
	}
	return 0;
}

/* ---- the record of the last one ----------------------------------------------------------- */

static void load_last(void)
{
	char buf[2048];
	if (pf_db_kv_get("backup", "last", buf, sizeof buf) != 0) return;
	cJSON *j = cJSON_Parse(buf);
	if (!j) return;
	g.last_ts = pf_json_num(j, "ts", 0);
	pf_strlcpy(g.last_name, pf_json_str(j, "name", ""), sizeof g.last_name);
	g.last_size = (long)pf_json_num(j, "size", 0);
	g.last_ok = pf_json_bool(j, "ok", false);
	cJSON *r = cJSON_GetObjectItem(j, "results");
	char *txt = r ? cJSON_PrintUnformatted(r) : NULL;
	pf_strlcpy(g.last_results, txt ? txt : "{}", sizeof g.last_results);
	free(txt);
	g.last_good_ts = pf_json_num(j, "good_ts", g.last_ok ? g.last_ts : 0);
	cJSON_Delete(j);
}

static void file_last(bool ok, const char *name, long size, cJSON *results_owned)
{
	pthread_mutex_lock(&g.mu);
	g.last_ts = pf_wall(); g.last_ok = ok; g.last_size = size;
	if (ok) { g.last_good_ts = g.last_ts; g.fail_streak = 0; }
	pf_strlcpy(g.last_name, name ? name : "", sizeof g.last_name);
	char *rt = results_owned ? cJSON_PrintUnformatted(results_owned) : NULL;
	pf_strlcpy(g.last_results, rt ? rt : "{}", sizeof g.last_results);
	free(rt);
	cJSON *j = cJSON_CreateObject();
	cJSON_AddNumberToObject(j, "ts", g.last_ts);
	cJSON_AddStringToObject(j, "name", g.last_name);
	cJSON_AddNumberToObject(j, "size", (double)size);
	cJSON_AddBoolToObject(j, "ok", ok);
	cJSON_AddItemToObject(j, "results", results_owned ? results_owned : cJSON_CreateObject());
	cJSON_AddNumberToObject(j, "good_ts", g.last_good_ts);
	pthread_mutex_unlock(&g.mu);
	char *txt = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	if (txt && pf_db_handle()) pf_db_kv_put("backup", "last", txt);
	free(txt);
}

/* ---- a folder ------------------------------------------------------------------------------ */

static int folder_put(const loc_t *L, const char *local, const char *name, char *err, size_t n)
{
	char to[800];
	if (!L->folder[0]) { snprintf(err, n, "no folder is set"); return -1; }
	if (pf_mkdir_p(L->folder)) { snprintf(err, n, "cannot create %.150s", L->folder); return -1; }
	snprintf(to, sizeof to, "%s/%s", L->folder, name);
	if (copy_file(local, to)) { snprintf(err, n, "cannot write to %.150s: %s", L->folder, strerror(errno)); return -1; }
	return 0;
}

static cJSON *folder_list(const loc_t *L, char *err, size_t n)
{
	if (!L->folder[0]) { snprintf(err, n, "no folder is set"); return NULL; }
	DIR *d = opendir(L->folder);
	if (!d) { snprintf(err, n, "cannot open %.150s: %s", L->folder, strerror(errno)); return NULL; }
	cJSON *arr = cJSON_CreateArray();
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!is_ours(e->d_name)) continue;
		char p[800]; snprintf(p, sizeof p, "%s/%s", L->folder, e->d_name);
		struct stat st; if (stat(p, &st)) continue;
		cJSON *f = cJSON_CreateObject();
		cJSON_AddStringToObject(f, "name", e->d_name);
		cJSON_AddNumberToObject(f, "size", (double)st.st_size);
		cJSON_AddNumberToObject(f, "ts", (double)st.st_mtime);
		cJSON_AddItemToArray(arr, f);
	}
	closedir(d);
	return arr;
}

static int folder_del(const loc_t *L, const char *name) { char p[800]; snprintf(p, sizeof p, "%s/%s", L->folder, name); return unlink(p); }
static int folder_get(const loc_t *L, const char *name, const char *local) { char p[800]; snprintf(p, sizeof p, "%s/%s", L->folder, name); return copy_file(p, local); }

/* ---- a network share, through smbclient --------------------------------------------------- */

static bool smb_have(void) { const char *argv[] = { "smbclient", "-V", NULL }; return run(argv, NULL, 0, 10) == 0; }

/* //host/share, a credentials file the way smbclient wants it (never the password on a command
 * line, where ps would show it), and the folder within the share */
static int smb_prep(const loc_t *L, char *svc, size_t sn, char *auth, size_t an, char *dir, size_t dn, char *err, size_t en)
{
	if (!L->host[0] || !L->share[0]) { snprintf(err, en, "the share needs a host and a share name"); return -1; }
	if (!g.have_smb && !(g.have_smb = smb_have())) { snprintf(err, en, "smbclient is not installed on the grill (sudo apt install smbclient)"); return -1; }
	snprintf(svc, sn, "//%s/%s", L->host, L->share);
	snprintf(auth, an, "%s/.smbauth-%.23s", g.work, L->id);
	char body[300];
	snprintf(body, sizeof body, "username=%s\npassword=%s\n", L->user, L->pass);
	if (pf_write_file_atomic(auth, body, strlen(body))) { snprintf(err, en, "cannot write the credentials file"); return -1; }
	chmod(auth, 0600);
	pf_strlcpy(dir, L->path[0] ? L->path : "PiFire", dn);
	/* a leading slash and a trailing one both confuse smbclient's cd */
	size_t l = strlen(dir);
	while (l && dir[l - 1] == '/') dir[--l] = 0;
	if (dir[0] == '/') memmove(dir, dir + 1, strlen(dir));
	return 0;
}

static int smb_cmd(const loc_t *L, const char *cmd, char *out, size_t n, char *err, size_t en)
{
	char svc[300], auth[600], dir[256];
	if (smb_prep(L, svc, sizeof svc, auth, sizeof auth, dir, sizeof dir, err, en)) return -1;
	char full[1200];
	if (dir[0]) snprintf(full, sizeof full, "cd \"%s\"; %s", dir, cmd); else snprintf(full, sizeof full, "%s", cmd);
	const char *argv[] = { "smbclient", svc, "-A", auth, "-c", full, NULL };
	int rc = run(argv, out, n, 120);
	if (rc != 0) {
		/* smbclient's own words are the useful part; the first line usually says it */
		char *nl = strchr(out, '\n'); if (nl) *nl = 0;
		snprintf(err, en, "%s", out[0] ? out : "smbclient failed");
	}
	return rc;
}

static int smb_put(const loc_t *L, const char *local, const char *name, char *err, size_t n)
{
	/* make the folder if it is not there; smbclient says so and fails when it already exists,
	 * which is not a failure */
	char svc[300], auth[600], dir[256], out[4096];
	if (smb_prep(L, svc, sizeof svc, auth, sizeof auth, dir, sizeof dir, err, n)) return -1;
	if (dir[0]) {
		char mk[400]; snprintf(mk, sizeof mk, "mkdir \"%s\"", dir);
		const char *argv[] = { "smbclient", svc, "-A", auth, "-c", mk, NULL };
		run(argv, out, sizeof out, 60);
	}
	char cmd[1400];
	snprintf(cmd, sizeof cmd, "put \"%s\" \"%s\"", local, name);
	return smb_cmd(L, cmd, out, sizeof out, err, n);
}

static cJSON *smb_list(const loc_t *L, char *err, size_t n)
{
	size_t cap = 65536;
	char *out = malloc(cap);
	if (!out) { snprintf(err, n, "out of memory"); return NULL; }
	if (smb_cmd(L, "ls pifire-*.tar.gz", out, cap, err, n) != 0 && !strstr(out, "blocks")) { free(out); return NULL; }
	cJSON *arr = cJSON_CreateArray();
	/* smbclient's listing:  "  pifire-x-20260926-0300.tar.gz   A   1843201  Fri Sep 26 03:00:12 2026" */
	char *save = NULL;
	for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char name[200], attr[16]; long size = 0;
		if (sscanf(line, " %199s %15s %ld", name, attr, &size) != 3 || !is_ours(name)) continue;
		cJSON *f = cJSON_CreateObject();
		cJSON_AddStringToObject(f, "name", name);
		cJSON_AddNumberToObject(f, "size", (double)size);
		cJSON_AddNumberToObject(f, "ts", ts_from_name(name));
		cJSON_AddItemToArray(arr, f);
	}
	free(out);
	return arr;
}

static int smb_del(const loc_t *L, const char *name) { char cmd[300], out[2048], err[200]; snprintf(cmd, sizeof cmd, "del \"%s\"", name); return smb_cmd(L, cmd, out, sizeof out, err, sizeof err); }
static int smb_get(const loc_t *L, const char *name, const char *local) { char cmd[1000], out[2048], err[200]; snprintf(cmd, sizeof cmd, "get \"%s\" \"%s\"", name, local); return smb_cmd(L, cmd, out, sizeof out, err, sizeof err); }

/* ---- browsing: what is at a path, so a location can be picked rather than typed ---------- */

static int by_str(const void *a, const void *b) { return strcasecmp(*(const char *const *)a, *(const char *const *)b); }

static cJSON *sorted_names(char **names, int k)
{
	qsort(names, (size_t)k, sizeof *names, by_str);
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < k; i++) { cJSON_AddItemToArray(arr, cJSON_CreateString(names[i])); free(names[i]); }
	return arr;
}

static cJSON *browse_folder(const char *path, char *err, size_t n)
{
	char here[512];
	pf_strlcpy(here, path && path[0] ? path : "/", sizeof here);
	size_t l = strlen(here);
	while (l > 1 && here[l - 1] == '/') here[--l] = 0;
	DIR *d = opendir(here);
	if (!d) { snprintf(err, n, "cannot open %.150s: %s", here, strerror(errno)); return NULL; }
	char *names[512]; int k = 0;
	struct dirent *e;
	while ((e = readdir(d)) && k < 512) {
		if (e->d_name[0] == '.') continue;
		char p[900]; snprintf(p, sizeof p, "%s/%s", here, e->d_name);
		struct stat st;
		if (stat(p, &st) || !S_ISDIR(st.st_mode)) continue;
		names[k++] = strdup(e->d_name);
	}
	closedir(d);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "path", here);
	cJSON_AddItemToObject(o, "dirs", sorted_names(names, k));
	if (strcmp(here, "/")) {
		char *slash = strrchr(here, '/');
		if (slash) { *slash = 0; cJSON_AddStringToObject(o, "parent", here[0] ? here : "/"); }
	}
	return o;
}

/* the shares a host offers, from smbclient -L: "\tSharename  Type  Comment" rows, Disk ones only */
static cJSON *browse_shares(const loc_t *L, char *err, size_t n)
{
	if (!L->host[0]) { snprintf(err, n, "enter the host first"); return NULL; }
	if (!g.have_smb && !(g.have_smb = smb_have())) { snprintf(err, n, "smbclient is not installed on the grill (sudo apt install smbclient)"); return NULL; }
	char auth[600];
	snprintf(auth, sizeof auth, "%s/.smbauth-browse", g.work);
	char body[300];
	snprintf(body, sizeof body, "username=%s\npassword=%s\n", L->user, L->pass);
	pf_write_file_atomic(auth, body, strlen(body));
	chmod(auth, 0600);
	char svc[160];
	snprintf(svc, sizeof svc, "//%s", L->host);
	const char *argv[] = { "smbclient", "-L", svc, "-A", auth, "-g", NULL };
	char *out = malloc(65536);
	if (!out) { snprintf(err, n, "out of memory"); return NULL; }
	int rc = run(argv, out, 65536, 60);
	/* -g gives "Disk|name|comment" lines, one per share; the exit code is unreliable when the
	 * host also refuses the browse list, so what matters is whether any came back */
	char *names[256]; int k = 0;
	char *save = NULL;
	for (char *line = strtok_r(out, "\n", &save); line && k < 256; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "Disk|", 5)) continue;
		char *name = line + 5, *bar = strchr(name, '|');
		if (bar) *bar = 0;
		size_t nl = strlen(name);
		if (!nl || name[nl - 1] == '$') continue;   /* administrative shares are not for backups */
		names[k++] = strdup(name);
	}
	if (!k && rc != 0) {
		char *nl = strchr(out, '\n'); if (nl) *nl = 0;
		snprintf(err, n, "%s", out[0] ? out : "could not reach the host");
		free(out);
		return NULL;
	}
	free(out);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddItemToObject(o, "shares", sorted_names(names, k));
	return o;
}

static cJSON *browse_share(const loc_t *L, const char *path, char *err, size_t n)
{
	loc_t at = *L;
	pf_strlcpy(at.path, path && path[0] ? path : "", sizeof at.path);
	/* smb_cmd cd's into L->path; a blank one means the root of the share, which smb_prep would
	 * otherwise turn into the PiFire default */
	char svc[300], auth[600], dir[256];
	if (smb_prep(&at, svc, sizeof svc, auth, sizeof auth, dir, sizeof dir, err, n)) return NULL;
	char full[600];
	if (at.path[0]) snprintf(full, sizeof full, "cd \"%s\"; ls", dir); else snprintf(full, sizeof full, "ls");
	const char *argv[] = { "smbclient", svc, "-A", auth, "-c", full, NULL };
	char *out = malloc(65536);
	if (!out) { snprintf(err, n, "out of memory"); return NULL; }
	int rc = run(argv, out, 65536, 60);
	if (rc != 0 && !strstr(out, "blocks")) {
		char *nl = strchr(out, '\n'); if (nl) *nl = 0;
		snprintf(err, n, "%s", out[0] ? out : "smbclient failed");
		free(out);
		return NULL;
	}
	/* "  name   D   0  Fri Sep 26 ..." -- the attribute column says D for a directory */
	char *names[512]; int k = 0;
	char *save = NULL;
	for (char *line = strtok_r(out, "\n", &save); line && k < 512; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] != ' ') continue;
		/* the name may hold spaces: it ends where the attribute column begins, which is the
		 * last run of spaces before a short token of attribute letters */
		char name[256] = "", attr[16] = "";
		long size = -1;
		/* scan from the right: "<size> <weekday> ..." after the attribute */
		int i = (int)strlen(line);
		while (i > 0 && line[i - 1] == ' ') i--;
		/* walk back over the date (5 tokens) and size */
		int tokens = 0; int j = i;
		while (j > 0 && tokens < 6) { while (j > 0 && line[j - 1] != ' ') j--; tokens++; if (tokens < 6) while (j > 0 && line[j - 1] == ' ') j--; }
		if (tokens < 6) continue;
		if (sscanf(line + j, "%15s %ld", attr, &size) != 2) continue;
		int e = j; while (e > 0 && line[e - 1] == ' ') e--;
		int b = 0; while (line[b] == ' ') b++;
		if (e <= b) continue;
		int nl2 = e - b; if (nl2 > 255) nl2 = 255;
		memcpy(name, line + b, (size_t)nl2); name[nl2] = 0;
		if (!strchr(attr, 'D') || !strcmp(name, ".") || !strcmp(name, "..")) continue;
		names[k++] = strdup(name);
	}
	free(out);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "path", at.path);
	cJSON_AddItemToObject(o, "dirs", sorted_names(names, k));
	if (at.path[0]) {
		char parent[256]; pf_strlcpy(parent, at.path, sizeof parent);
		char *slash = strrchr(parent, '/');
		if (slash) *slash = 0; else parent[0] = 0;
		cJSON_AddStringToObject(o, "parent", parent);
	}
	return o;
}

cJSON *pf_backup_browse(cJSON *req, char *err, size_t n)
{
	const char *type = pf_json_str(req, "type", "folder");
	const char *path = pf_json_str(req, "path", "");
	if (!strcmp(type, "folder")) return browse_folder(path, err, n);
	if (!strcmp(type, "smb")) {
		loc_t L; memset(&L, 0, sizeof L);
		pf_strlcpy(L.id, "browse", sizeof L.id); pf_strlcpy(L.type, "smb", sizeof L.type);
		pf_strlcpy(L.host, pf_json_str(req, "host", ""), sizeof L.host);
		pf_strlcpy(L.share, pf_json_str(req, "share", ""), sizeof L.share);
		pf_strlcpy(L.user, pf_json_str(req, "user", ""), sizeof L.user);
		pf_strlcpy(L.pass, pf_json_str(req, "password", ""), sizeof L.pass);
		return L.share[0] ? browse_share(&L, path, err, n) : browse_shares(&L, err, n);
	}
	snprintf(err, n, "nothing to browse for a %s location", type);
	return NULL;
}

/* ---- the clouds ---------------------------------------------------------------------------- */
#if PF_WITH_CURL

typedef struct { char *buf; size_t len, cap; } membuf;
static size_t mem_cb(char *p, size_t sz, size_t n, void *ud)
{
	membuf *m = ud; size_t add = sz * n;
	if (m->len + add + 1 > m->cap) {
		size_t nc = m->cap ? m->cap * 2 : 4096;
		while (nc < m->len + add + 1) nc *= 2;
		char *nb = realloc(m->buf, nc);
		if (!nb) return 0;
		m->buf = nb; m->cap = nc;
	}
	memcpy(m->buf + m->len, p, add); m->len += add; m->buf[m->len] = 0;
	return add;
}

typedef struct {
	const char *method, *url, *bearer, *ctype, *body, *upload_path, *extra_hdr;
	size_t blen;
	bool follow;
	char *hdr_out; size_t hdr_n;
} req_t;

/* One request. The reply body is handed back and the status returned; -1 on a transport failure
 * with err filled. */
static long http(const req_t *r, membuf *reply, char *err, size_t en)
{
	CURL *c = curl_easy_init();
	if (!c) { snprintf(err, en, "curl init failed"); return -1; }
	struct curl_slist *h = NULL;
	char auth[3100], ct[128];
	if (r->bearer) { snprintf(auth, sizeof auth, "Authorization: Bearer %s", r->bearer); h = curl_slist_append(h, auth); }
	if (r->ctype) { snprintf(ct, sizeof ct, "Content-Type: %s", r->ctype); h = curl_slist_append(h, ct); }
	if (r->extra_hdr) h = curl_slist_append(h, r->extra_hdr);
	FILE *up = NULL;
	curl_easy_setopt(c, CURLOPT_URL, r->url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "pifired/" PF_VERSION);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, r->upload_path || r->follow ? 900L : 60L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
	/* a download may be a redirect to a signed link; the bearer stays behind on a change of host */
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, r->follow ? 1L : 0L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
	if (h) curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, reply);
	membuf hdrs = { 0 };
	if (r->hdr_out) { curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, mem_cb); curl_easy_setopt(c, CURLOPT_HEADERDATA, &hdrs); }
	if (r->upload_path) {
		up = fopen(r->upload_path, "rb");
		if (!up) { snprintf(err, en, "cannot read %s", r->upload_path); curl_slist_free_all(h); curl_easy_cleanup(c); return -1; }
		curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(c, CURLOPT_READDATA, up);
		curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_size(r->upload_path));
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, r->method);
	} else if (r->body) {
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->body);
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)r->blen);
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, r->method);
	} else if (strcmp(r->method, "GET")) {
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, r->method);
	}
	CURLcode rc = curl_easy_perform(c);
	long status = 0;
	if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	else snprintf(err, en, "%s", curl_easy_strerror(rc));
	if (r->hdr_out) { pf_strlcpy(r->hdr_out, hdrs.buf ? hdrs.buf : "", r->hdr_n); free(hdrs.buf); }
	if (up) fclose(up);
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
	return rc == CURLE_OK ? status : -1;
}

static long http_form(const char *url, const char *form, membuf *reply, char *err, size_t en)
{
	req_t r = { .method = "POST", .url = url, .ctype = "application/x-www-form-urlencoded", .body = form, .blen = strlen(form) };
	return http(&r, reply, err, en);
}

static char *url_escape(const char *s)
{
	CURL *c = curl_easy_init();
	char *e = c ? curl_easy_escape(c, s, 0) : NULL;
	char *out = e ? strdup(e) : NULL;
	if (e) curl_free(e);
	if (c) curl_easy_cleanup(c);
	return out;
}

/* the service's own account of what went wrong, when it gives one */
static void api_error(const char *who, membuf *m, long status, char *err, size_t n)
{
	cJSON *j = m->buf ? cJSON_Parse(m->buf) : NULL;
	const char *msg = j ? pf_json_str(j, "error.message", "") : "";
	if (!msg[0] && j) msg = pf_json_str(j, "error_description", "");
	if (!msg[0] && j) msg = pf_json_str(j, "error", "");
	snprintf(err, n, "%s said %ld%s%.160s", who, status, msg[0] ? ": " : "", msg);
	cJSON_Delete(j);
}

/* a header's value out of a raw header block, case-insensitively */
static bool header_value(const char *hdrs, const char *name, char *out, size_t n)
{
	size_t nl = strlen(name);
	for (const char *p = hdrs; p && *p; ) {
		const char *eol = strpbrk(p, "\r\n");
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		if (len > nl + 1 && !strncasecmp(p, name, nl) && p[nl] == ':') {
			const char *v = p + nl + 1;
			while (*v == ' ' && v < p + len) v++;
			size_t vl = (size_t)(p + len - v);
			if (vl >= n) vl = n - 1;
			memcpy(out, v, vl); out[vl] = 0;
			return true;
		}
		if (!eol) break;
		p = eol; while (*p == '\r' || *p == '\n') p++;
	}
	out[0] = 0;
	return false;
}

/* --- an access token, fresh, from the refresh token on file --- */
static int cloud_access(const loc_t *L, char *token, size_t n, char *err, size_t en)
{
	char refresh[2000];
	if (!L->client_id[0]) { snprintf(err, en, "%s needs a client ID", L->name); return -1; }
	if (!token_get(L, refresh, sizeof refresh)) { snprintf(err, en, "%s is not connected", L->name); return -1; }
	bool google = !strcmp(L->type, "gdrive");
	char *eid = url_escape(L->client_id), *esec = url_escape(L->client_secret), *eref = url_escape(refresh);
	char form[4000];
	if (google) snprintf(form, sizeof form, "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token", eid ? eid : "", esec ? esec : "", eref ? eref : "");
	else snprintf(form, sizeof form, "client_id=%s&refresh_token=%s&grant_type=refresh_token&scope=%s", eid ? eid : "", eref ? eref : "", "Files.ReadWrite.AppFolder%20offline_access");
	free(eid); free(esec); free(eref);
	membuf m = { 0 };
	long st = http_form(google ? GDRIVE_TOKEN_URL : MS_TOKEN_URL, form, &m, err, en);
	if (st < 0) { free(m.buf); return -1; }
	if (st != 200) {
		api_error(google ? "Google" : "Microsoft", &m, st, err, en); free(m.buf);
		if (st == 400 || st == 401) { LOGW(TAG, "%s refused the refresh token; disconnecting", L->name); token_drop(L->id); }
		return -1;
	}
	cJSON *j = cJSON_Parse(m.buf); free(m.buf);
	pf_strlcpy(token, j ? pf_json_str(j, "access_token", "") : "", n);
	/* Microsoft hands out a new refresh token each time; keep the latest */
	if (j && !google && pf_json_str(j, "refresh_token", "")[0]) token_put(L, pf_json_str(j, "refresh_token", ""));
	cJSON_Delete(j);
	if (!token[0]) { snprintf(err, en, "no access token came back"); return -1; }
	return 0;
}

/* --- Google Drive --- */

/* the folder the backups go in, made if it is not there */
static int gdrive_folder(const loc_t *L, const char *token, char *id, size_t n, bool create, char *err, size_t en)
{
	const char *name = L->cloud_folder[0] ? L->cloud_folder : "PiFire Backups";
	char q[400];
	snprintf(q, sizeof q, "name='%s' and mimeType='application/vnd.google-apps.folder' and trashed=false", name);
	char *eq = url_escape(q);
	char url[900];
	snprintf(url, sizeof url, GDRIVE_API "?q=%s&fields=files(id,name)&spaces=drive", eq ? eq : "");
	free(eq);
	membuf m = { 0 };
	req_t r = { .method = "GET", .url = url, .bearer = token };
	long st = http(&r, &m, err, en);
	if (st < 0) { free(m.buf); return -1; }
	if (st != 200) { api_error("Google", &m, st, err, en); free(m.buf); return -1; }
	cJSON *j = cJSON_Parse(m.buf); free(m.buf);
	cJSON *files = j ? cJSON_GetObjectItem(j, "files") : NULL;
	cJSON *first = files ? cJSON_GetArrayItem(files, 0) : NULL;
	id[0] = 0;
	if (first) pf_strlcpy(id, pf_json_str(first, "id", ""), n);
	cJSON_Delete(j);
	if (id[0]) return 0;
	if (!create) { snprintf(err, en, "no backups folder yet"); return 1; }
	cJSON *meta = cJSON_CreateObject();
	cJSON_AddStringToObject(meta, "name", name);
	cJSON_AddStringToObject(meta, "mimeType", "application/vnd.google-apps.folder");
	char *body = cJSON_PrintUnformatted(meta); cJSON_Delete(meta);
	membuf rb = { 0 };
	req_t cr = { .method = "POST", .url = GDRIVE_API "?fields=id", .bearer = token, .ctype = "application/json", .body = body, .blen = strlen(body) };
	st = http(&cr, &rb, err, en);
	free(body);
	if (st < 0) { free(rb.buf); return -1; }
	if (st != 200) { api_error("Google", &rb, st, err, en); free(rb.buf); return -1; }
	j = cJSON_Parse(rb.buf); free(rb.buf);
	pf_strlcpy(id, j ? pf_json_str(j, "id", "") : "", n);
	cJSON_Delete(j);
	if (!id[0]) { snprintf(err, en, "Google made no folder"); return -1; }
	return 0;
}

static int gdrive_put(const loc_t *L, const char *local, const char *name, char *err, size_t n)
{
	char token[3000], folder[128];
	if (cloud_access(L, token, sizeof token, err, n)) return -1;
	if (gdrive_folder(L, token, folder, sizeof folder, true, err, n)) return -1;
	/* resumable upload: the metadata first, then the bytes to the session it hands back */
	cJSON *meta = cJSON_CreateObject();
	cJSON_AddStringToObject(meta, "name", name);
	cJSON *parents = cJSON_AddArrayToObject(meta, "parents");
	cJSON_AddItemToArray(parents, cJSON_CreateString(folder));
	char *body = cJSON_PrintUnformatted(meta); cJSON_Delete(meta);
	membuf m = { 0 };
	char hdrs[8192];
	req_t r = { .method = "POST", .url = GDRIVE_UPLOAD, .bearer = token, .ctype = "application/json; charset=UTF-8", .body = body, .blen = strlen(body), .hdr_out = hdrs, .hdr_n = sizeof hdrs };
	long st = http(&r, &m, err, n);
	free(body);
	if (st < 0) { free(m.buf); return -1; }
	if (st != 200) { api_error("Google", &m, st, err, n); free(m.buf); return -1; }
	free(m.buf);
	char session[1024];
	if (!header_value(hdrs, "Location", session, sizeof session)) { snprintf(err, n, "Google gave no upload session"); return -1; }
	membuf rb = { 0 };
	req_t up = { .method = "PUT", .url = session, .bearer = token, .ctype = "application/gzip", .upload_path = local };
	st = http(&up, &rb, err, n);
	if (st < 0) { free(rb.buf); return -1; }
	if (st != 200 && st != 201) { api_error("Google", &rb, st, err, n); free(rb.buf); return -1; }
	free(rb.buf);
	return 0;
}

static cJSON *gdrive_list(const loc_t *L, char *err, size_t n)
{
	char token[3000], folder[128];
	if (cloud_access(L, token, sizeof token, err, n)) return NULL;
	int fr = gdrive_folder(L, token, folder, sizeof folder, false, err, n);
	if (fr < 0) return NULL;
	if (fr > 0) return cJSON_CreateArray();
	char q[300];
	snprintf(q, sizeof q, "'%s' in parents and trashed=false", folder);
	char *eq = url_escape(q);
	char url[900];
	snprintf(url, sizeof url, GDRIVE_API "?q=%s&fields=files(id,name,size,createdTime)&orderBy=createdTime%%20desc&pageSize=100", eq ? eq : "");
	free(eq);
	membuf m = { 0 };
	req_t r = { .method = "GET", .url = url, .bearer = token };
	long st = http(&r, &m, err, n);
	if (st < 0) { free(m.buf); return NULL; }
	if (st != 200) { api_error("Google", &m, st, err, n); free(m.buf); return NULL; }
	cJSON *j = cJSON_Parse(m.buf); free(m.buf);
	cJSON *arr = cJSON_CreateArray();
	cJSON *files = j ? cJSON_GetObjectItem(j, "files") : NULL, *f;
	cJSON_ArrayForEach(f, files) {
		const char *name = pf_json_str(f, "name", "");
		if (!is_ours(name)) continue;
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", name);
		cJSON_AddStringToObject(o, "id", pf_json_str(f, "id", ""));
		cJSON_AddNumberToObject(o, "size", atof(pf_json_str(f, "size", "0")));
		cJSON_AddNumberToObject(o, "ts", ts_from_name(name));
		cJSON_AddItemToArray(arr, o);
	}
	cJSON_Delete(j);
	return arr;
}

/* --- OneDrive --- */

static cJSON *onedrive_list(const loc_t *L, char *err, size_t n)
{
	char token[3000];
	if (cloud_access(L, token, sizeof token, err, n)) return NULL;
	membuf m = { 0 };
	req_t r = { .method = "GET", .url = MS_GRAPH "/me/drive/special/approot/children?$select=id,name,size,createdDateTime&$top=200", .bearer = token };
	long st = http(&r, &m, err, n);
	if (st < 0) { free(m.buf); return NULL; }
	if (st != 200) { api_error("Microsoft", &m, st, err, n); free(m.buf); return NULL; }
	cJSON *j = cJSON_Parse(m.buf); free(m.buf);
	cJSON *arr = cJSON_CreateArray();
	cJSON *items = j ? cJSON_GetObjectItem(j, "value") : NULL, *f;
	cJSON_ArrayForEach(f, items) {
		const char *name = pf_json_str(f, "name", "");
		if (!is_ours(name)) continue;
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", name);
		cJSON_AddStringToObject(o, "id", pf_json_str(f, "id", ""));
		cJSON_AddNumberToObject(o, "size", pf_json_num(f, "size", 0));
		cJSON_AddNumberToObject(o, "ts", ts_from_name(name));
		cJSON_AddItemToArray(arr, o);
	}
	cJSON_Delete(j);
	return arr;
}

static int onedrive_put(const loc_t *L, const char *local, const char *name, char *err, size_t n)
{
	char token[3000];
	if (cloud_access(L, token, sizeof token, err, n)) return -1;
	/* an upload session, then the whole file in one PUT to it: fine for anything under 60 MB,
	 * and a backup is a few */
	char url[400];
	snprintf(url, sizeof url, MS_GRAPH "/me/drive/special/approot:/%s:/createUploadSession", name);
	const char *body = "{\"item\":{\"@microsoft.graph.conflictBehavior\":\"replace\"}}";
	membuf m = { 0 };
	req_t r = { .method = "POST", .url = url, .bearer = token, .ctype = "application/json", .body = body, .blen = strlen(body) };
	long st = http(&r, &m, err, n);
	if (st < 0) { free(m.buf); return -1; }
	if (st != 200 && st != 201) { api_error("Microsoft", &m, st, err, n); free(m.buf); return -1; }
	cJSON *j = cJSON_Parse(m.buf); free(m.buf);
	char session[2048];
	pf_strlcpy(session, j ? pf_json_str(j, "uploadUrl", "") : "", sizeof session);
	cJSON_Delete(j);
	if (!session[0]) { snprintf(err, n, "Microsoft gave no upload session"); return -1; }
	long size = file_size(local);
	char range[96];
	snprintf(range, sizeof range, "Content-Range: bytes 0-%ld/%ld", size - 1, size);
	membuf rb = { 0 };
	/* the session URL is pre-authorised: no bearer on this one, by Microsoft's instruction */
	req_t up = { .method = "PUT", .url = session, .ctype = "application/gzip", .upload_path = local, .extra_hdr = range };
	st = http(&up, &rb, err, n);
	if (st < 0) { free(rb.buf); return -1; }
	if (st != 200 && st != 201) { api_error("Microsoft", &rb, st, err, n); free(rb.buf); return -1; }
	free(rb.buf);
	return 0;
}

/* --- both clouds: by id --- */

static int cloud_id_for(const loc_t *L, const char *name, char *id, size_t n)
{
	char err[200];
	cJSON *l = !strcmp(L->type, "gdrive") ? gdrive_list(L, err, sizeof err) : onedrive_list(L, err, sizeof err);
	if (!l) return -1;
	id[0] = 0;
	cJSON *f;
	cJSON_ArrayForEach(f, l) if (!strcmp(pf_json_str(f, "name", ""), name)) { pf_strlcpy(id, pf_json_str(f, "id", ""), n); break; }
	cJSON_Delete(l);
	return id[0] ? 0 : -1;
}

static int cloud_del(const loc_t *L, const char *name)
{
	char token[3000], id[256], err[200], url[600];
	if (cloud_access(L, token, sizeof token, err, sizeof err) || cloud_id_for(L, name, id, sizeof id)) return -1;
	if (!strcmp(L->type, "gdrive")) snprintf(url, sizeof url, GDRIVE_API "/%s", id);
	else snprintf(url, sizeof url, MS_GRAPH "/me/drive/items/%s", id);
	membuf m = { 0 };
	req_t r = { .method = "DELETE", .url = url, .bearer = token };
	long st = http(&r, &m, err, sizeof err);
	free(m.buf);
	return st == 204 || st == 200 ? 0 : -1;
}

static int cloud_get(const loc_t *L, const char *name, const char *local)
{
	char token[3000], id[256], err[200], url[600];
	if (cloud_access(L, token, sizeof token, err, sizeof err) || cloud_id_for(L, name, id, sizeof id)) return -1;
	if (!strcmp(L->type, "gdrive")) snprintf(url, sizeof url, GDRIVE_API "/%s?alt=media", id);
	else snprintf(url, sizeof url, MS_GRAPH "/me/drive/items/%s/content", id);
	membuf m = { 0 };
	req_t r = { .method = "GET", .url = url, .bearer = token, .follow = true };
	long st = http(&r, &m, err, sizeof err);
	int rc = st == 200 && m.buf ? pf_write_file_atomic(local, m.buf, m.len) : -1;
	free(m.buf);
	return rc;
}

/* --- sign-in for a device with no browser: a code shown here, typed in on the phone --- */

static void *device_poll_thread(void *arg)
{
	(void)arg;
	loc_t L;
	char code[1200], type[12], id[24];
	double interval, expires;
	pthread_mutex_lock(&g.mu);
	pf_strlcpy(code, g.dev.device_code, sizeof code);
	pf_strlcpy(type, g.dev.type, sizeof type);
	pf_strlcpy(id, g.dev.loc, sizeof id);
	interval = g.dev.interval; expires = g.dev.expires;
	pthread_mutex_unlock(&g.mu);
	if (!find_loc(id, &L)) { pthread_mutex_lock(&g.mu); g.dev.pending = false; pthread_mutex_unlock(&g.mu); return NULL; }
	bool google = !strcmp(type, "gdrive");
	const char *who = google ? "Google" : "Microsoft";
	char *eid = url_escape(L.client_id), *esec = url_escape(L.client_secret), *ecode = url_escape(code);
	char form[4000];
	if (google) snprintf(form, sizeof form, "client_id=%s&client_secret=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code", eid ? eid : "", esec ? esec : "", ecode ? ecode : "");
	else snprintf(form, sizeof form, "client_id=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code", eid ? eid : "", ecode ? ecode : "");
	free(eid); free(esec); free(ecode);
	while (pf_wall() < expires) {
		pf_sleep_ms((unsigned)(interval * 1000));
		pthread_mutex_lock(&g.mu);
		bool still = g.dev.pending;
		pthread_mutex_unlock(&g.mu);
		if (!still) return NULL;
		membuf m = { 0 };
		char err[200];
		long st = http_form(google ? GDRIVE_TOKEN_URL : MS_TOKEN_URL, form, &m, err, sizeof err);
		cJSON *j = st >= 0 && m.buf ? cJSON_Parse(m.buf) : NULL;
		free(m.buf);
		const char *e = j ? pf_json_str(j, "error", "") : "";
		if (st == 200 && j && pf_json_str(j, "refresh_token", "")[0]) {
			token_put(&L, pf_json_str(j, "refresh_token", ""));
			cJSON_Delete(j);
			pthread_mutex_lock(&g.mu); g.dev.pending = false; pthread_mutex_unlock(&g.mu);
			say(false, "%s connected", L.name);
			pf_events_emit("Backup_Connected", "Backup location connected", "%s is connected; backups can go there now.", L.name);
			return NULL;
		}
		if (!strcmp(e, "slow_down")) interval += 5;
		else if (strcmp(e, "authorization_pending")) {
			say(true, "%s sign-in did not finish: %s", who, e[0] ? e : (st < 0 ? err : "no answer"));
			cJSON_Delete(j);
			pthread_mutex_lock(&g.mu); g.dev.pending = false; pthread_mutex_unlock(&g.mu);
			return NULL;
		}
		cJSON_Delete(j);
	}
	say(true, "the %s sign-in code expired before it was used", who);
	pthread_mutex_lock(&g.mu); g.dev.pending = false; pthread_mutex_unlock(&g.mu);
	return NULL;
}

int pf_backup_connect(const char *loc_id, char *err, size_t n)
{
	loc_t L;
	if (!find_loc(loc_id, &L) || !is_cloud(&L)) { snprintf(err, n, "no such cloud location"); return -1; }
	bool google = !strcmp(L.type, "gdrive");
	if (!L.client_id[0] || (google && !L.client_secret[0])) { snprintf(err, n, "enter the client ID%s first, then save", google ? " and secret" : ""); return -1; }
	pthread_mutex_lock(&g.mu);
	bool pending = g.dev.pending && !strcmp(g.dev.loc, loc_id);
	if (g.dev.pending && !pending) g.dev.pending = false;   /* a sign-in for another location gives way */
	pthread_mutex_unlock(&g.mu);
	if (pending) return 0;   /* the code already showing is the one to use */
	char *eid = url_escape(L.client_id), *esc = url_escape(google ? GDRIVE_SCOPE : MS_SCOPE);
	char form[1200];
	snprintf(form, sizeof form, "client_id=%s&scope=%s", eid ? eid : "", esc ? esc : "");
	free(eid); free(esc);
	membuf m = { 0 };
	long st = http_form(google ? GDRIVE_DEVICE_URL : MS_DEVICE_URL, form, &m, err, n);
	if (st < 0) { free(m.buf); return -1; }
	if (st != 200) { api_error(google ? "Google" : "Microsoft", &m, st, err, n); free(m.buf); return -1; }
	cJSON *j = cJSON_Parse(m.buf); free(m.buf);
	if (!j || !pf_json_str(j, "device_code", "")[0]) { cJSON_Delete(j); snprintf(err, n, "no device code came back"); return -1; }
	pthread_mutex_lock(&g.mu);
	g.dev.pending = true;
	pf_strlcpy(g.dev.loc, loc_id, sizeof g.dev.loc);
	pf_strlcpy(g.dev.type, L.type, sizeof g.dev.type);
	pf_strlcpy(g.dev.user_code, pf_json_str(j, "user_code", ""), sizeof g.dev.user_code);
	pf_strlcpy(g.dev.url, pf_json_str(j, google ? "verification_url" : "verification_uri", google ? "https://www.google.com/device" : "https://microsoft.com/devicelogin"), sizeof g.dev.url);
	pf_strlcpy(g.dev.device_code, pf_json_str(j, "device_code", ""), sizeof g.dev.device_code);
	g.dev.interval = pf_json_num(j, "interval", 5);
	g.dev.expires = pf_wall() + pf_json_num(j, "expires_in", 900);
	pthread_mutex_unlock(&g.mu);
	cJSON_Delete(j);
	pthread_t t;
	pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &a, device_poll_thread, NULL)) { pthread_attr_destroy(&a); pthread_mutex_lock(&g.mu); g.dev.pending = false; pthread_mutex_unlock(&g.mu); snprintf(err, n, "cannot start the sign-in"); return -1; }
	pthread_attr_destroy(&a);
	return 0;
}
#else
static int gdrive_put(const loc_t *L, const char *l, const char *nm, char *err, size_t n) { (void)L; (void)l; (void)nm; snprintf(err, n, "built without libcurl"); return -1; }
static int onedrive_put(const loc_t *L, const char *l, const char *nm, char *err, size_t n) { (void)L; (void)l; (void)nm; snprintf(err, n, "built without libcurl"); return -1; }
static cJSON *gdrive_list(const loc_t *L, char *err, size_t n) { (void)L; snprintf(err, n, "built without libcurl"); return NULL; }
static cJSON *onedrive_list(const loc_t *L, char *err, size_t n) { (void)L; snprintf(err, n, "built without libcurl"); return NULL; }
static int cloud_del(const loc_t *L, const char *name) { (void)L; (void)name; return -1; }
static int cloud_get(const loc_t *L, const char *name, const char *local) { (void)L; (void)name; (void)local; return -1; }
int pf_backup_connect(const char *loc_id, char *err, size_t n) { (void)loc_id; snprintf(err, n, "built without libcurl"); return -1; }
#endif

void pf_backup_disconnect(const char *loc_id)
{
	token_drop(loc_id);
	pthread_mutex_lock(&g.mu);
	if (g.dev.pending && !strcmp(g.dev.loc, loc_id)) g.dev.pending = false;
	pthread_mutex_unlock(&g.mu);
	say(false, "disconnected %s", loc_id);
}

/* ---- a location, whichever kind ------------------------------------------------------------ */

static int loc_put(const loc_t *L, const char *local, const char *name, char *err, size_t n)
{
	if (!strcmp(L->type, "gdrive")) return gdrive_put(L, local, name, err, n);
	if (!strcmp(L->type, "onedrive")) return onedrive_put(L, local, name, err, n);
	if (!strcmp(L->type, "smb")) return smb_put(L, local, name, err, n);
	if (!strcmp(L->type, "folder")) return folder_put(L, local, name, err, n);
	snprintf(err, n, "unknown location type %s", L->type); return -1;
}
static cJSON *loc_list(const loc_t *L, char *err, size_t n)
{
	if (!strcmp(L->type, "gdrive")) return gdrive_list(L, err, n);
	if (!strcmp(L->type, "onedrive")) return onedrive_list(L, err, n);
	if (!strcmp(L->type, "smb")) return smb_list(L, err, n);
	if (!strcmp(L->type, "folder")) return folder_list(L, err, n);
	snprintf(err, n, "unknown location type %s", L->type); return NULL;
}
static int loc_del(const loc_t *L, const char *name) { return is_cloud(L) ? cloud_del(L, name) : !strcmp(L->type, "smb") ? smb_del(L, name) : folder_del(L, name); }
static int loc_get(const loc_t *L, const char *name, const char *local) { return is_cloud(L) ? cloud_get(L, name, local) : !strcmp(L->type, "smb") ? smb_get(L, name, local) : folder_get(L, name, local); }

static int by_name_desc(const void *a, const void *b) { return strcmp(*(const char *const *)b, *(const char *const *)a); }

/* keep the newest N at this location; the time is in the name, so the names sort the files */
static void prune(const loc_t *L, int keep)
{
	if (keep <= 0) return;
	char err[200];
	cJSON *l = loc_list(L, err, sizeof err);
	if (!l) { LOGW(TAG, "cannot list %s to prune: %s", L->name, err); return; }
	int n = cJSON_GetArraySize(l);
	const char **names = calloc((size_t)(n > 0 ? n : 1), sizeof *names);
	int k = 0;
	cJSON *f;
	cJSON_ArrayForEach(f, l) names[k++] = pf_json_str(f, "name", "");
	qsort(names, (size_t)k, sizeof *names, by_name_desc);
	for (int i = keep; i < k; i++) {
		if (loc_del(L, names[i]) == 0) LOGI(TAG, "pruned %s from %s", names[i], L->name);
		else LOGW(TAG, "could not remove %s from %s", names[i], L->name);
	}
	free(names);
	cJSON_Delete(l);
}

/* ---- the worker ---------------------------------------------------------------------------- */

static void *backup_thread(void *arg)
{
	(void)arg;
	loc_t all[MAX_LOC];
	int n = locations(all, MAX_LOC);
	char name[128], local[700], err[240], sz[32];
	archive_name(name, sizeof name, pf_wall());
	snprintf(local, sizeof local, "%s/%s", g.work, name);
	say(false, "Making the backup");
	if (pf_backup_make(local, err, sizeof err)) {
		say(true, "Backup failed: %s", err);
		file_last(false, name, 0, NULL);
		if (g.fail_streak++ == 0) pf_events_emit("Backup_Failed", "Backup failed", "%s", err);
		goto done;
	}
	long size = file_size(local);
	fmt_size(size, sz, sizeof sz);
	cJSON *results = cJSON_CreateObject();
	int sent = 0, tried = 0;
	char failed[400] = "";
	for (int i = 0; i < n; i++) {
		if (!all[i].enabled) continue;
		tried++;
		say(false, "Sending %s (%s) to %s", name, sz, all[i].name);
		cJSON *r = cJSON_AddObjectToObject(results, all[i].id);
		if (loc_put(&all[i], local, name, err, sizeof err)) {
			LOGW(TAG, "%s: %s", all[i].name, err);
			cJSON_AddBoolToObject(r, "ok", false);
			cJSON_AddStringToObject(r, "message", err);
			size_t fl = strlen(failed);
			snprintf(failed + fl, sizeof failed - fl, "%s%s: %.120s", fl ? "; " : "", all[i].name, err);
		} else {
			cJSON_AddBoolToObject(r, "ok", true);
			cJSON_AddStringToObject(r, "message", "");
			sent++;
		}
	}
	unlink(local);
	bool ok = tried > 0 && sent == tried;
	file_last(ok, name, size, results);
	if (ok) {
		say(false, "Backed up %s (%s) to %d location%s", name, sz, sent, sent == 1 ? "" : "s");
		pf_events_emit("Backup_Done", "Backup done", "%s, %s, sent to %d location%s.", name, sz, sent, sent == 1 ? "" : "s");
	} else if (tried == 0) {
		say(true, "Backup made but no location is switched on");
		if (g.fail_streak++ == 0) pf_events_emit("Backup_Failed", "Backup failed", "No backup location is switched on.");
	} else {
		say(true, "Backup sent to %d of %d: %s", sent, tried, failed);
		if (g.fail_streak++ == 0) pf_events_emit("Backup_Failed", "Backup incomplete", "Sent to %d of %d locations. %s", sent, tried, failed);
	}
	for (int i = 0; i < n; i++) if (all[i].enabled) prune(&all[i], (int)pf_set_num("backup.keep", 8));
done:
	pthread_mutex_lock(&g.mu); g.busy = false; pthread_mutex_unlock(&g.mu);
	return NULL;
}

static int start_worker(void *(*fn)(void *), char *err, size_t n)
{
	pthread_mutex_lock(&g.mu);
	if (g.busy) { pthread_mutex_unlock(&g.mu); snprintf(err, n, "a backup is already running"); return -1; }
	g.busy = true;
	pthread_mutex_unlock(&g.mu);
	pthread_t t;
	pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&t, &a, fn, NULL);
	pthread_attr_destroy(&a);
	if (rc) { pthread_mutex_lock(&g.mu); g.busy = false; pthread_mutex_unlock(&g.mu); snprintf(err, n, "cannot start the worker"); return -1; }
	return 0;
}

static int enabled_count(void)
{
	loc_t all[MAX_LOC];
	int n = locations(all, MAX_LOC), k = 0;
	for (int i = 0; i < n; i++) if (all[i].enabled) k++;
	return k;
}

int pf_backup_run(char *err, size_t n)
{
	if (enabled_count() == 0) { snprintf(err, n, "add a backup location first"); return -1; }
	return start_worker(backup_thread, err, n);
}

/* ---- restore ------------------------------------------------------------------------------- */

static bool grill_idle(void)
{
	pf_mode m = pf_status_mode();
	return m == PF_MODE_STOP || m == PF_MODE_MONITOR || m == PF_MODE_ERROR;
}

/* Stage it, mark it, and restart: the next start of the daemon puts it in place before the
 * database is opened. The grill is stopped, so a restart costs a few seconds of the web page. */
static int stage_and_restart(const char *archive, char *err, size_t n)
{
	char stage[600], pending[600];
	snprintf(stage, sizeof stage, "%s/restore", g.work);
	snprintf(pending, sizeof pending, "%s/restore.pending", g.work);
	if (pf_backup_stage(archive, stage, err, n)) return -1;
	if (pf_write_file_atomic(pending, stage, strlen(stage))) { snprintf(err, n, "cannot mark the restore"); rm_rf(stage); return -1; }
	say(false, "Backup checked; restarting to put it in place");
	pf_events_emit("Backup_Restore", "Restoring a backup", "The grill restarts and comes back with the backup's settings, tuning, recipes and cooks.");
	if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "BACKUP_RESTORE", "restoring a backup; restarting");
	/* in the simulator there is nothing to restart us, so the staged restore waits for the next run */
	if (!g.sim) { pf_sleep_ms(500); raise(SIGTERM); }
	return 0;
}

static char g_restore_name[128], g_restore_loc[24];
static void *restore_thread(void *arg)
{
	(void)arg;
	char local[700], err[240];
	snprintf(local, sizeof local, "%s/restore-download.tar.gz", g.work);
	loc_t all[MAX_LOC];
	int n = locations(all, MAX_LOC);
	/* the named location, or whichever has it -- a folder or a share before a cloud, being
	 * nearer */
	static const char *pref[] = { "folder", "smb", "onedrive", "gdrive" };
	bool got = false;
	for (int p = 0; p < 4 && !got; p++) for (int i = 0; i < n && !got; i++) {
		if (!all[i].enabled || strcmp(all[i].type, pref[p])) continue;
		if (g_restore_loc[0] && strcmp(all[i].id, g_restore_loc)) continue;
		say(false, "Fetching %s from %s", g_restore_name, all[i].name);
		if (loc_get(&all[i], g_restore_name, local) == 0) got = true;
	}
	if (!got) {
		say(true, "Could not fetch %s from any location", g_restore_name);
		pf_events_emit("Backup_Failed", "Restore failed", "Could not fetch %s.", g_restore_name);
		goto done;
	}
	if (stage_and_restart(local, err, sizeof err)) {
		say(true, "Restore refused: %s", err);
		pf_events_emit("Backup_Failed", "Restore failed", "%s", err);
	}
	unlink(local);
done:
	pthread_mutex_lock(&g.mu); g.busy = false; pthread_mutex_unlock(&g.mu);
	return NULL;
}

int pf_backup_restore_named(const char *name, const char *loc_id, char *err, size_t n)
{
	if (!grill_idle()) { snprintf(err, n, "stop the grill first"); return -1; }
	if (!name || !is_ours(name)) { snprintf(err, n, "not a PiFire backup name"); return -1; }
	if (enabled_count() == 0) { snprintf(err, n, "no backup location is switched on"); return -1; }
	pf_strlcpy(g_restore_name, name, sizeof g_restore_name);
	pf_strlcpy(g_restore_loc, loc_id ? loc_id : "", sizeof g_restore_loc);
	return start_worker(restore_thread, err, n);
}

int pf_backup_restore_bytes(const void *data, size_t len, char *err, size_t n)
{
	if (!grill_idle()) { snprintf(err, n, "stop the grill first"); return -1; }
	if (len < 64 || ((const unsigned char *)data)[0] != 0x1f || ((const unsigned char *)data)[1] != 0x8b) { snprintf(err, n, "that is not a .tar.gz backup"); return -1; }
	pthread_mutex_lock(&g.mu);
	if (g.busy) { pthread_mutex_unlock(&g.mu); snprintf(err, n, "a backup is already running"); return -1; }
	g.busy = true;
	pthread_mutex_unlock(&g.mu);
	char local[700];
	snprintf(local, sizeof local, "%s/restore-upload.tar.gz", g.work);
	int rc = pf_write_file_atomic(local, data, len) ? -1 : 0;
	if (rc) snprintf(err, n, "cannot save the upload");
	else rc = stage_and_restart(local, err, n);
	unlink(local);
	pthread_mutex_lock(&g.mu); g.busy = false; pthread_mutex_unlock(&g.mu);
	return rc;
}

int pf_backup_apply_staged(const char *data_dir, const char *config_path)
{
	char work[560], pending[600], stage[600] = "";
	snprintf(work, sizeof work, "%s/backup", data_dir);
	snprintf(pending, sizeof pending, "%s/restore.pending", work);
	char *txt = pf_read_file(pending, NULL);
	if (!txt) return 0;
	pf_strlcpy(stage, txt, sizeof stage);
	free(txt);
	unlink(pending);   /* one attempt, whatever happens: a restore that loops is worse than one that failed */
	char from[900], to[900];
	snprintf(from, sizeof from, "%s/manifest.json", stage);
	if (!pf_file_exists(from)) { LOGW(TAG, "a restore was marked but %s holds no backup", stage); return -1; }
	LOGW(TAG, "restoring a backup from %s", stage);

	snprintf(from, sizeof from, "%s/settings.json", stage);
	if (copy_file(from, config_path)) { LOGE(TAG, "cannot write %s", config_path); return -1; }

	snprintf(from, sizeof from, "%s/pifire.db", stage);
	snprintf(to, sizeof to, "%s/pifire.db", data_dir);
	{
		/* the journal files belong to the old database and must not meet the new one */
		char j[920];
		snprintf(j, sizeof j, "%s-wal", to); unlink(j);
		snprintf(j, sizeof j, "%s-shm", to); unlink(j);
	}
	if (copy_file(from, to)) { LOGE(TAG, "cannot write %s", to); return -1; }

	/* the cook files: the backup's set, whole */
	char cdir[600], sdir[620];
	snprintf(cdir, sizeof cdir, "%s/cookfiles", data_dir);
	snprintf(sdir, sizeof sdir, "%s/cookfiles", stage);
	DIR *d = opendir(cdir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (l > 5 && !strcmp(e->d_name + l - 5, ".json")) { char p[900]; snprintf(p, sizeof p, "%s/%s", cdir, e->d_name); unlink(p); }
	}
	if (d) closedir(d);
	pf_mkdir_p(cdir);
	int cooks = 0;
	d = opendir(sdir);
	while (d && (e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
		snprintf(from, sizeof from, "%s/%s", sdir, e->d_name);
		snprintf(to, sizeof to, "%s/%s", cdir, e->d_name);
		if (copy_file(from, to) == 0) cooks++;
	}
	if (d) closedir(d);
	rm_rf(stage);
	LOGW(TAG, "backup restored: settings, database and %d cook file%s", cooks, cooks == 1 ? "" : "s");
	/* the event goes in once the database is open; main() reads this marker */
	char done[600];
	snprintf(done, sizeof done, "%s/restored", work);
	pf_write_file_atomic(done, "1", 1);
	return 1;
}

/* ---- status, listing, test ----------------------------------------------------------------- */

static int by_item_name_desc(const void *a, const void *b)
{
	return strcmp(pf_json_str(*(cJSON *const *)b, "name", ""), pf_json_str(*(cJSON *const *)a, "name", ""));
}

/* every enabled location's listing, merged by name: one row per backup, saying where it is */
cJSON *pf_backup_list(char *err, size_t n)
{
	loc_t all[MAX_LOC];
	int nl = locations(all, MAX_LOC);
	cJSON *merged = cJSON_CreateObject();
	int reached = 0;
	err[0] = 0;
	for (int i = 0; i < nl; i++) {
		if (!all[i].enabled) continue;
		char e[240];
		cJSON *l = loc_list(&all[i], e, sizeof e);
		if (!l) { size_t el = strlen(err); snprintf(err + el, n - el, "%s%s: %.120s", el ? "; " : "", all[i].name, e); continue; }
		reached++;
		cJSON *f;
		cJSON_ArrayForEach(f, l) {
			const char *name = pf_json_str(f, "name", "");
			cJSON *row = cJSON_GetObjectItem(merged, name);
			if (!row) {
				row = cJSON_AddObjectToObject(merged, name);
				cJSON_AddStringToObject(row, "name", name);
				cJSON_AddNumberToObject(row, "size", pf_json_num(f, "size", 0));
				cJSON_AddNumberToObject(row, "ts", pf_json_num(f, "ts", 0) > 0 ? pf_json_num(f, "ts", 0) : ts_from_name(name));
				cJSON_AddArrayToObject(row, "locations");
			}
			cJSON_AddItemToArray(cJSON_GetObjectItem(row, "locations"), cJSON_CreateString(all[i].id));
		}
		cJSON_Delete(l);
	}
	if (!reached && nl > 0) { cJSON_Delete(merged); if (!err[0]) snprintf(err, n, "no backup location is switched on"); return NULL; }
	int k = cJSON_GetArraySize(merged);
	cJSON **items = calloc((size_t)(k > 0 ? k : 1), sizeof *items);
	for (int i = 0; i < k; i++) items[i] = cJSON_DetachItemFromArray(merged, 0);
	cJSON_Delete(merged);
	qsort(items, (size_t)k, sizeof *items, by_item_name_desc);
	cJSON *out = cJSON_CreateArray();
	for (int i = 0; i < k; i++) cJSON_AddItemToArray(out, items[i]);
	free(items);
	return out;
}

int pf_backup_test(const char *loc_id, char *msg, size_t n)
{
	loc_t L;
	if (!find_loc(loc_id, &L)) { snprintf(msg, n, "no such location"); return -1; }
	if (!strcmp(L.type, "smb")) g.have_smb = smb_have();
	char err[240];
	cJSON *l = loc_list(&L, err, sizeof err);
	if (!l) { snprintf(msg, n, "%s", err); return -1; }
	int k = cJSON_GetArraySize(l);
	cJSON_Delete(l);
	snprintf(msg, n, "Reached %s: %d backup%s there.", L.name, k, k == 1 ? "" : "s");
	return 0;
}

/* when the schedule next fires, from what it says and when the last one ran */
static double next_due(double last)
{
	char sched[16];
	pf_set_str("backup.schedule", sched, sizeof sched, "off");
	if (!strcmp(sched, "off") || enabled_count() == 0) return 0;
	int hour = (int)pf_set_num("backup.hour", 3);
	int wday = (int)pf_set_num("backup.weekday", 0);
	int mday = (int)pf_set_num("backup.monthday", 1);
	if (hour < 0 || hour > 23) hour = 3;
	/* walk forward day by day from the last run -- or from now, for a schedule that has never run,
	 * so turning one on does not fire it on the spot -- to the first matching slot after it */
	double after = last > 0 ? last : pf_wall();
	time_t base = (time_t)after;
	struct tm tm; localtime_r(&base, &tm);
	for (int i = 0; i < 40; i++) {
		struct tm c = tm;
		c.tm_mday += i; c.tm_hour = hour; c.tm_min = 0; c.tm_sec = 0; c.tm_isdst = -1;
		time_t t = mktime(&c);
		localtime_r(&t, &c);
		bool matches = !strcmp(sched, "daily") || (!strcmp(sched, "weekly") && c.tm_wday == wday) || (!strcmp(sched, "monthly") && c.tm_mday == mday);
		if (matches && (double)t > after) return (double)t;
	}
	return 0;
}

cJSON *pf_backup_status_json(void)
{
	loc_t all[MAX_LOC];
	int nl = locations(all, MAX_LOC);
	pthread_mutex_lock(&g.mu);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "busy", g.busy);
	cJSON_AddStringToObject(o, "message", g.message);
	cJSON_AddBoolToObject(o, "error", g.msg_err);
	cJSON *last = cJSON_AddObjectToObject(o, "last");
	cJSON_AddNumberToObject(last, "ts", g.last_ts);
	cJSON_AddStringToObject(last, "name", g.last_name);
	cJSON_AddNumberToObject(last, "size", (double)g.last_size);
	cJSON_AddBoolToObject(last, "ok", g.last_ok);
	cJSON *results = cJSON_Parse(g.last_results);
	cJSON_AddItemToObject(last, "results", results ? results : cJSON_CreateObject());
	if (g.dev.pending) {
		cJSON *p = cJSON_AddObjectToObject(o, "pending");
		cJSON_AddStringToObject(p, "loc", g.dev.loc);
		cJSON_AddStringToObject(p, "user_code", g.dev.user_code);
		cJSON_AddStringToObject(p, "url", g.dev.url);
		cJSON_AddNumberToObject(p, "expires", g.dev.expires);
	}
	double last_good_ts = g.last_good_ts;
	pthread_mutex_unlock(&g.mu);
	cJSON *locs = cJSON_AddArrayToObject(o, "locations");
	for (int i = 0; i < nl; i++) {
		cJSON *L = cJSON_CreateObject();
		cJSON_AddStringToObject(L, "id", all[i].id);
		cJSON_AddStringToObject(L, "type", all[i].type);
		cJSON_AddStringToObject(L, "name", all[i].name);
		cJSON_AddBoolToObject(L, "enabled", all[i].enabled);
		if (is_cloud(&all[i])) cJSON_AddBoolToObject(L, "connected", connected(&all[i]));
		cJSON_AddItemToArray(locs, L);
	}
	{
		char id[256];
		cJSON *cl = cJSON_AddObjectToObject(o, "clients");
		cJSON_AddBoolToObject(cl, "gdrive", builtin_client("gdrive", id, sizeof id, NULL, 0));
		cJSON_AddBoolToObject(cl, "onedrive", builtin_client("onedrive", id, sizeof id, NULL, 0));
	}
	cJSON_AddNumberToObject(o, "next_ts", next_due(last_good_ts));
	/* Found once at start and remembered -- except that "not there" is worth a second look now and
	 * then: an upgrade used to restart the daemon before it installed smbclient, and the page said
	 * it was missing until the next restart. */
	if (!g.have_smb && !g.sim && pf_now() - g.last_smb_check > 30) { g.last_smb_check = pf_now(); g.have_smb = smb_have(); }
	cJSON_AddBoolToObject(o, "smbclient", g.have_smb);
	return o;
}

/* ---- lifecycle ----------------------------------------------------------------------------- */

/* The page before this one had one destination in backup.destination with its fields beside it.
 * Turned into the first location, once, so nothing set up there is lost. */
static void migrate_single_destination(void)
{
	char dest[16];
	pf_set_str("backup.destination", dest, sizeof dest, "");
	cJSON *have = pf_set_dup("backup.locations");
	bool any = cJSON_IsArray(have) && cJSON_GetArraySize(have) > 0;
	cJSON_Delete(have);
	if (any || !dest[0] || !strcmp(dest, "off")) return;
	cJSON *L = cJSON_CreateObject();
	cJSON_AddStringToObject(L, "id", "loc-1");
	cJSON_AddStringToObject(L, "type", dest);
	cJSON_AddBoolToObject(L, "enabled", true);
	char v[256];
	if (!strcmp(dest, "gdrive")) {
		cJSON_AddStringToObject(L, "name", "Google Drive");
		pf_set_str("backup.gdrive.client_id", v, sizeof v, ""); cJSON_AddStringToObject(L, "client_id", v);
		pf_set_str("backup.gdrive.client_secret", v, sizeof v, ""); cJSON_AddStringToObject(L, "client_secret", v);
		pf_set_str("backup.gdrive.folder", v, sizeof v, "PiFire Backups"); cJSON_AddStringToObject(L, "cloud_folder", v);
		/* the token was filed under the old single key */
		char buf[3000];
		if (pf_db_kv_get("backup", "gdrive", buf, sizeof buf) == 0) { pf_db_kv_put("backup", "token:loc-1", buf); pf_db_kv_delete("backup", "gdrive"); }
	} else if (!strcmp(dest, "smb")) {
		cJSON_AddStringToObject(L, "name", "Network share");
		pf_set_str("backup.smb.host", v, sizeof v, ""); cJSON_AddStringToObject(L, "host", v);
		pf_set_str("backup.smb.share", v, sizeof v, ""); cJSON_AddStringToObject(L, "share", v);
		pf_set_str("backup.smb.path", v, sizeof v, "PiFire"); cJSON_AddStringToObject(L, "path", v);
		pf_set_str("backup.smb.user", v, sizeof v, ""); cJSON_AddStringToObject(L, "user", v);
		pf_set_str("backup.smb.password", v, sizeof v, ""); cJSON_AddStringToObject(L, "password", v);
	} else if (!strcmp(dest, "folder")) {
		cJSON_AddStringToObject(L, "name", "Folder");
		pf_set_str("backup.folder.path", v, sizeof v, ""); cJSON_AddStringToObject(L, "folder", v);
	} else { cJSON_Delete(L); return; }
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, L);
	pf_set_put("backup.locations", arr);
	pf_set_put_str("backup.destination", "off");
	pf_settings_save();
	LOGI(TAG, "moved the %s destination into the locations list", dest);
}

void pf_backup_init(const char *data_dir, const char *config_path, bool sim)
{
	pf_strlcpy(g.data_dir, data_dir, sizeof g.data_dir);
	pf_strlcpy(g.config, config_path, sizeof g.config);
	snprintf(g.work, sizeof g.work, "%s/backup", data_dir);
	g.sim = sim;
	pf_mkdir_p(g.work);
	migrate_single_destination();
	load_last();
	g.have_smb = sim ? true : smb_have();
	char done[600];
	snprintf(done, sizeof done, "%s/restored", g.work);
	if (pf_file_exists(done)) {
		unlink(done);
		pf_events_emit("Backup_Restored", "Backup restored", "Settings, tuning, recipes and cooks are back from the backup.");
	}
	/* anything left in the work directory is from a run that did not finish */
	DIR *d = opendir(g.work);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (is_ours(e->d_name) || !strncmp(e->d_name, "stage-", 6) || !strncmp(e->d_name, "restore-", 8)) {
			char p[900]; snprintf(p, sizeof p, "%s/%s", g.work, e->d_name);
			if (is_ours(e->d_name) || !strncmp(e->d_name, "restore-", 8)) unlink(p); else rm_rf(p);
		}
	}
	if (d) closedir(d);
	int k = enabled_count();
	LOGI(TAG, "backups: %d location%s, %s", k, k == 1 ? "" : "s", g.last_ts > 0 ? g.last_name : "none yet");
}

void pf_backup_tick(double now)
{
	if (now - g.last_check < 60) return;
	g.last_check = now;
	pthread_mutex_lock(&g.mu);
	bool busy = g.busy;
	double good = g.last_good_ts, attempt = g.last_ts;
	pthread_mutex_unlock(&g.mu);
	if (busy) return;
	/* Counted from the last backup that got everywhere, so one that failed is tried again -- an
	 * hour later, not every minute, and announced once per streak rather than every hour. A slot
	 * missed while the grill was switched off runs when it comes back. */
	double due = next_due(good);
	if (due <= 0 || pf_wall() < due) return;
	if (attempt > good && pf_wall() - attempt < 3600) return;
	char err[200];
	LOGI(TAG, "scheduled backup is due");
	if (pf_backup_run(err, sizeof err)) LOGW(TAG, "scheduled backup did not start: %s", err);
}
