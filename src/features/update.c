#define _GNU_SOURCE
#include "features/update.h"
#include "features/tuner.h"
#include "core/db.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/sha256.h"
#include "core/status.h"
#include "core/util.h"
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
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

typedef enum { ST_IDLE, ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING, ST_INSTALLING, ST_UPGRADING, ST_ERROR } state_t;
static const char *const names[] = { "idle", "checking", "downloading", "verifying", "installing", "upgrading", "error" };

#define SYS_HELPER "/usr/local/bin/pifire-system-update"
#define MAX_PKGS 400

static struct {
	pthread_mutex_t mu;
	char data_dir[256];
	bool sim, busy;
	state_t state;
	char message[200];
	double checked_at, next_check;
	char latest[32], notes[4096], asset_name[96], asset_url[512], sums_url[512], html_url[256];
	bool available;
	double progress;   /* 0..1 while downloading */
	char last_notified[32];
	cJSON *installed;   /* the release this daemon was installed as, with its notes, for the app to announce */
	char branch[64];    /* the branch the last PiFire check was for */
	bool switching;     /* what is on offer is another branch's build rather than a newer one */
	cJSON *branches;    /* branches with a build to install, from the last check */
	cJSON *releases;    /* what can be installed from the chosen branch, newest first:
	                     * [{tag, version, prerelease, asset, url, sums, notes, html_url}] */
	/* the system */
	cJSON *packages;    /* [{name, from, to}] from the last system check */
	double sys_checked_at, next_sys_check;
	char sys_message[160];
	bool reboot_required;
} g = { .mu = PTHREAD_MUTEX_INITIALIZER };

/* ---------------- console ----------------
 * One running story of what the updater is doing, in the words of the tools doing it. A job
 * starts a new one. It is also written to disk, because installing PiFire restarts the daemon
 * that is telling it, and the page following along should read the end of the story from the new
 * daemon rather than lose it. */
#define CON_MAX 800
#define CON_W 200
static pthread_mutex_t con_mu = PTHREAD_MUTEX_INITIALIZER;
static char con_text[CON_MAX][CON_W];
static unsigned con_seq, con_id;
static char con_path[340];
/* an automatic check runs quietly: it must not wipe the story of the install before it */
static __thread bool con_quiet;

static void con_put(const char *text)
{
	if (con_quiet) return;
	pthread_mutex_lock(&con_mu);
	pf_strlcpy(con_text[con_seq % CON_MAX], text, CON_W);
	con_seq++;
	FILE *f = con_path[0] ? fopen(con_path, "a") : NULL;
	if (f) { fprintf(f, "%s\n", text); fclose(f); }
	pthread_mutex_unlock(&con_mu);
}

static void con(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void con(const char *fmt, ...)
{
	char buf[CON_W];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	con_put(buf);
}

static void con_line_cb(const char *text, void *ud) { (void)ud; con_put(text); }

/* apt's status lines ("pmstatus:pkg:42.5:Unpacking pkg") are for machines: they become the
 * progress bar, and the console keeps the words people read */
static void apt_line_cb(const char *text, void *ud)
{
	(void)ud;
	if (!strncmp(text, "pmstatus:", 9) || !strncmp(text, "dlstatus:", 9)) {
		const char *p = strchr(text + 9, ':');
		double pct = p ? strtod(p + 1, NULL) : -1;
		if (pct >= 0 && pct <= 100) { pthread_mutex_lock(&g.mu); g.progress = pct / 100.0; pthread_mutex_unlock(&g.mu); }
		return;
	}
	con_put(text);
}

/* a new story: the page following along sees the id change and starts again */
static void con_begin(void)
{
	if (con_quiet) return;
	pthread_mutex_lock(&con_mu);
	con_seq = 0;
	con_id++;
	FILE *f = con_path[0] ? fopen(con_path, "w") : NULL;
	if (f) fclose(f);
	pthread_mutex_unlock(&con_mu);
}

cJSON *pf_update_log_json(unsigned since)
{
	cJSON *o = cJSON_CreateObject();
	pthread_mutex_lock(&con_mu);
	unsigned first = con_seq > CON_MAX ? con_seq - CON_MAX : 0;
	if (since < first) since = first;
	if (since > con_seq) since = first;   /* a reader from an older story */
	cJSON_AddNumberToObject(o, "id", con_id);
	cJSON_AddNumberToObject(o, "seq", con_seq);
	cJSON_AddNumberToObject(o, "from", since);
	cJSON *lines = cJSON_AddArrayToObject(o, "lines");
	for (unsigned i = since; i < con_seq; i++) cJSON_AddItemToArray(lines, cJSON_CreateString(con_text[i % CON_MAX]));
	pthread_mutex_unlock(&con_mu);
	char st[16]; double pr;
	pf_update_stage(st, sizeof st, &pr);
	cJSON_AddStringToObject(o, "state", st);
	cJSON_AddNumberToObject(o, "progress", pr);
	pthread_mutex_lock(&g.mu);
	cJSON_AddBoolToObject(o, "busy", g.busy);
	cJSON_AddStringToObject(o, "message", g.message);
	cJSON_AddBoolToObject(o, "reboot_required", g.reboot_required);
	pthread_mutex_unlock(&g.mu);
	cJSON_AddStringToObject(o, "version", PF_VERSION);
	return o;
}

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

/* numeric dotted core, then an optional pre-release suffix ("-alpha.2" < "-beta.1" < "-rc.1" < release) */
static int cmp_prerelease(const char *a, const char *b)
{
	if (!*a && !*b) return 0;
	if (!*a) return 1;   /* release > pre-release */
	if (!*b) return -1;
	while (*a || *b) {
		char ta[32] = "", tb[32] = "";
		int i = 0;
		while (*a && *a != '.' && i < 31) ta[i++] = *a++;
		ta[i] = 0; i = 0;
		while (*b && *b != '.' && i < 31) tb[i++] = *b++;
		tb[i] = 0;
		char *ea, *eb;
		long na = strtol(ta, &ea, 10), nb = strtol(tb, &eb, 10);
		bool numa = ta[0] && !*ea, numb = tb[0] && !*eb;
		int c;
		if (numa && numb) c = na < nb ? -1 : na > nb ? 1 : 0;
		else if (numa != numb) c = numa ? -1 : 1;      /* numeric identifiers sort before alphabetic */
		else c = strcmp(ta, tb);
		if (c) return c < 0 ? -1 : 1;
		if (*a == '.') a++;
		if (*b == '.') b++;
	}
	return 0;
}

int pf_version_compare(const char *a, const char *b)
{
	if (*a == 'v' || *a == 'V') a++;
	if (*b == 'v' || *b == 'V') b++;
	for (int i = 0; i < 4; i++) {
		long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
		if (x != y) return x < y ? -1 : 1;
		if (*a == '.' && *b == '.') { a++; b++; continue; }
		if (*a == '.') { a++; continue; }
		if (*b == '.') { b++; continue; }
		break;
	}
	return cmp_prerelease(*a == '-' ? a + 1 : a, *b == '-' ? b + 1 : b);
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
	(void)ult; (void)uln;
	int *tenth = ud;
	if (dltotal > 0) {
		pthread_mutex_lock(&g.mu); g.progress = (double)dlnow / (double)dltotal; pthread_mutex_unlock(&g.mu);
		/* a line every tenth of the way, so the console shows it moving without a line per packet */
		int t = (int)(10 * dlnow / dltotal);
		if (tenth && t > *tenth) { *tenth = t; con("  %3d%%  %.1f of %.1f MB", t * 10, dlnow / 1048576.0, dltotal / 1048576.0); }
	}
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
	int tenth = 0;
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, &tenth);
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

/* ---------------- small pure helpers (tested) ---------------- */

void pf_update_branch_slug(const char *branch, char *out, size_t n)
{
	size_t o = 0;
	for (const char *p = branch; *p && o + 1 < n; p++) {
		char c = *p;
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		out[o++] = ok ? c : '-';
	}
	if (n) out[o] = 0;
}

/* "libc6/stable-security 2.36-9+deb12u9 arm64 [upgradable from: 2.36-9+deb12u8]" */
int pf_update_parse_apt_line(const char *line, char *name, size_t nn, char *to, size_t tn, char *from, size_t fn)
{
	const char *slash = strchr(line, '/');
	const char *sp = strchr(line, ' ');
	if (!slash || !sp || slash > sp || slash == line) return -1;
	size_t l = (size_t)(slash - line);
	if (l >= nn) return -1;
	memcpy(name, line, l);
	name[l] = 0;
	const char *v = sp + 1;
	const char *ve = strchr(v, ' ');
	if (!ve || ve == v) return -1;
	l = (size_t)(ve - v);
	if (l >= tn) l = tn - 1;
	memcpy(to, v, l);
	to[l] = 0;
	from[0] = 0;
	const char *f = strstr(line, "from: ");
	if (f) {
		f += 6;
		const char *fe = strchr(f, ']');
		l = fe ? (size_t)(fe - f) : strlen(f);
		if (l >= fn) l = fn - 1;
		memcpy(from, f, l);
		from[l] = 0;
	}
	return 0;
}

bool pf_update_in_window(int wday, int minute, int start_min, unsigned days_mask)
{
	if (!days_mask) days_mask = 0x7f;
	int since = minute - start_min;
	/* a window that starts late in the evening runs past midnight and belongs to the day it began */
	if (since < 0) { since += 1440; wday = (wday + 6) % 7; }
	return since >= 0 && since < 60 && (days_mask & (1u << wday));
}

static void branch_setting(char *out, size_t n)
{
	pf_set_str("update.branch", out, n, "main");
	if (!out[0]) pf_strlcpy(out, "main", n);
}
static bool is_main(const char *b) { return !strcmp(b, "main"); }

/* ---------------- check ---------------- */

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	set_state(ST_ERROR, "%s", buf);
	con("Error: %s", buf);
}

#if PF_WITH_CURL
/* which asset of a release is ours, and what version it carries: pifire-<version>-<arch>.tar.gz */
static bool pick_asset(cJSON *rel, char *ver, size_t vn)
{
	char suffix[40];
	/* the simulator runs on a workstation nobody publishes builds for: it walks through the Pi's */
	snprintf(suffix, sizeof suffix, "-%s.tar.gz", g.sim ? "arm64" : pf_update_arch());
	g.asset_name[0] = g.asset_url[0] = g.sums_url[0] = 0;
	bool found = false;
	cJSON *a;
	cJSON_ArrayForEach(a, cJSON_GetObjectItem(rel, "assets")) {
		const char *name = pf_json_str(a, "name", "");
		size_t l = strlen(name), sl = strlen(suffix);
		if (!strncmp(name, "pifire-", 7) && l > 7 + sl && !strcmp(name + l - sl, suffix)) {
			pf_strlcpy(g.asset_name, name, sizeof g.asset_name);
			pf_strlcpy(g.asset_url, pf_json_str(a, "browser_download_url", ""), sizeof g.asset_url);
			size_t vl = l - 7 - sl;
			if (vl >= vn) vl = vn - 1;
			memcpy(ver, name + 7, vl);
			ver[vl] = 0;
			found = true;
		}
		if (!strcmp(name, "SHA256SUMS")) pf_strlcpy(g.sums_url, pf_json_str(a, "browser_download_url", ""), sizeof g.sums_url);
	}
	return found;
}

/* one installable release, as the page lists it and the installer finds it again */
static cJSON *release_entry(cJSON *rel, const char *version)
{
	char suffix[40];
	snprintf(suffix, sizeof suffix, "-%s.tar.gz", g.sim ? "arm64" : pf_update_arch());
	cJSON *o = cJSON_CreateObject();
	const char *tag = pf_json_str(rel, "tag_name", "");
	cJSON_AddStringToObject(o, "tag", tag);
	cJSON_AddStringToObject(o, "version", version && *version ? version : (tag[0] == 'v' ? tag + 1 : tag));
	cJSON_AddBoolToObject(o, "prerelease", pf_json_bool(rel, "prerelease", false));
	char notes[1500];
	pf_strlcpy(notes, pf_json_str(rel, "body", ""), sizeof notes);
	cJSON_AddStringToObject(o, "notes", notes);
	cJSON_AddStringToObject(o, "html_url", pf_json_str(rel, "html_url", ""));
	cJSON *a;
	cJSON_ArrayForEach(a, cJSON_GetObjectItem(rel, "assets")) {
		const char *name = pf_json_str(a, "name", "");
		size_t l = strlen(name), sl = strlen(suffix);
		if (!strncmp(name, "pifire-", 7) && l > 7 + sl && !strcmp(name + l - sl, suffix)) {
			cJSON_AddStringToObject(o, "asset", name);
			cJSON_AddStringToObject(o, "url", pf_json_str(a, "browser_download_url", ""));
		}
		if (!strcmp(name, "SHA256SUMS")) cJSON_AddStringToObject(o, "sums", pf_json_str(a, "browser_download_url", ""));
	}
	return o;
}

static int cmp_release(const void *x, const void *y)
{
	const cJSON *a = *(const cJSON *const *)x, *b = *(const cJSON *const *)y;
	return -pf_version_compare(pf_json_str((cJSON *)a, "tag_name", ""), pf_json_str((cJSON *)b, "tag_name", ""));
}

static void check_pifire(void)
{
	char repo[96], url[256], err[160], branch[64], slug[64];
	pf_set_str("update.repo", repo, sizeof repo, "");
	if (!repo[0] || strchr(repo, '/') == NULL) { fail("settings.update.repo is not set (owner/name)"); return; }
	branch_setting(branch, sizeof branch);
	pf_update_branch_slug(branch, slug, sizeof slug);
	con("Checking %s for PiFire (%s)", repo, branch);
	/* one listing answers both questions: the newest tagged release, and which branches have a build */
	snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases?per_page=50", repo);
	char *body = http_get(url, err, sizeof err);
	if (!body) { fail("%s", err); return; }
	cJSON *j = cJSON_Parse(body);
	free(body);
	if (!cJSON_IsArray(j)) { cJSON_Delete(j); fail("bad response from GitHub"); return; }
	bool pre = pf_set_bool("update.include_prerelease", true);
	cJSON *best = NULL, *it, *branches = cJSON_CreateArray();
	cJSON *mains[64];
	int nmain = 0;
	cJSON_AddItemToArray(branches, cJSON_CreateString("main"));
	char want[80];
	snprintf(want, sizeof want, "branch-%s", slug);
	cJSON_ArrayForEach(it, j) {
		if (pf_json_bool(it, "draft", false)) continue;
		const char *tag = pf_json_str(it, "tag_name", "");
		if (!strncmp(tag, "branch-", 7)) {
			/* a rolling build: its release is named after the branch it was built from */
			const char *bn = pf_json_str(it, "name", tag + 7);
			if (strcmp(bn, "main")) cJSON_AddItemToArray(branches, cJSON_CreateString(bn));
			if (!is_main(branch) && !strcmp(tag, want)) best = it;
			continue;
		}
		if (!is_main(branch)) continue;
		if (!pre && pf_json_bool(it, "prerelease", false)) continue;
		if (nmain < 64) mains[nmain++] = it;
		if (!best || pf_version_compare(tag, pf_json_str(best, "tag_name", "")) > 0) best = it;
	}
	/* the list to choose from: this branch's build, or the releases newest first */
	cJSON *list = cJSON_CreateArray();
	if (is_main(branch)) {
		qsort(mains, (size_t)nmain, sizeof mains[0], cmp_release);
		for (int i = 0; i < nmain && i < 25; i++) cJSON_AddItemToArray(list, release_entry(mains[i], NULL));
	}
	pthread_mutex_lock(&g.mu);
	cJSON_Delete(g.branches);
	g.branches = branches;
	cJSON_Delete(g.releases);
	g.releases = list;
	pf_strlcpy(g.branch, branch, sizeof g.branch);
	pthread_mutex_unlock(&g.mu);
	if (!best) {
		cJSON_Delete(j);
		pthread_mutex_lock(&g.mu); g.available = false; g.latest[0] = 0; g.checked_at = pf_wall(); pthread_mutex_unlock(&g.mu);
		if (is_main(branch)) fail("no releases published yet");
		else fail("no build of %s has been published", branch);
		return;
	}
	char ver[48] = "";
	pthread_mutex_lock(&g.mu);
	bool have_asset = pick_asset(best, ver, sizeof ver);
	const char *tag = pf_json_str(best, "tag_name", "");
	if (is_main(branch)) {
		pf_strlcpy(g.latest, tag, sizeof g.latest);
		/* a newer release, or the way back from a branch build to the releases */
		g.switching = PF_BRANCH[0] != 0;
		g.available = pf_version_compare(tag, PF_VERSION) > 0 || g.switching;
	} else {
		/* a branch build is what that branch is now; there is no newer or older between branches */
		pf_strlcpy(g.latest, ver[0] ? ver : tag, sizeof g.latest);
		cJSON_AddItemToArray(g.releases, release_entry(best, ver));
		g.switching = strcmp(PF_BRANCH, branch) != 0;
		g.available = ver[0] && strcmp(ver, PF_VERSION) != 0;
	}
	pf_strlcpy(g.notes, pf_json_str(best, "body", ""), sizeof g.notes);
	pf_strlcpy(g.html_url, pf_json_str(best, "html_url", ""), sizeof g.html_url);
	g.checked_at = pf_wall();
	bool avail = g.available;
	char latest[32];
	pf_strlcpy(latest, g.latest, sizeof latest);
	bool notify = avail && strcmp(g.last_notified, latest) != 0;
	if (notify) pf_strlcpy(g.last_notified, latest, sizeof g.last_notified);
	pthread_mutex_unlock(&g.mu);
	cJSON_Delete(j);
	if (avail && !have_asset) { set_state(ST_IDLE, "%s is available but has no %s build", latest, pf_update_arch()); con("%s", g.message); return; }
	set_state(ST_IDLE, avail ? "%s is available" : "up to date (%s)", avail ? latest : PF_VERSION);
	con("PiFire: %s", g.message);
	if (notify) { LOGI(TAG, "update available: %s (running %s)", latest, PF_VERSION); if (pf_db_handle()) pf_db_event(PF_LVL_INFO, "UPDATE_AVAILABLE", g.message); }
}
#endif

/* The simulator has no apt and no root; it shows what the page does with a made-up list. */
static const char *const SIM_PKGS[][3] = {
	{ "libc6", "2.36-9+deb12u8", "2.36-9+deb12u9" }, { "openssl", "3.0.15-1~deb12u1", "3.0.16-1~deb12u1" },
	{ "raspi-firmware", "1:1.20240924-1", "1:1.20241126-1" }, { "tzdata", "2024a-0+deb12u1", "2025a-0+deb12u1" },
};

static void check_system(void)
{
	con("Checking for system updates");
	cJSON *pkgs = cJSON_CreateArray();
	int rc = 0;
	if (g.sim) {
		for (size_t i = 0; i < sizeof SIM_PKGS / sizeof SIM_PKGS[0]; i++) {
			cJSON *p = cJSON_CreateObject();
			cJSON_AddStringToObject(p, "name", SIM_PKGS[i][0]);
			cJSON_AddStringToObject(p, "from", SIM_PKGS[i][1]);
			cJSON_AddStringToObject(p, "to", SIM_PKGS[i][2]);
			cJSON_AddItemToArray(pkgs, p);
		}
		con("(simulator: a sample list)");
	} else {
		const char *refresh[] = { "sudo", "-n", SYS_HELPER, "refresh", NULL };
		rc = pf_run_stream(refresh, 600, con_line_cb, NULL);
		if (rc != 0) { cJSON_Delete(pkgs); fail("apt-get update failed (%d)", rc); return; }
		static char out[64 * 1024];
		const char *list[] = { "sudo", "-n", SYS_HELPER, "list", NULL };
		rc = pf_run_capture(list, out, sizeof out, 120);
		if (rc != 0) { cJSON_Delete(pkgs); fail("listing upgradable packages failed (%d)", rc); return; }
		for (char *save = NULL, *ln = strtok_r(out, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
			char name[96], to[64], from[64];
			if (pf_update_parse_apt_line(ln, name, sizeof name, to, sizeof to, from, sizeof from)) continue;
			if (cJSON_GetArraySize(pkgs) >= MAX_PKGS) break;
			cJSON *p = cJSON_CreateObject();
			cJSON_AddStringToObject(p, "name", name);
			cJSON_AddStringToObject(p, "from", from);
			cJSON_AddStringToObject(p, "to", to);
			cJSON_AddItemToArray(pkgs, p);
		}
	}
	int n = cJSON_GetArraySize(pkgs);
	bool reboot = !g.sim && access("/var/run/reboot-required", F_OK) == 0;
	pthread_mutex_lock(&g.mu);
	cJSON_Delete(g.packages);
	g.packages = pkgs;
	g.sys_checked_at = pf_wall();
	g.reboot_required = reboot;
	snprintf(g.sys_message, sizeof g.sys_message, n ? "%d update%s" : "up to date", n, n == 1 ? "" : "s");
	pthread_mutex_unlock(&g.mu);
	con("System: %s", g.sys_message);
}

static void *check_thread(void *arg)
{
	int what = (int)(intptr_t)arg;
	pthread_setname_np(pthread_self(), "pf-update");
	con_quiet = (what & 4) != 0;
	con_begin();
	if (what & 1) {
#if PF_WITH_CURL
		check_pifire();
#else
		fail("built without libcurl");
#endif
	}
	if (what & 2) {
		check_system();
		/* a system check alone says what it found; with PiFire's, PiFire's answer keeps the line */
		pthread_mutex_lock(&g.mu);
		if (!(what & 1) && g.state == ST_CHECKING) pf_strlcpy(g.message, g.sys_message, sizeof g.message);
		pthread_mutex_unlock(&g.mu);
	}
	con("Done.");
	pthread_mutex_lock(&g.mu); g.busy = false; if (g.state == ST_CHECKING) g.state = ST_IDLE; pthread_mutex_unlock(&g.mu);
	return NULL;
}

static int start_worker(void *(*fn)(void *), void *arg)
{
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&t, &at, fn, arg);
	pthread_attr_destroy(&at);
	return rc;
}

static int check_start(bool pifire, bool system, bool quiet)
{
	if (!pifire && !system) return -1;
	pthread_mutex_lock(&g.mu);
	if (g.busy) { pthread_mutex_unlock(&g.mu); return -1; }
	g.busy = true; g.state = ST_CHECKING; snprintf(g.message, sizeof g.message, "checking");
	pthread_mutex_unlock(&g.mu);
	intptr_t what = (pifire ? 1 : 0) | (system ? 2 : 0) | (quiet ? 4 : 0);
	if (start_worker(check_thread, (void *)what)) { pthread_mutex_lock(&g.mu); g.busy = false; g.state = ST_ERROR; pthread_mutex_unlock(&g.mu); return -1; }
	return 0;
}

int pf_update_check(bool pifire, bool system) { return check_start(pifire, system, false); }

/* ---------------- install ---------------- */

typedef struct { bool pifire; int npk; char (*pk)[96]; } job_t;

/* Make `tag` the release the installer fetches: its archive, its checksums and its notes. Caller
 * holds the lock. 0 when found and installable. */
static int select_release(const char *tag, char *err, size_t n)
{
	cJSON *r;
	cJSON_ArrayForEach(r, g.releases) {
		if (strcmp(pf_json_str(r, "tag", ""), tag) && strcmp(pf_json_str(r, "version", ""), tag)) continue;
		if (!pf_json_str(r, "url", "")[0] || (!pf_json_str(r, "sums", "")[0] && !g.sim)) { snprintf(err, n, "%s has no %s build", tag, pf_update_arch()); return -1; }
		const char *ver = pf_json_str(r, "version", "");
		if (!strcmp(ver, PF_VERSION) && !g.switching) { snprintf(err, n, "%s is already installed", ver); return -1; }
		pf_strlcpy(g.asset_name, pf_json_str(r, "asset", ""), sizeof g.asset_name);
		pf_strlcpy(g.asset_url, pf_json_str(r, "url", ""), sizeof g.asset_url);
		pf_strlcpy(g.sums_url, pf_json_str(r, "sums", ""), sizeof g.sums_url);
		pf_strlcpy(g.notes, pf_json_str(r, "notes", ""), sizeof g.notes);
		pf_strlcpy(g.latest, pf_json_str(r, "tag", ""), sizeof g.latest);
		return 0;
	}
	snprintf(err, n, "%s is not in the list - check for updates first", tag);
	return -1;
}

static bool upgrade_system(job_t *jb)
{
	set_state(ST_UPGRADING, "upgrading %d package%s", jb->npk, jb->npk == 1 ? "" : "s");
	con("== System: %d package%s", jb->npk, jb->npk == 1 ? "" : "s");
	int rc;
	if (g.sim) {
		for (int i = 0; i < jb->npk; i++) {
			con("Preparing to unpack %s ...", jb->pk[i]);
			pf_sleep_ms(400);
			con("Unpacking %s ...", jb->pk[i]);
			pf_sleep_ms(400);
			con("Setting up %s ...", jb->pk[i]);
		}
		con("(simulator: nothing was installed)");
		rc = 0;
	} else {
		const char **argv = calloc((size_t)jb->npk + 5, sizeof *argv);
		if (!argv) { fail("out of memory"); return false; }
		int a = 0;
		argv[a++] = "sudo"; argv[a++] = "-n"; argv[a++] = SYS_HELPER; argv[a++] = "upgrade";
		for (int i = 0; i < jb->npk; i++) argv[a++] = jb->pk[i];
		argv[a] = NULL;
		pthread_mutex_lock(&g.mu); g.progress = 0; pthread_mutex_unlock(&g.mu);
		rc = pf_run_stream(argv, 3600, apt_line_cb, NULL);
		free(argv);
	}
	if (rc != 0) { fail("system upgrade failed (%d)", rc); return false; }
	/* what is left: anything not ticked, and anything the upgrade could not take */
	pthread_mutex_lock(&g.mu);
	cJSON *keep = cJSON_CreateArray(), *p;
	cJSON_ArrayForEach(p, g.packages) {
		bool done = false;
		for (int i = 0; i < jb->npk && !done; i++) if (!strcmp(pf_json_str(p, "name", ""), jb->pk[i])) done = true;
		if (!done) cJSON_AddItemToArray(keep, cJSON_Duplicate(p, true));
	}
	cJSON_Delete(g.packages);
	g.packages = keep;
	int left = cJSON_GetArraySize(keep);
	snprintf(g.sys_message, sizeof g.sys_message, left ? "%d update%s" : "up to date", left, left == 1 ? "" : "s");
	g.reboot_required = !g.sim && access("/var/run/reboot-required", F_OK) == 0;
	bool reboot = g.reboot_required;
	pthread_mutex_unlock(&g.mu);
	con("System packages upgraded.%s", reboot ? " A reboot is needed to finish." : "");
	if (pf_db_handle()) pf_db_event(PF_LVL_INFO, "UPDATE_SYSTEM", "system packages upgraded");
	set_state(ST_IDLE, "system packages upgraded");
	return true;
}

static void install_pifire(void)
{
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

	con("== PiFire %s (running %s)", latest, PF_VERSION);
	set_state(ST_DOWNLOADING, "downloading %s", asset_name);
	con("Downloading %s", asset_name);
	if (g.sim) {
		for (int t = 1; t <= 10; t++) { pthread_mutex_lock(&g.mu); g.progress = t / 10.0; pthread_mutex_unlock(&g.mu); con("  %3d%%", t * 10); pf_sleep_ms(250); }
		con("(simulator: not installed)");
		set_state(ST_IDLE, "simulator: not installed");
		return;
	}
	if (http_download(asset_url, tarball, err, sizeof err)) { fail("%s", err); return; }
	if (http_download(sums_url, sums, err, sizeof err)) { fail("%s", err); return; }

	set_state(ST_VERIFYING, "verifying checksum");
	con("Verifying SHA-256");
	{
		char hex[65];
		if (pf_sha256_file(tarball, hex)) { fail("cannot hash %s", tarball); return; }
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
		if (!found) { fail("SHA256SUMS has no entry for %s", asset_name); return; }
		if (!ok) { fail("checksum mismatch - refusing to install"); unlink(tarball); return; }
		con("  %s  OK", hex);
	}

	set_state(ST_INSTALLING, "unpacking");
	con("Unpacking");
	{
		const char *rm[] = { "rm", "-rf", stage, NULL };
		char out[256];
		pf_run_capture(rm, out, sizeof out, 30);
		pf_mkdir_p(stage);
		const char *tar[] = { "tar", "-xzf", tarball, "-C", stage, "--strip-components=1", NULL };
		if (pf_run_capture(tar, out, sizeof out, 120) != 0) { fail("unpack failed: %.120s", out); return; }
	}
	set_state(ST_INSTALLING, "installing %s - the service restarts and a running cook resumes", latest);
	LOGW(TAG, "installing %s from %s", latest, stage);
	if (pf_db_handle()) pf_db_event(PF_LVL_WARN, "UPDATE_INSTALL", g.message);
	/* the next daemon announces this: which release, from which, and what changed */
	if (pf_db_handle()) {
		cJSON *inst = cJSON_CreateObject();
		cJSON_AddStringToObject(inst, "tag", latest);
		cJSON_AddStringToObject(inst, "from", PF_VERSION);
		pthread_mutex_lock(&g.mu); cJSON_AddStringToObject(inst, "notes", g.notes); pthread_mutex_unlock(&g.mu);
		cJSON_AddNumberToObject(inst, "ts", pf_wall());
		char *txt = cJSON_PrintUnformatted(inst);
		if (txt) pf_db_kv_put("update", "installed", txt);
		free(txt);
		cJSON_Delete(inst);
	}
	con("Installing; PiFire restarts");
	{
		const char *apply[] = { "sudo", "-n", "/usr/local/bin/pifire-update-apply", stage, NULL };
		int rc = pf_run_stream(apply, 60, con_line_cb, NULL);
		if (rc != 0) { if (pf_db_handle()) pf_db_kv_delete("update", "installed"); fail("apply failed (%d)", rc); return; }
	}
	set_state(ST_INSTALLING, "installed %s - restarting", latest);
#else
	fail("built without libcurl");
#endif
}

static void *install_thread(void *arg)
{
	job_t *jb = arg;
	pthread_setname_np(pthread_self(), "pf-update");
	con_begin();
	/* the system first: PiFire's install restarts the daemon, and nothing after it would run */
	bool ok = jb->npk ? upgrade_system(jb) : true;
	if (ok && jb->pifire) install_pifire();
	else if (ok) con("Done.");
	free(jb->pk);
	free(jb);
	pthread_mutex_lock(&g.mu); g.busy = false; pthread_mutex_unlock(&g.mu);
	return NULL;
}

static bool valid_pkg(const char *s)
{
	if (!*s || strlen(s) > 90 || !((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9'))) return false;
	for (const char *p = s; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.' || *p == ':')) return false;
	return true;
}

int pf_update_install_ex(bool pifire, const cJSON *packages, const char *tag, char *err, size_t n)
{
	int npk = cJSON_IsArray(packages) ? cJSON_GetArraySize(packages) : 0;
	if (!pifire && !npk) { snprintf(err, n, "nothing selected"); return -1; }
	pf_status st;
	pf_status_get(&st);
	bool cooking = st.mode != PF_MODE_STOP && st.mode != PF_MODE_MONITOR && st.mode != PF_MODE_ERROR;
	if (st.mode == PF_MODE_MANUAL || st.mode == PF_MODE_PRIME) { snprintf(err, n, "finish the manual / prime run first"); return -1; }
	/* with update.hot_update the daemon hands the running cook to the new process (resume snapshot) */
	if (pifire && cooking && !pf_set_bool("update.hot_update", false)) { snprintf(err, n, "stop the grill first (or enable Update while cooking)"); return -1; }
	/* an apt upgrade can take the CPU, the SD card and the network for minutes: never under a cook */
	if (npk && cooking) { snprintf(err, n, "stop the grill before upgrading system packages"); return -1; }
	job_t *jb = calloc(1, sizeof *jb);
	if (!jb) { snprintf(err, n, "out of memory"); return -1; }
	jb->pifire = pifire;
	if (npk) {
		jb->pk = calloc((size_t)npk, sizeof *jb->pk);
		if (!jb->pk) { free(jb); snprintf(err, n, "out of memory"); return -1; }
		const cJSON *it;
		cJSON_ArrayForEach(it, packages) {
			const char *name = cJSON_IsString(it) ? it->valuestring : "";
			if (!valid_pkg(name)) { free(jb->pk); free(jb); snprintf(err, n, "not a package name: %.40s", name); return -1; }
			pf_strlcpy(jb->pk[jb->npk++], name, sizeof jb->pk[0]);
		}
	}
	pthread_mutex_lock(&g.mu);
	if (g.busy) { pthread_mutex_unlock(&g.mu); free(jb->pk); free(jb); snprintf(err, n, "an update operation is already running"); return -1; }
	if (pifire && tag && *tag && select_release(tag, err, n)) { pthread_mutex_unlock(&g.mu); free(jb->pk); free(jb); return -1; }
	if (pifire && (!tag || !*tag) && (!g.available || !g.asset_url[0] || (!g.sums_url[0] && !g.sim))) {
		pthread_mutex_unlock(&g.mu); free(jb->pk); free(jb);
		snprintf(err, n, "no installable PiFire build for %s - check for updates first", pf_update_arch());
		return -1;
	}
	g.busy = true;
	g.state = npk ? ST_UPGRADING : ST_DOWNLOADING;
	pthread_mutex_unlock(&g.mu);
	if (start_worker(install_thread, jb)) {
		pthread_mutex_lock(&g.mu); g.busy = false; g.state = ST_IDLE; pthread_mutex_unlock(&g.mu);
		free(jb->pk); free(jb);
		snprintf(err, n, "cannot start worker");
		return -1;
	}
	return 0;
}

int pf_update_install(char *err, size_t n) { return pf_update_install_ex(true, NULL, NULL, err, n); }

/* ---------------- status / lifecycle ---------------- */

cJSON *pf_update_status_json(void)
{
	pthread_mutex_lock(&g.mu);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "current", PF_VERSION);
	cJSON_AddStringToObject(o, "current_branch", PF_BRANCH[0] ? PF_BRANCH : "main");
	cJSON_AddStringToObject(o, "arch", pf_update_arch());
	char repo[96], branch[64];
	pf_set_str("update.repo", repo, sizeof repo, "");
	branch_setting(branch, sizeof branch);
	cJSON_AddStringToObject(o, "repo", repo);
	cJSON_AddStringToObject(o, "branch", branch);
	cJSON_AddItemToObject(o, "branches", g.branches ? cJSON_Duplicate(g.branches, true) : cJSON_CreateArray());
	cJSON_AddItemToObject(o, "releases", g.releases && !strcmp(g.branch, branch) ? cJSON_Duplicate(g.releases, true) : cJSON_CreateArray());
	/* a check for another branch answers nothing about this one */
	bool fresh = !strcmp(g.branch, branch);
	cJSON_AddStringToObject(o, "latest", fresh ? g.latest : "");
	cJSON_AddBoolToObject(o, "available", fresh && g.available);
	cJSON_AddBoolToObject(o, "switching", fresh && g.available && g.switching);
	char ign[40];
	pf_set_str("update.ignored", ign, sizeof ign, "");
	cJSON_AddStringToObject(o, "ignored", ign);
	cJSON_AddBoolToObject(o, "installable", fresh && g.available && g.asset_url[0] && (g.sums_url[0] || g.sim));
	cJSON_AddStringToObject(o, "asset", g.asset_name);
	cJSON_AddStringToObject(o, "notes", fresh ? g.notes : "");
	cJSON_AddStringToObject(o, "html_url", g.html_url);
	cJSON_AddStringToObject(o, "state", names[g.state]);
	cJSON_AddStringToObject(o, "message", g.message);
	cJSON_AddNumberToObject(o, "progress", g.progress);
	cJSON_AddNumberToObject(o, "checked_at", fresh ? g.checked_at : 0);
	cJSON_AddBoolToObject(o, "busy", g.busy);
	cJSON *sys = cJSON_AddObjectToObject(o, "system");
	cJSON_AddItemToObject(sys, "packages", g.packages ? cJSON_Duplicate(g.packages, true) : cJSON_CreateArray());
	cJSON_AddNumberToObject(sys, "checked_at", g.sys_checked_at);
	cJSON_AddStringToObject(sys, "message", g.sys_message);
	cJSON_AddBoolToObject(sys, "reboot_required", g.reboot_required);
	if (g.installed) cJSON_AddItemToObject(o, "installed", cJSON_Duplicate(g.installed, true));
	pthread_mutex_unlock(&g.mu);
	return o;
}

static const char *bare_v(const char *t) { return (t[0] == 'v' || t[0] == 'V') ? t + 1 : t; }

void pf_update_summary(bool *available, char *latest, size_t n, int *system_count)
{
	pthread_mutex_lock(&g.mu);
	char branch[64];
	branch_setting(branch, sizeof branch);
	bool fresh = !strcmp(g.branch, branch);
	/* a release the person chose to ignore is not news, here or in the header; the next one is */
	char ign[40];
	pf_set_str("update.ignored", ign, sizeof ign, "");
	bool ignored = ign[0] && !strcmp(bare_v(ign), bare_v(g.latest));
	if (available) *available = fresh && g.available && g.asset_url[0] && !ignored;
	if (latest && n) pf_strlcpy(latest, fresh ? g.latest : "", n);
	if (system_count) *system_count = g.packages ? cJSON_GetArraySize(g.packages) : 0;
	pthread_mutex_unlock(&g.mu);
}

/* After an install the new daemon holds the end of the story: the console the old one wrote, then
 * what the installer printed once the old one had gone. */
static void con_resume(void)
{
	char apply[320];
	snprintf(apply, sizeof apply, "%s/update/apply.log", g.data_dir);
	FILE *f = fopen(con_path, "r");
	if (!f) return;
	char saved[CON_W];
	char path[sizeof con_path];
	pf_strlcpy(path, con_path, sizeof path);
	con_path[0] = 0;   /* read back without writing it out again */
	while (fgets(saved, sizeof saved, f)) { saved[strcspn(saved, "\r\n")] = 0; con_put(saved); }
	fclose(f);
	FILE *a = fopen(apply, "r");
	while (a && fgets(saved, sizeof saved, a)) { saved[strcspn(saved, "\r\n")] = 0; if (saved[0]) con_put(saved); }
	if (a) fclose(a);
	pf_strlcpy(con_path, path, sizeof con_path);
	con("PiFire %s is running.", PF_VERSION);
}

void pf_update_init(const char *data_dir, bool sim)
{
	pf_strlcpy(g.data_dir, data_dir, sizeof g.data_dir);
	g.sim = sim;
	g.state = ST_IDLE;
	snprintf(g.message, sizeof g.message, "not checked yet");
	g.next_check = pf_now() + 120;  /* first automatic check two minutes after boot */
	g.next_sys_check = pf_now() + 600;
	char dir[300];
	snprintf(dir, sizeof dir, "%s/update", data_dir);
	pf_mkdir_p(dir);
	snprintf(con_path, sizeof con_path, "%s/console.log", dir);
	con_id = (unsigned)time(NULL);   /* a new daemon is a new story, unless it is the end of the last one */
	/* what the last install left behind: kept while it names the version now running */
	char *txt = pf_db_handle() ? pf_db_kv_get_dup("update", "installed") : NULL;
	if (txt) {
		cJSON *inst = cJSON_Parse(txt);
		free(txt);
		const char *tag = pf_json_str(inst, "tag", "");
		if (inst && pf_version_compare(tag, PF_VERSION) == 0) { g.installed = inst; con_resume(); }
		else { cJSON_Delete(inst); pf_db_kv_delete("update", "installed"); }
	}
	LOGI(TAG, "version %s%s%s (%s)", PF_VERSION, PF_BRANCH[0] ? " from branch " : "", PF_BRANCH, pf_update_arch());
}

void pf_update_installed_seen(void)
{
	pthread_mutex_lock(&g.mu);
	cJSON_Delete(g.installed);
	g.installed = NULL;
	pthread_mutex_unlock(&g.mu);
	if (pf_db_handle()) pf_db_kv_delete("update", "installed");
}

void pf_update_stage(char *state, size_t n, double *progress)
{
	pthread_mutex_lock(&g.mu);
	pf_strlcpy(state, names[g.state], n);
	if (progress) *progress = g.progress;
	pthread_mutex_unlock(&g.mu);
}

/* the schedule: "HH:MM" local time and the weekdays it applies on (0 = Sunday) */
static void schedule(int *start_min, unsigned *days)
{
	char t[8];
	pf_set_str("update.auto_install_time", t, sizeof t, "02:00");
	int hh = 2, mm = 0;
	if (sscanf(t, "%d:%d", &hh, &mm) < 1 || hh < 0 || hh > 23 || mm < 0 || mm > 59) { hh = 2; mm = 0; }
	*start_min = hh * 60 + mm;
	*days = 0;
	cJSON *d = pf_set_dup("update.auto_install_days");
	cJSON *it;
	cJSON_ArrayForEach(it, d) if (cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint <= 6) *days |= 1u << it->valueint;
	cJSON_Delete(d);
	if (!cJSON_IsArray(d) && !*days) *days = 0x7f;
}

/* Install on its own, when allowed: the switch is on, there is something to install, nothing else
 * is going on with the updater, it is inside the scheduled window, and the grill is idle -- Stop
 * or Monitor, with no timer, recipe or tuning run going. Never mid-cook, whatever the hot-update
 * switch says: that switch is for a person who has decided to, and this is nobody deciding. One
 * attempt per release, and one system upgrade per window, so a failure is not retried every tick. */
static void auto_install(double now)
{
	static char tried[32];
	static double not_before;
	static int sys_tried_yday = -1;
	if (!pf_set_bool("update.auto_install", false) || now < not_before) return;
	not_before = now + 60;
	time_t wall = time(NULL);
	struct tm lt;
	localtime_r(&wall, &lt);
	int start; unsigned days;
	schedule(&start, &days);
	if (!pf_update_in_window(lt.tm_wday, lt.tm_hour * 60 + lt.tm_min, start, days)) return;
	pf_status st;
	pf_status_get(&st);
	bool idle = st.mode == PF_MODE_STOP || st.mode == PF_MODE_MONITOR;
	if (!idle || st.timer.running || st.recipe.active || pf_tuner_active(NULL, NULL, NULL)) return;
	bool with_system = pf_set_bool("update.auto_install_system", false);
	pthread_mutex_lock(&g.mu);
	bool busy = g.busy;
	bool pifire = g.available && g.asset_url[0] && g.sums_url[0] && !g.sim && g.latest[0] && strcmp(tried, g.latest) != 0;
	char tag[32];
	pf_strlcpy(tag, g.latest, sizeof tag);
	bool sys_stale = pf_wall() - g.sys_checked_at > 6 * 3600;
	cJSON *pk = NULL;
	if (with_system && !sys_stale && sys_tried_yday != lt.tm_yday && g.packages && cJSON_GetArraySize(g.packages) > 0) {
		pk = cJSON_CreateArray();
		cJSON *p;
		cJSON_ArrayForEach(p, g.packages) cJSON_AddItemToArray(pk, cJSON_CreateString(pf_json_str(p, "name", "")));
	}
	pthread_mutex_unlock(&g.mu);
	if (busy) { cJSON_Delete(pk); return; }
	/* the list is from hours ago: look again first, and install on the next pass */
	if (with_system && sys_stale && sys_tried_yday != lt.tm_yday) { cJSON_Delete(pk); check_start(false, true, true); return; }
	if (!pifire && !pk) return;
	if (pifire) pf_strlcpy(tried, tag, sizeof tried);
	if (pk) sys_tried_yday = lt.tm_yday;
	char err[160];
	if (pf_update_install_ex(pifire, pk, NULL, err, sizeof err) == 0) {
		LOGI(TAG, "installing automatically: %s%s%s", pifire ? tag : "", pifire && pk ? " and " : "", pk ? "system packages" : "");
		if (pf_db_handle()) pf_db_event(PF_LVL_INFO, "UPDATE_AUTO", pifire ? tag : "system packages");
	} else LOGW(TAG, "automatic install not started: %s", err);
	cJSON_Delete(pk);
}

void pf_update_tick(double now)
{
	auto_install(now);
	if (!pf_set_bool("update.auto_check", true)) return;
	double hours = pf_set_num("update.check_interval_h", 24);
	if (hours < 1) hours = 1;
	if (now >= g.next_check) {
		if (check_start(true, false, true) == 0) g.next_check = now + hours * 3600;
		else g.next_check = now + 300;
		return;
	}
	/* The system list needs apt-get update, which works the CPU and the SD card for a minute on a
	 * Zero: only while the grill is idle, and put off until it is. */
	if (now >= g.next_sys_check && !g.sim) {
		pf_status st;
		pf_status_get(&st);
		if (st.mode != PF_MODE_STOP && st.mode != PF_MODE_MONITOR) { g.next_sys_check = now + 600; return; }
		if (check_start(false, true, true) == 0) g.next_sys_check = now + hours * 3600;
		else g.next_sys_check = now + 300;
	}
}
