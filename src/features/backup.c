/* One file, the whole picture.
 *
 * A grill accumulates things worth keeping: the settings that make it this grill, the tuning
 * library and the feed-forward it has learned over months of cooks, the recipes, the pellet
 * profiles, the notification rules, every saved cook. All of it lives on an SD card in the weather.
 * This module puts the lot in one tar.gz and sends it somewhere else -- Google Drive, a share on
 * the network, or a folder such as a USB stick -- on a schedule, and can bring it back.
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if PF_WITH_CURL
#include <curl/curl.h>
#endif

#define TAG "backup"
#define GDRIVE_SCOPE "https://www.googleapis.com/auth/drive.file"
#define GDRIVE_DEVICE_URL "https://oauth2.googleapis.com/device/code"
#define GDRIVE_TOKEN_URL "https://oauth2.googleapis.com/token"
#define GDRIVE_API "https://www.googleapis.com/drive/v3/files"
#define GDRIVE_UPLOAD "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable"

typedef enum { DEST_OFF = 0, DEST_GDRIVE, DEST_SMB, DEST_FOLDER } dest_t;

static struct {
	char data_dir[512], config[512], work[560];
	bool sim;
	pthread_mutex_t mu;
	bool busy;                 /* a backup or restore is running in the worker */
	char message[240];         /* what the worker is doing or last said */
	bool msg_err;
	/* the last backup, as filed in the database */
	double last_ts; char last_name[128]; long last_size; bool last_ok; char last_msg[200]; char last_where[16];
	double last_good_ts;       /* the last one that got there: what the schedule counts from */
	int fail_streak;           /* failures since the last success; the first of a streak is announced, the rest logged */
	bool have_smb;             /* smbclient is installed (checked at start and on a test) */
	double last_check;         /* schedule tick throttle */
	/* Google's device sign-in, while it is going on */
	struct { bool pending; char user_code[32], url[128], device_code[256]; double expires, interval; } dev;
	bool gdrive_connected;
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

static dest_t dest_from(const char *s)
{
	if (!strcmp(s, "gdrive")) return DEST_GDRIVE;
	if (!strcmp(s, "smb")) return DEST_SMB;
	if (!strcmp(s, "folder")) return DEST_FOLDER;
	return DEST_OFF;
}
static dest_t dest_now(void) { char b[16]; pf_set_str("backup.destination", b, sizeof b, "off"); return dest_from(b); }
static const char *dest_name(dest_t d) { return d == DEST_GDRIVE ? "Google Drive" : d == DEST_SMB ? "the network share" : d == DEST_FOLDER ? "the folder" : "nowhere"; }

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
	return l > 14 && !strncmp(name, "pifire-", 7) && !strcmp(name + l - 7, ".tar.gz") && !strchr(name, '/');
}

/* ---- the archive --------------------------------------------------------------------------- */

int pf_backup_make(const char *out_path, char *err, size_t n)
{
	char stage[600];
	snprintf(stage, sizeof stage, "%s/stage-%d", g.work, (int)getpid());
	rm_rf(stage);
	if (pf_mkdir_p(stage)) { snprintf(err, n, "cannot create %s", stage); return -1; }
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
	if (pf_mkdir_p(stage_dir)) { snprintf(err, n, "cannot create %s", stage_dir); return -1; }
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
	char buf[1024];
	if (pf_db_kv_get("backup", "last", buf, sizeof buf) != 0) return;
	cJSON *j = cJSON_Parse(buf);
	if (!j) return;
	g.last_ts = pf_json_num(j, "ts", 0);
	pf_strlcpy(g.last_name, pf_json_str(j, "name", ""), sizeof g.last_name);
	g.last_size = (long)pf_json_num(j, "size", 0);
	g.last_ok = pf_json_bool(j, "ok", false);
	pf_strlcpy(g.last_msg, pf_json_str(j, "message", ""), sizeof g.last_msg);
	pf_strlcpy(g.last_where, pf_json_str(j, "where", ""), sizeof g.last_where);
	g.last_good_ts = pf_json_num(j, "good_ts", g.last_ok ? g.last_ts : 0);
	cJSON_Delete(j);
}

static void file_last(bool ok, const char *name, long size, const char *where, const char *msg)
{
	pthread_mutex_lock(&g.mu);
	g.last_ts = pf_wall(); g.last_ok = ok; g.last_size = size;
	if (ok) { g.last_good_ts = g.last_ts; g.fail_streak = 0; }
	pf_strlcpy(g.last_name, name ? name : "", sizeof g.last_name);
	pf_strlcpy(g.last_msg, msg ? msg : "", sizeof g.last_msg);
	pf_strlcpy(g.last_where, where ? where : "", sizeof g.last_where);
	cJSON *j = cJSON_CreateObject();
	cJSON_AddNumberToObject(j, "ts", g.last_ts);
	cJSON_AddStringToObject(j, "name", g.last_name);
	cJSON_AddNumberToObject(j, "size", (double)size);
	cJSON_AddBoolToObject(j, "ok", ok);
	cJSON_AddStringToObject(j, "message", g.last_msg);
	cJSON_AddStringToObject(j, "where", g.last_where);
	cJSON_AddNumberToObject(j, "good_ts", g.last_good_ts);
	pthread_mutex_unlock(&g.mu);
	char *txt = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	if (txt && pf_db_handle()) pf_db_kv_put("backup", "last", txt);
	free(txt);
}

/* ---- a folder ------------------------------------------------------------------------------ */

static bool folder_path(char *out, size_t n) { pf_set_str("backup.folder.path", out, n, ""); return out[0] != 0; }

static int folder_put(const char *local, const char *name, char *err, size_t n)
{
	char dir[512], to[800];
	if (!folder_path(dir, sizeof dir)) { snprintf(err, n, "no folder is set"); return -1; }
	if (pf_mkdir_p(dir)) { snprintf(err, n, "cannot create %.150s", dir); return -1; }
	snprintf(to, sizeof to, "%s/%s", dir, name);
	if (copy_file(local, to)) { snprintf(err, n, "cannot write to %.150s: %s", dir, strerror(errno)); return -1; }
	return 0;
}

static cJSON *folder_list(char *err, size_t n)
{
	char dir[512];
	if (!folder_path(dir, sizeof dir)) { snprintf(err, n, "no folder is set"); return NULL; }
	DIR *d = opendir(dir);
	if (!d) { snprintf(err, n, "cannot open %.150s: %s", dir, strerror(errno)); return NULL; }
	cJSON *arr = cJSON_CreateArray();
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!is_ours(e->d_name)) continue;
		char p[800]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
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

static int folder_del(const char *name) { char dir[512], p[800]; if (!folder_path(dir, sizeof dir)) return -1; snprintf(p, sizeof p, "%s/%s", dir, name); return unlink(p); }
static int folder_get(const char *name, const char *local) { char dir[512], p[800]; if (!folder_path(dir, sizeof dir)) return -1; snprintf(p, sizeof p, "%s/%s", dir, name); return copy_file(p, local); }

/* ---- a network share, through smbclient --------------------------------------------------- */

static bool smb_have(void) { const char *argv[] = { "smbclient", "-V", NULL }; return run(argv, NULL, 0, 10) == 0; }

/* //host/share, a credentials file the way smbclient wants it (never the password on a command
 * line, where ps would show it), and the folder within the share */
static int smb_prep(char *svc, size_t sn, char *auth, size_t an, char *dir, size_t dn, char *err, size_t en)
{
	char host[128], share[128], user[96], pass[128];
	pf_set_str("backup.smb.host", host, sizeof host, "");
	pf_set_str("backup.smb.share", share, sizeof share, "");
	pf_set_str("backup.smb.user", user, sizeof user, "");
	pf_set_str("backup.smb.password", pass, sizeof pass, "");
	pf_set_str("backup.smb.path", dir, dn, "PiFire");
	if (!host[0] || !share[0]) { snprintf(err, en, "the share needs a host and a share name"); return -1; }
	if (!g.have_smb && !(g.have_smb = smb_have())) { snprintf(err, en, "smbclient is not installed on the grill (sudo apt install smbclient)"); return -1; }
	snprintf(svc, sn, "//%s/%s", host, share);
	snprintf(auth, an, "%s/.smbauth", g.work);
	char body[300];
	snprintf(body, sizeof body, "username=%s\npassword=%s\n", user, pass);
	if (pf_write_file_atomic(auth, body, strlen(body))) { snprintf(err, en, "cannot write the credentials file"); return -1; }
	chmod(auth, 0600);
	/* a leading slash and a trailing one both confuse smbclient's cd */
	size_t l = strlen(dir);
	while (l && dir[l - 1] == '/') dir[--l] = 0;
	if (dir[0] == '/') memmove(dir, dir + 1, strlen(dir));
	return 0;
}

static int smb_cmd(const char *cmd, char *out, size_t n, char *err, size_t en)
{
	char svc[300], auth[600], dir[256];
	if (smb_prep(svc, sizeof svc, auth, sizeof auth, dir, sizeof dir, err, en)) return -1;
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

static int smb_put(const char *local, const char *name, char *err, size_t n)
{
	/* make the folder if it is not there; smbclient says so and fails when it already exists,
	 * which is not a failure */
	char svc[300], auth[600], dir[256], out[4096];
	if (smb_prep(svc, sizeof svc, auth, sizeof auth, dir, sizeof dir, err, n)) return -1;
	if (dir[0]) {
		char mk[400]; snprintf(mk, sizeof mk, "mkdir \"%s\"", dir);
		const char *argv[] = { "smbclient", svc, "-A", auth, "-c", mk, NULL };
		run(argv, out, sizeof out, 60);
	}
	char cmd[1400];
	snprintf(cmd, sizeof cmd, "put \"%s\" \"%s\"", local, name);
	return smb_cmd(cmd, out, sizeof out, err, n);
}

static cJSON *smb_list(char *err, size_t n)
{
	size_t cap = 65536;
	char *out = malloc(cap);
	if (!out) { snprintf(err, n, "out of memory"); return NULL; }
	if (smb_cmd("ls pifire-*.tar.gz", out, cap, err, n) != 0 && !strstr(out, "blocks")) { free(out); return NULL; }
	cJSON *arr = cJSON_CreateArray();
	/* smbclient's listing:  "  pifire-x-20260926-0300.tar.gz   A   1843201  Fri Sep 26 03:00:12 2026" */
	char *save = NULL;
	for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char name[200], attr[16]; long size = 0;
		if (sscanf(line, " %199s %15s %ld", name, attr, &size) != 3 || !is_ours(name)) continue;
		cJSON *f = cJSON_CreateObject();
		cJSON_AddStringToObject(f, "name", name);
		cJSON_AddNumberToObject(f, "size", (double)size);
		/* the time is in the name, which is more reliable than parsing a locale date */
		struct tm tm = { 0 };
		const char *stamp = strrchr(name, '-');   /* -HHMM.tar.gz */
		const char *date = stamp ? stamp - 8 : NULL;   /* YYYYMMDD */
		double ts = 0;
		if (date && date > name && sscanf(date, "%4d%2d%2d-%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min) == 5) {
			tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = -1;
			ts = (double)mktime(&tm);
		}
		cJSON_AddNumberToObject(f, "ts", ts);
		cJSON_AddItemToArray(arr, f);
	}
	free(out);
	return arr;
}

static int smb_del(const char *name) { char cmd[300], out[2048], err[200]; snprintf(cmd, sizeof cmd, "del \"%s\"", name); return smb_cmd(cmd, out, sizeof out, err, sizeof err); }
static int smb_get(const char *name, const char *local) { char cmd[1000], out[2048], err[200]; snprintf(cmd, sizeof cmd, "get \"%s\" \"%s\"", name, local); return smb_cmd(cmd, out, sizeof out, err, sizeof err); }

/* ---- Google Drive -------------------------------------------------------------------------- */
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

/* One request. method GET/POST/PUT/DELETE/PATCH; body sent as given with its content type; the
 * bearer token when there is one; the reply body handed back, the status returned. -1 on a
 * transport failure with err filled. */
static long http(const char *method, const char *url, const char *bearer, const char *ctype, const char *body, size_t blen,
                 const char *upload_path, char *hdr_out, size_t hdr_n, membuf *reply, char *err, size_t en)
{
	CURL *c = curl_easy_init();
	if (!c) { snprintf(err, en, "curl init failed"); return -1; }
	struct curl_slist *h = NULL;
	char auth[2400];
	if (bearer) { snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer); h = curl_slist_append(h, auth); }
	char ct[128];
	if (ctype) { snprintf(ct, sizeof ct, "Content-Type: %s", ctype); h = curl_slist_append(h, ct); }
	FILE *up = NULL;
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "pifired/" PF_VERSION);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, upload_path ? 900L : 60L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
	if (h) curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, reply);
	membuf hdrs = { 0 };
	if (hdr_out) { curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, mem_cb); curl_easy_setopt(c, CURLOPT_HEADERDATA, &hdrs); }
	if (upload_path) {
		up = fopen(upload_path, "rb");
		if (!up) { snprintf(err, en, "cannot read %s", upload_path); curl_slist_free_all(h); curl_easy_cleanup(c); return -1; }
		curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
		curl_easy_setopt(c, CURLOPT_READDATA, up);
		curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_size(upload_path));
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
	} else if (body) {
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)blen);
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
	} else if (strcmp(method, "GET")) {
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
	}
	CURLcode rc = curl_easy_perform(c);
	long status = 0;
	if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	else snprintf(err, en, "%s", curl_easy_strerror(rc));
	if (hdr_out) { pf_strlcpy(hdr_out, hdrs.buf ? hdrs.buf : "", hdr_n); free(hdrs.buf); }
	if (up) fclose(up);
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
	return rc == CURLE_OK ? status : -1;
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

/* the API's own account of what went wrong, when it gives one */
static void api_error(membuf *m, long status, char *err, size_t n)
{
	cJSON *j = m->buf ? cJSON_Parse(m->buf) : NULL;
	const char *msg = j ? pf_json_str(j, "error.message", "") : "";
	if (!msg[0] && j) msg = pf_json_str(j, "error_description", "");
	if (!msg[0] && j) msg = pf_json_str(j, "error", "");
	snprintf(err, n, "Google said %ld%s%s", status, msg[0] ? ": " : "", msg);
	cJSON_Delete(j);
}

static bool gdrive_client(char *id, size_t in, char *secret, size_t sn)
{
	pf_set_str("backup.gdrive.client_id", id, in, "");
	pf_set_str("backup.gdrive.client_secret", secret, sn, "");
	return id[0] && secret[0];
}

static bool gdrive_refresh_token(char *out, size_t n)
{
	char buf[2048];
	out[0] = 0;
	if (pf_db_kv_get("backup", "gdrive", buf, sizeof buf) != 0) return false;
	cJSON *j = cJSON_Parse(buf);
	if (!j) return false;
	pf_strlcpy(out, pf_json_str(j, "refresh_token", ""), n);
	cJSON_Delete(j);
	return out[0] != 0;
}

/* an access token, fresh, from the refresh token on file */
static int gdrive_access(char *token, size_t n, char *err, size_t en)
{
	char id[256], secret[256], refresh[512];
	if (!gdrive_client(id, sizeof id, secret, sizeof secret)) { snprintf(err, en, "Google Drive needs a client ID and secret"); return -1; }
	if (!gdrive_refresh_token(refresh, sizeof refresh)) { snprintf(err, en, "Google Drive is not connected"); return -1; }
	char *eid = url_escape(id), *esec = url_escape(secret), *eref = url_escape(refresh);
	char body[1600];
	snprintf(body, sizeof body, "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token", eid ? eid : "", esec ? esec : "", eref ? eref : "");
	free(eid); free(esec); free(eref);
	membuf m = { 0 };
	long st = http("POST", GDRIVE_TOKEN_URL, NULL, "application/x-www-form-urlencoded", body, strlen(body), NULL, NULL, 0, &m, err, en);
	if (st < 0) { free(m.buf); return -1; }
	if (st != 200) { api_error(&m, st, err, en); free(m.buf); if (st == 400 || st == 401) { LOGW(TAG, "Google refused the refresh token; disconnecting"); pf_db_kv_delete("backup", "gdrive"); pthread_mutex_lock(&g.mu); g.gdrive_connected = false; pthread_mutex_unlock(&g.mu); } return -1; }
	cJSON *j = cJSON_Parse(m.buf); free(m.buf);
	pf_strlcpy(token, j ? pf_json_str(j, "access_token", "") : "", n);
	cJSON_Delete(j);
	if (!token[0]) { snprintf(err, en, "Google returned no access token"); return -1; }
	return 0;
}

/* the folder the backups go in, made if it is not there */
static int gdrive_folder(const char *token, char *id, size_t n, bool create, char *err, size_t en)
{
	char name[96];
	pf_set_str("backup.gdrive.folder", name, sizeof name, "PiFire Backups");
	if (!name[0]) pf_strlcpy(name, "PiFire Backups", sizeof name);
	char q[400];
	snprintf(q, sizeof q, "name='%s' and mimeType='application/vnd.google-apps.folder' and trashed=false", name);
	char *eq = url_escape(q);
	char url[900];
	snprintf(url, sizeof url, GDRIVE_API "?q=%s&fields=files(id,name)&spaces=drive", eq ? eq : "");
	free(eq);
	membuf m = { 0 };
	long st = http("GET", url, token, NULL, NULL, 0, NULL, NULL, 0, &m, err, en);
	if (st < 0) { free(m.buf); return -1; }
	if (st != 200) { api_error(&m, st, err, en); free(m.buf); return -1; }
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
	membuf r = { 0 };
	st = http("POST", GDRIVE_API "?fields=id", token, "application/json", body, strlen(body), NULL, NULL, 0, &r, err, en);
	free(body);
	if (st < 0) { free(r.buf); return -1; }
	if (st != 200) { api_error(&r, st, err, en); free(r.buf); return -1; }
	j = cJSON_Parse(r.buf); free(r.buf);
	pf_strlcpy(id, j ? pf_json_str(j, "id", "") : "", n);
	cJSON_Delete(j);
	if (!id[0]) { snprintf(err, en, "Google made no folder"); return -1; }
	return 0;
}

static int gdrive_put(const char *local, const char *name, char *err, size_t n)
{
	char token[2048], folder[128];
	if (gdrive_access(token, sizeof token, err, n)) return -1;
	if (gdrive_folder(token, folder, sizeof folder, true, err, n)) return -1;
	/* resumable upload: the metadata first, then the bytes to the session it hands back */
	cJSON *meta = cJSON_CreateObject();
	cJSON_AddStringToObject(meta, "name", name);
	cJSON *parents = cJSON_AddArrayToObject(meta, "parents");
	cJSON_AddItemToArray(parents, cJSON_CreateString(folder));
	char *body = cJSON_PrintUnformatted(meta); cJSON_Delete(meta);
	membuf m = { 0 };
	char hdrs[8192];
	long st = http("POST", GDRIVE_UPLOAD, token, "application/json; charset=UTF-8", body, strlen(body), NULL, hdrs, sizeof hdrs, &m, err, n);
	free(body);
	if (st < 0) { free(m.buf); return -1; }
	if (st != 200) { api_error(&m, st, err, n); free(m.buf); return -1; }
	free(m.buf);
	char session[1024] = "";
	for (char *p = hdrs; (p = strstr(p, "ocation:")); p += 8) {
		if (p > hdrs && (p[-1] == 'L' || p[-1] == 'l')) {
			p += 8; while (*p == ' ') p++;
			size_t k = 0; while (p[k] && p[k] != '\r' && p[k] != '\n' && k < sizeof session - 1) { session[k] = p[k]; k++; }
			session[k] = 0;
			break;
		}
	}
	if (!session[0]) { snprintf(err, n, "Google gave no upload session"); return -1; }
	membuf r = { 0 };
	st = http("PUT", session, token, "application/gzip", NULL, 0, local, NULL, 0, &r, err, n);
	if (st < 0) { free(r.buf); return -1; }
	if (st != 200 && st != 201) { api_error(&r, st, err, n); free(r.buf); return -1; }
	free(r.buf);
	return 0;
}

static cJSON *gdrive_list(char *err, size_t n)
{
	char token[2048], folder[128];
	if (gdrive_access(token, sizeof token, err, n)) return NULL;
	int fr = gdrive_folder(token, folder, sizeof folder, false, err, n);
	if (fr < 0) return NULL;
	if (fr > 0) return cJSON_CreateArray();
	char q[300];
	snprintf(q, sizeof q, "'%s' in parents and trashed=false", folder);
	char *eq = url_escape(q);
	char url[900];
	snprintf(url, sizeof url, GDRIVE_API "?q=%s&fields=files(id,name,size,createdTime)&orderBy=createdTime%%20desc&pageSize=100", eq ? eq : "");
	free(eq);
	membuf m = { 0 };
	long st = http("GET", url, token, NULL, NULL, 0, NULL, NULL, 0, &m, err, n);
	if (st < 0) { free(m.buf); return NULL; }
	if (st != 200) { api_error(&m, st, err, n); free(m.buf); return NULL; }
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
		struct tm tm = { 0 };
		const char *ct = pf_json_str(f, "createdTime", "");
		double ts = 0;
		if (sscanf(ct, "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
			tm.tm_year -= 1900; tm.tm_mon -= 1;
			ts = (double)timegm(&tm);
		}
		cJSON_AddNumberToObject(o, "ts", ts);
		cJSON_AddItemToArray(arr, o);
	}
	cJSON_Delete(j);
	return arr;
}

static int gdrive_id_for(const char *name, char *id, size_t n)
{
	char err[200];
	cJSON *l = gdrive_list(err, sizeof err);
	if (!l) return -1;
	id[0] = 0;
	cJSON *f;
	cJSON_ArrayForEach(f, l) if (!strcmp(pf_json_str(f, "name", ""), name)) { pf_strlcpy(id, pf_json_str(f, "id", ""), n); break; }
	cJSON_Delete(l);
	return id[0] ? 0 : -1;
}

static int gdrive_del(const char *name)
{
	char token[2048], id[128], err[200], url[300];
	if (gdrive_access(token, sizeof token, err, sizeof err) || gdrive_id_for(name, id, sizeof id)) return -1;
	snprintf(url, sizeof url, GDRIVE_API "/%s", id);
	membuf m = { 0 };
	long st = http("DELETE", url, token, NULL, NULL, 0, NULL, NULL, 0, &m, err, sizeof err);
	free(m.buf);
	return st == 204 || st == 200 ? 0 : -1;
}

static int gdrive_get(const char *name, const char *local)
{
	char token[2048], id[128], err[200], url[300];
	if (gdrive_access(token, sizeof token, err, sizeof err) || gdrive_id_for(name, id, sizeof id)) return -1;
	snprintf(url, sizeof url, GDRIVE_API "/%s?alt=media", id);
	membuf m = { 0 };
	long st = http("GET", url, token, NULL, NULL, 0, NULL, NULL, 0, &m, err, sizeof err);
	int rc = st == 200 && m.buf ? pf_write_file_atomic(local, m.buf, m.len) : -1;
	free(m.buf);
	return rc;
}

/* Google's sign-in for devices without a browser: ask for a code, show it, poll until the person
 * has typed it into google.com/device on their phone. The refresh token that comes back is kept
 * in the database, so a restored backup brings the connection with it. */
static void *gdrive_poll_thread(void *arg)
{
	(void)arg;
	char id[256], secret[256], code[256];
	double interval, expires;
	pthread_mutex_lock(&g.mu);
	pf_strlcpy(code, g.dev.device_code, sizeof code);
	interval = g.dev.interval; expires = g.dev.expires;
	pthread_mutex_unlock(&g.mu);
	gdrive_client(id, sizeof id, secret, sizeof secret);
	char *eid = url_escape(id), *esec = url_escape(secret), *ecode = url_escape(code);
	char body[1400];
	snprintf(body, sizeof body, "client_id=%s&client_secret=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code", eid ? eid : "", esec ? esec : "", ecode ? ecode : "");
	free(eid); free(esec); free(ecode);
	while (pf_wall() < expires) {
		pf_sleep_ms((unsigned)(interval * 1000));
		pthread_mutex_lock(&g.mu);
		bool still = g.dev.pending;
		pthread_mutex_unlock(&g.mu);
		if (!still) return NULL;
		membuf m = { 0 };
		char err[200];
		long st = http("POST", GDRIVE_TOKEN_URL, NULL, "application/x-www-form-urlencoded", body, strlen(body), NULL, NULL, 0, &m, err, sizeof err);
		cJSON *j = st >= 0 && m.buf ? cJSON_Parse(m.buf) : NULL;
		free(m.buf);
		const char *e = j ? pf_json_str(j, "error", "") : "";
		if (st == 200 && j && pf_json_str(j, "refresh_token", "")[0]) {
			cJSON *keep = cJSON_CreateObject();
			cJSON_AddStringToObject(keep, "refresh_token", pf_json_str(j, "refresh_token", ""));
			cJSON_AddNumberToObject(keep, "connected", pf_wall());
			char *txt = cJSON_PrintUnformatted(keep); cJSON_Delete(keep);
			if (txt) pf_db_kv_put("backup", "gdrive", txt);
			free(txt);
			cJSON_Delete(j);
			pthread_mutex_lock(&g.mu); g.dev.pending = false; g.gdrive_connected = true; pthread_mutex_unlock(&g.mu);
			say(false, "Google Drive connected");
			pf_events_emit("Backup_Connected", "Google Drive connected", "Backups can go to Google Drive now.");
			return NULL;
		}
		if (!strcmp(e, "slow_down")) interval += 5;
		else if (strcmp(e, "authorization_pending")) {
			say(true, "Google sign-in did not finish: %s", e[0] ? e : (st < 0 ? err : "no answer"));
			cJSON_Delete(j);
			pthread_mutex_lock(&g.mu); g.dev.pending = false; pthread_mutex_unlock(&g.mu);
			return NULL;
		}
		cJSON_Delete(j);
	}
	say(true, "Google sign-in code expired before it was used");
	pthread_mutex_lock(&g.mu); g.dev.pending = false; pthread_mutex_unlock(&g.mu);
	return NULL;
}

int pf_backup_gdrive_connect(char *err, size_t n)
{
	char id[256], secret[256];
	if (!gdrive_client(id, sizeof id, secret, sizeof secret)) { snprintf(err, n, "enter the Google client ID and secret first, then save"); return -1; }
	pthread_mutex_lock(&g.mu);
	bool pending = g.dev.pending;
	pthread_mutex_unlock(&g.mu);
	if (pending) return 0;   /* the code already showing is the one to use */
	char *eid = url_escape(id), *esc = url_escape(GDRIVE_SCOPE);
	char body[800];
	snprintf(body, sizeof body, "client_id=%s&scope=%s", eid ? eid : "", esc ? esc : "");
	free(eid); free(esc);
	membuf m = { 0 };
	long st = http("POST", GDRIVE_DEVICE_URL, NULL, "application/x-www-form-urlencoded", body, strlen(body), NULL, NULL, 0, &m, err, n);
	if (st < 0) { free(m.buf); return -1; }
	if (st != 200) { api_error(&m, st, err, n); free(m.buf); return -1; }
	cJSON *j = cJSON_Parse(m.buf); free(m.buf);
	if (!j || !pf_json_str(j, "device_code", "")[0]) { cJSON_Delete(j); snprintf(err, n, "Google gave no device code"); return -1; }
	pthread_mutex_lock(&g.mu);
	g.dev.pending = true;
	pf_strlcpy(g.dev.user_code, pf_json_str(j, "user_code", ""), sizeof g.dev.user_code);
	pf_strlcpy(g.dev.url, pf_json_str(j, "verification_url", "https://www.google.com/device"), sizeof g.dev.url);
	pf_strlcpy(g.dev.device_code, pf_json_str(j, "device_code", ""), sizeof g.dev.device_code);
	g.dev.interval = pf_json_num(j, "interval", 5);
	g.dev.expires = pf_wall() + pf_json_num(j, "expires_in", 1800);
	pthread_mutex_unlock(&g.mu);
	cJSON_Delete(j);
	pthread_t t;
	pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &a, gdrive_poll_thread, NULL)) { pthread_attr_destroy(&a); pthread_mutex_lock(&g.mu); g.dev.pending = false; pthread_mutex_unlock(&g.mu); snprintf(err, n, "cannot start the sign-in"); return -1; }
	pthread_attr_destroy(&a);
	return 0;
}
#else
static int gdrive_put(const char *l, const char *nm, char *err, size_t n) { (void)l; (void)nm; snprintf(err, n, "built without libcurl"); return -1; }
static cJSON *gdrive_list(char *err, size_t n) { snprintf(err, n, "built without libcurl"); return NULL; }
static int gdrive_del(const char *name) { (void)name; return -1; }
static int gdrive_get(const char *name, const char *local) { (void)name; (void)local; return -1; }
int pf_backup_gdrive_connect(char *err, size_t n) { snprintf(err, n, "built without libcurl"); return -1; }
#endif

void pf_backup_gdrive_disconnect(void)
{
	if (pf_db_handle()) pf_db_kv_delete("backup", "gdrive");
	pthread_mutex_lock(&g.mu); g.gdrive_connected = false; g.dev.pending = false; pthread_mutex_unlock(&g.mu);
	say(false, "Google Drive disconnected");
}

/* ---- the destination, whichever it is ------------------------------------------------------ */

static int dest_put(dest_t d, const char *local, const char *name, char *err, size_t n)
{
	switch (d) {
	case DEST_GDRIVE: return gdrive_put(local, name, err, n);
	case DEST_SMB: return smb_put(local, name, err, n);
	case DEST_FOLDER: return folder_put(local, name, err, n);
	default: snprintf(err, n, "no destination is set"); return -1;
	}
}
static cJSON *dest_list(dest_t d, char *err, size_t n)
{
	switch (d) {
	case DEST_GDRIVE: return gdrive_list(err, n);
	case DEST_SMB: return smb_list(err, n);
	case DEST_FOLDER: return folder_list(err, n);
	default: snprintf(err, n, "no destination is set"); return NULL;
	}
}
static int dest_del(dest_t d, const char *name) { return d == DEST_GDRIVE ? gdrive_del(name) : d == DEST_SMB ? smb_del(name) : d == DEST_FOLDER ? folder_del(name) : -1; }
static int dest_get(dest_t d, const char *name, const char *local) { return d == DEST_GDRIVE ? gdrive_get(name, local) : d == DEST_SMB ? smb_get(name, local) : d == DEST_FOLDER ? folder_get(name, local) : -1; }

static int by_name_desc(const void *a, const void *b) { return strcmp(*(const char *const *)b, *(const char *const *)a); }

/* keep the newest N; the time is in the name, so the names sort the files */
static void prune(dest_t d, int keep)
{
	if (keep <= 0) return;
	char err[200];
	cJSON *l = dest_list(d, err, sizeof err);
	if (!l) { LOGW(TAG, "cannot list %s to prune: %s", dest_name(d), err); return; }
	int n = cJSON_GetArraySize(l);
	const char **names = calloc((size_t)(n > 0 ? n : 1), sizeof *names);
	int k = 0;
	cJSON *f;
	cJSON_ArrayForEach(f, l) names[k++] = pf_json_str(f, "name", "");
	qsort(names, (size_t)k, sizeof *names, by_name_desc);
	for (int i = keep; i < k; i++) {
		if (dest_del(d, names[i]) == 0) LOGI(TAG, "pruned %s from %s", names[i], dest_name(d));
		else LOGW(TAG, "could not remove %s from %s", names[i], dest_name(d));
	}
	free(names);
	cJSON_Delete(l);
}

/* ---- the worker ---------------------------------------------------------------------------- */

static void *backup_thread(void *arg)
{
	(void)arg;
	dest_t d = dest_now();
	char name[128], local[700], err[240];
	archive_name(name, sizeof name, pf_wall());
	snprintf(local, sizeof local, "%s/%s", g.work, name);
	say(false, "Making the backup");
	if (pf_backup_make(local, err, sizeof err)) {
		say(true, "Backup failed: %s", err);
		file_last(false, name, 0, "", err);
		if (g.fail_streak++ == 0) pf_events_emit("Backup_Failed", "Backup failed", "%s", err);
		goto done;
	}
	long size = file_size(local);
	say(false, "Sending %s to %s", name, dest_name(d));
	if (dest_put(d, local, name, err, sizeof err)) {
		say(true, "Backup could not be sent to %s: %s", dest_name(d), err);
		file_last(false, name, size, "", err);
		if (g.fail_streak++ == 0) pf_events_emit("Backup_Failed", "Backup failed", "Could not send it to %s: %s", dest_name(d), err);
		unlink(local);
		goto done;
	}
	unlink(local);
	{
		char where[16]; pf_set_str("backup.destination", where, sizeof where, "");
		file_last(true, name, size, where, "");
	}
	char sz[32];
	say(false, "Backed up %s (%s) to %s", name, fmt_size(size, sz, sizeof sz), dest_name(d));
	pf_events_emit("Backup_Done", "Backup done", "%s, %s, sent to %s.", name, sz, dest_name(d));
	prune(d, (int)pf_set_num("backup.keep", 8));
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

int pf_backup_run(char *err, size_t n)
{
	if (dest_now() == DEST_OFF) { snprintf(err, n, "choose where backups go first"); return -1; }
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

static char g_restore_name[128];
static void *restore_thread(void *arg)
{
	(void)arg;
	char local[700], err[240];
	snprintf(local, sizeof local, "%s/restore-download.tar.gz", g.work);
	dest_t d = dest_now();
	say(false, "Fetching %s from %s", g_restore_name, dest_name(d));
	if (dest_get(d, g_restore_name, local)) {
		say(true, "Could not fetch %s from %s", g_restore_name, dest_name(d));
		pf_events_emit("Backup_Failed", "Restore failed", "Could not fetch %s from %s.", g_restore_name, dest_name(d));
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

int pf_backup_restore_named(const char *name, char *err, size_t n)
{
	if (!grill_idle()) { snprintf(err, n, "stop the grill first"); return -1; }
	if (!name || !is_ours(name)) { snprintf(err, n, "not a PiFire backup name"); return -1; }
	if (dest_now() == DEST_OFF) { snprintf(err, n, "no destination is set"); return -1; }
	pf_strlcpy(g_restore_name, name, sizeof g_restore_name);
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

cJSON *pf_backup_list(char *err, size_t n)
{
	dest_t d = dest_now();
	cJSON *l = dest_list(d, err, n);
	if (!l) return NULL;
	/* newest first, by the time in the name */
	int k = cJSON_GetArraySize(l);
	if (k < 2) return l;
	cJSON **items = calloc((size_t)k, sizeof *items);
	if (!items) return l;
	for (int i = 0; i < k; i++) items[i] = cJSON_DetachItemFromArray(l, 0);
	qsort(items, (size_t)k, sizeof *items, by_item_name_desc);
	for (int i = 0; i < k; i++) cJSON_AddItemToArray(l, items[i]);
	free(items);
	return l;
}

int pf_backup_test(char *msg, size_t n)
{
	dest_t d = dest_now();
	if (d == DEST_SMB) g.have_smb = smb_have();
	char err[240];
	cJSON *l = dest_list(d, err, sizeof err);
	if (!l) { snprintf(msg, n, "%s", err); return -1; }
	int k = cJSON_GetArraySize(l);
	cJSON_Delete(l);
	snprintf(msg, n, "Reached %s: %d backup%s there.", dest_name(d), k, k == 1 ? "" : "s");
	return 0;
}

/* when the schedule next fires, from what it says and when the last one ran */
static double next_due(double last)
{
	char sched[16];
	pf_set_str("backup.schedule", sched, sizeof sched, "off");
	if (!strcmp(sched, "off") || dest_now() == DEST_OFF) return 0;
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
	pthread_mutex_lock(&g.mu);
	cJSON *o = cJSON_CreateObject();
	char dest[16]; pf_set_str("backup.destination", dest, sizeof dest, "off");
	cJSON_AddStringToObject(o, "destination", dest);
	cJSON_AddBoolToObject(o, "busy", g.busy);
	cJSON_AddStringToObject(o, "message", g.message);
	cJSON_AddBoolToObject(o, "error", g.msg_err);
	cJSON *last = cJSON_AddObjectToObject(o, "last");
	cJSON_AddNumberToObject(last, "ts", g.last_ts);
	cJSON_AddStringToObject(last, "name", g.last_name);
	cJSON_AddNumberToObject(last, "size", (double)g.last_size);
	cJSON_AddBoolToObject(last, "ok", g.last_ok);
	cJSON_AddStringToObject(last, "message", g.last_msg);
	cJSON_AddStringToObject(last, "where", g.last_where);
	cJSON *gd = cJSON_AddObjectToObject(o, "gdrive");
	cJSON_AddBoolToObject(gd, "connected", g.gdrive_connected);
	if (g.dev.pending) {
		cJSON *p = cJSON_AddObjectToObject(gd, "pending");
		cJSON_AddStringToObject(p, "user_code", g.dev.user_code);
		cJSON_AddStringToObject(p, "url", g.dev.url);
		cJSON_AddNumberToObject(p, "expires", g.dev.expires);
	}
	double last_good_ts = g.last_good_ts;
	pthread_mutex_unlock(&g.mu);
	cJSON_AddNumberToObject(o, "next_ts", next_due(last_good_ts));
	cJSON_AddBoolToObject(o, "smbclient", g.have_smb);
	return o;
}

/* ---- lifecycle ----------------------------------------------------------------------------- */

void pf_backup_init(const char *data_dir, const char *config_path, bool sim)
{
	pf_strlcpy(g.data_dir, data_dir, sizeof g.data_dir);
	pf_strlcpy(g.config, config_path, sizeof g.config);
	snprintf(g.work, sizeof g.work, "%s/backup", data_dir);
	g.sim = sim;
	pf_mkdir_p(g.work);
	load_last();
	char buf[64];
	g.gdrive_connected = pf_db_kv_get("backup", "gdrive", buf, sizeof buf) == 0;
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
	LOGI(TAG, "backups: %s, %s", dest_name(dest_now()), g.last_ts > 0 ? g.last_name : "none yet");
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
	/* Counted from the last backup that got there, so one that failed is tried again -- an hour
	 * later, not every minute, and announced once per streak rather than every hour. A slot missed
	 * while the grill was switched off runs when it comes back. */
	double due = next_due(good);
	if (due <= 0 || pf_wall() < due) return;
	if (attempt > good && pf_wall() - attempt < 3600) return;
	char err[200];
	LOGI(TAG, "scheduled backup is due");
	if (pf_backup_run(err, sizeof err)) LOGW(TAG, "scheduled backup did not start: %s", err);
}
