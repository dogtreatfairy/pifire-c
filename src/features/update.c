#define _GNU_SOURCE
#include "features/update.h"
#include "core/db.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/sha256.h"
#include "core/status.h"
#include "core/util.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#if PF_WITH_CURL
#include <curl/curl.h>
#endif

#define TAG "update"

typedef enum { ST_IDLE, ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING, ST_INSTALLING, ST_ERROR } state_t;

static struct {
	pthread_mutex_t mu;
	char data_dir[256];
	bool sim, busy;
	state_t state;
	char message[200];
	double checked_at, next_check;
	char latest[32], notes[2048], asset_name[96], asset_url[512], sums_url[512], html_url[256];
	bool available;
	double progress;   /* 0..1 while downloading */
	char last_notified[32];
} g = { .mu = PTHREAD_MUTEX_INITIALIZER };

const char *pf_update_arch(void)
{
	static char arch[16];
	if (!arch[0]) {
		struct utsname u;
		const char *m = uname(&u) == 0 ? u.machine : "";
		if (!strcmp(m, "aarch64")) strcpy(arch, "arm64");
		else if (!strncmp(m, "armv", 4)) strcpy(arch, "armhf");
		else if (!strcmp(m, "x86_64")) strcpy(arch, "amd64");
		else snprintf(arch, sizeof arch, "%.15s", m[0] ? m : "unknown");
	}
	return arch;
}

int pf_version_compare(const char *a, const char *b)
{
	if (*a == 'v' || *a == 'V') a++;
	if (*b == 'v' || *b == 'V') b++;
	for (int i = 0; i < 4; i++) {
		long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
		if (x != y) return x < y ? -1 : 1;
		if (*a == '.') a++;
		if (*b == '.') b++;
		if (!*a && !*b) return 0;
	}
	/* a pre-release suffix ("-rc1") sorts before the plain release */
	bool pa = *a == '-', pb = *b == '-';
	return pa == pb ? 0 : pa ? -1 : 1;
}

static void set_state(state_t st, const char *fmt, ...)
{
	pthread_mutex_lock(&g.mu);
	g.state = st;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g.message, sizeof g.message, fmt, ap);
	va_end(ap);
	pthread_mutex_unlock(&g.mu);
}

#if PF_WITH_CURL
typedef struct { char *buf; size_t len, cap; } membuf;

static size_t mem_cb(char *p, size_t sz, size_t n, void *ud)
{
	membuf *m = ud;
	size_t add = sz * n;
	if (m->len + add + 1 > m->cap) {
		size_t nc = (m->len + add + 1) * 2;
		if (nc > 4 * 1024 * 1024) return 0;
		char *nb = realloc(m->buf, nc);
		if (!nb) return 0;
		m->buf = nb; m->cap = nc;
	}
	memcpy(m->buf + m->len, p, add);
	m->len += add;
	m->buf[m->len] = 0;
	return add;
}

static CURL *curl_new(const char *url)
{
	CURL *c = curl_easy_init();
	if (!c) return NULL;
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "pifired/" PF_VERSION);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	return c;
}

/* GET into memory; returns malloc'd body or NULL (err filled) */
static char *http_get(const char *url, char *err, size_t n)
{
	CURL *c = curl_new(url);
	if (!c) { snprintf(err, n, "curl init failed"); return NULL; }
	membuf m = { 0 };
	struct curl_slist *h = curl_slist_append(NULL, "Accept: application/vnd.github+json");
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
	CURLcode rc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(h);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK) {
		if (code == 404) snprintf(err, n, "no release found (HTTP 404) - check settings.update.repo");
		else if (code == 403) snprintf(err, n, "GitHub rate limit or forbidden (HTTP 403), try later");
		else snprintf(err, n, "%s (HTTP %ld)", curl_easy_strerror(rc), code);
		free(m.buf);
		return NULL;
	}
	return m.buf;
}

static int progress_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ult, curl_off_t uln)
{
	(void)ud; (void)ult; (void)uln;
	if (dltotal > 0) { pthread_mutex_lock(&g.mu); g.progress = (double)dlnow / (double)dltotal; pthread_mutex_unlock(&g.mu); }
	return 0;
}

static int http_download(const char *url, const char *path, char *err, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f) { snprintf(err, n, "cannot write %s", path); return -1; }
	CURL *c = curl_new(url);
	if (!c) { fclose(f); snprintf(err, n, "curl init failed"); return -1; }
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, NULL);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 900L);
	CURLcode rc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	fclose(f);
	if (rc != CURLE_OK) { snprintf(err, n, "download failed: %s (HTTP %ld)", curl_easy_strerror(rc), code); unlink(path); return -1; }
	return 0;
}
#endif

/* ---------------- check ---------------- */

static void *check_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-update");
#if PF_WITH_CURL
	char repo[96], url[256], err[160];
	pf_set_str("update.repo", repo, sizeof repo, "");
	if (!repo[0] || strchr(repo, '/') == NULL) { set_state(ST_ERROR, "settings.update.repo is not set (owner/name)"); goto done; }
	snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", repo);
	char *body = http_get(url, err, sizeof err);
	if (!body) { set_state(ST_ERROR, "%s", err); goto done; }
	cJSON *j = cJSON_Parse(body);
	free(body);
	if (!j) { set_state(ST_ERROR, "bad response from GitHub"); goto done; }
	char tag[32];
	pf_strlcpy(tag, pf_json_str(j, "tag_name", ""), sizeof tag);
	char want[128];
	snprintf(want, sizeof want, "pifire-%s-%s.tar.gz", tag[0] == 'v' ? tag + 1 : tag, pf_update_arch());
	pthread_mutex_lock(&g.mu);
	pf_strlcpy(g.latest, tag, sizeof g.latest);
	pf_strlcpy(g.notes, pf_json_str(j, "body", ""), sizeof g.notes);
	pf_strlcpy(g.html_url, pf_json_str(j, "html_url", ""), sizeof g.html_url);
	g.asset_name[0] = g.asset_url[0] = g.sums_url[0] = 0;
	cJSON *a;
	cJSON_ArrayForEach(a, cJSON_GetObjectItem(j, "assets")) {
		const char *name = pf_json_str(a, "name", "");
		if (!strcmp(name, want)) { pf_strlcpy(g.asset_name, name, sizeof g.asset_name); pf_strlcpy(g.asset_url, pf_json_str(a, "browser_download_url", ""), sizeof g.asset_url); }
		if (!strcmp(name, "SHA256SUMS")) pf_strlcpy(g.sums_url, pf_json_str(a, "browser_download_url", ""), sizeof g.sums_url);
	}
	g.available = tag[0] && pf_version_compare(tag, PF_VERSION) > 0;
	g.checked_at = pf_wall();
	bool notify = g.available && strcmp(g.last_notified, tag) != 0;
	if (notify) pf_strlcpy(g.last_notified, tag, sizeof g.last_notified);
	pthread_mutex_unlock(&g.mu);
	cJSON_Delete(j);
	if (!tag[0]) set_state(ST_ERROR, "release has no tag");
	else if (g.available && !g.asset_url[0]) set_state(ST_IDLE, "%s is available but has no %s asset", tag, pf_update_arch());
	else set_state(ST_IDLE, g.available ? "%s is available" : "up to date (%s)", g.available ? tag : PF_VERSION);
	if (notify) { LOGI(TAG, "update available: %s (running %s)", tag, PF_VERSION); if (pf_db_handle()) pf_db_event(PF_LVL_INFO, "UPDATE_AVAILABLE", g.message); }
done:
#else
	set_state(ST_ERROR, "built without libcurl");
#endif
	pthread_mutex_lock(&g.mu); g.busy = false; pthread_mutex_unlock(&g.mu);
	return NULL;
}

int pf_update_check(void)
{
	pthread_mutex_lock(&g.mu);
	if (g.busy) { pthread_mutex_unlock(&g.mu); return -1; }
	g.busy = true; g.state = ST_CHECKING; snprintf(g.message, sizeof g.message, "checking");
	pthread_mutex_unlock(&g.mu);
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, check_thread, NULL)) { pthread_mutex_lock(&g.mu); g.busy = false; g.state = ST_ERROR; pthread_mutex_unlock(&g.mu); return -1; }
	pthread_attr_destroy(&at);
	return 0;
}

/* ---------------- install ---------------- */

static void *install_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-update");
#if PF_WITH_CURL
	char dir[300], tarball[420], sums[340], stage[320], err[160];
	snprintf(dir, sizeof dir, "%s/update", g.data_dir);
	pf_mkdir_p(dir);
	pthread_mutex_lock(&g.mu);
	snprintf(tarball, sizeof tarball, "%s/%s", dir, g.asset_name);
	char asset_url[512], sums_url[512], asset_name[96], latest[32];
	pf_strlcpy(asset_url, g.asset_url, sizeof asset_url);
	pf_strlcpy(sums_url, g.sums_url, sizeof sums_url);
	pf_strlcpy(asset_name, g.asset_name, sizeof asset_name);
	pf_strlcpy(latest, g.latest, sizeof latest);
	g.progress = 0;
	pthread_mutex_unlock(&g.mu);
	snprintf(sums, sizeof sums, "%s/SHA256SUMS", dir);
	snprintf(stage, sizeof stage, "%s/stage", dir);

	set_state(ST_DOWNLOADING, "downloading %s", asset_name);
	if (http_download(asset_url, tarball, err, sizeof err)) { set_state(ST_ERROR, "%s", err); goto done; }
	if (http_download(sums_url, sums, err, sizeof err)) { set_state(ST_ERROR, "%s", err); goto done; }

	set_state(ST_VERIFYING, "verifying checksum");
	{
		char hex[65];
		if (pf_sha256_file(tarball, hex)) { set_state(ST_ERROR, "cannot hash %s", tarball); goto done; }
		FILE *f = fopen(sums, "r");
		char line[400];
		bool found = false, ok = false;
		while (f && fgets(line, sizeof line, f)) {
			char h[65], name[300];
			if (sscanf(line, "%64s %299s", h, name) != 2) continue;
			const char *base = name[0] == '*' ? name + 1 : name;
			if (!strcmp(base, asset_name)) { found = true; ok = !strcasecmp(h, hex); }
		}
		if (f) fclose(f);
		if (!found) { set_state(ST_ERROR, "SHA256SUMS has no entry for %s", asset_name); goto done; }
		if (!ok) { set_state(ST_ERROR, "checksum mismatch - refusing to install"); unlink(tarball); goto done; }
	}

	set_state(ST_INSTALLING, "unpacking");
	{
		const char *rm[] = { "rm", "-rf", stage, NULL };
		char out[256];
		pf_run_capture(rm, out, sizeof out, 30);
		pf_mkdir_p(stage);
		const char *tar[] = { "tar", "-xzf", tarball, "-C", stage, "--strip-components=1", NULL };
		if (pf_run_capture(tar, out, sizeof out, 120) != 0) { set_state(ST_ERROR, "unpack failed: %.120s", out); goto done; }
	}
	set_state(ST_INSTALLING, "installing %s - the service will restart", latest);
	LOGW(TAG, "installing %s from %s", latest, stage);
	if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "UPDATE_INSTALL", g.message);
	{
		const char *apply[] = { "sudo", "-n", "/usr/local/bin/pifire-update-apply", stage, NULL };
		char out[512];
		int rc = pf_run_capture(apply, out, sizeof out, 60);
		if (rc != 0) { set_state(ST_ERROR, "apply failed (%d): %.150s", rc, out); goto done; }
	}
	set_state(ST_INSTALLING, "installed %s - restarting", latest);
done:
#else
	set_state(ST_ERROR, "built without libcurl");
#endif
	pthread_mutex_lock(&g.mu); g.busy = false; pthread_mutex_unlock(&g.mu);
	return NULL;
}

int pf_update_install(char *err, size_t n)
{
	pf_status st;
	pf_status_get(&st);
	if (st.mode != PF_MODE_STOP && st.mode != PF_MODE_MONITOR && st.mode != PF_MODE_ERROR) { snprintf(err, n, "stop the grill first"); return -1; }
	if (g.sim) { snprintf(err, n, "not available in simulator"); return -1; }
	pthread_mutex_lock(&g.mu);
	if (g.busy) { pthread_mutex_unlock(&g.mu); snprintf(err, n, "an update operation is already running"); return -1; }
	if (!g.available || !g.asset_url[0] || !g.sums_url[0]) { pthread_mutex_unlock(&g.mu); snprintf(err, n, "no installable release for %s - check for updates first", pf_update_arch()); return -1; }
	g.busy = true;
	pthread_mutex_unlock(&g.mu);
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, install_thread, NULL)) { pthread_mutex_lock(&g.mu); g.busy = false; pthread_mutex_unlock(&g.mu); snprintf(err, n, "cannot start worker"); return -1; }
	pthread_attr_destroy(&at);
	return 0;
}

/* ---------------- status / lifecycle ---------------- */

cJSON *pf_update_status_json(void)
{
	static const char *const names[] = { "idle", "checking", "downloading", "verifying", "installing", "error" };
	pthread_mutex_lock(&g.mu);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "current", PF_VERSION);
	cJSON_AddStringToObject(o, "arch", pf_update_arch());
	char repo[96];
	pf_set_str("update.repo", repo, sizeof repo, "");
	cJSON_AddStringToObject(o, "repo", repo);
	cJSON_AddStringToObject(o, "latest", g.latest);
	cJSON_AddBoolToObject(o, "available", g.available);
	cJSON_AddBoolToObject(o, "installable", g.available && g.asset_url[0] && g.sums_url[0] && !g.sim);
	cJSON_AddStringToObject(o, "asset", g.asset_name);
	cJSON_AddStringToObject(o, "notes", g.notes);
	cJSON_AddStringToObject(o, "html_url", g.html_url);
	cJSON_AddStringToObject(o, "state", names[g.state]);
	cJSON_AddStringToObject(o, "message", g.message);
	cJSON_AddNumberToObject(o, "progress", g.progress);
	cJSON_AddNumberToObject(o, "checked_at", g.checked_at);
	cJSON_AddBoolToObject(o, "busy", g.busy);
	pthread_mutex_unlock(&g.mu);
	return o;
}

void pf_update_init(const char *data_dir, bool sim)
{
	pf_strlcpy(g.data_dir, data_dir, sizeof g.data_dir);
	g.sim = sim;
	g.state = ST_IDLE;
	snprintf(g.message, sizeof g.message, "not checked yet");
	g.next_check = pf_now() + 120;  /* first automatic check two minutes after boot */
	LOGI(TAG, "version %s (%s)", PF_VERSION, pf_update_arch());
}

void pf_update_tick(double now)
{
	if (!pf_set_bool("update.auto_check", true)) return;
	if (now < g.next_check) return;
	double hours = pf_set_num("update.check_interval_h", 24);
	if (hours < 1) hours = 1;
	g.next_check = now + hours * 3600;
	pf_update_check();
}
