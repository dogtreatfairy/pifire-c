#define _GNU_SOURCE
#include "net/cloudflare.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/util.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "cloudflare"
#define HELPER "/usr/local/bin/pifire-cloudflare"
/* cloudflared's metrics server, as the helper's unit file starts it: loopback only */
#define METRICS_PORT 20241

/* ---------------- settings, normalised ---------------- */

/* The team name as Cloudflare spells it in <team>.cloudflareaccess.com. People paste the whole
 * domain or the URL, so those are reduced to the name; anything that is not a DNS label after that
 * is refused (it becomes part of a URL the grill fetches keys from). */
static bool team_name(char *out, size_t n)
{
	char raw[160];
	pf_set_str("network.cloudflare_team", raw, sizeof raw, "");
	char *s = raw;
	while (isspace((unsigned char)*s)) s++;
	if (!strncasecmp(s, "https://", 8)) s += 8;
	else if (!strncasecmp(s, "http://", 7)) s += 7;
	char *e = s + strcspn(s, "./ \t\r\n");
	*e = 0;
	size_t l = strlen(s);
	if (!l || l > 63 || l >= n) { if (n) out[0] = 0; return false; }
	for (size_t i = 0; i < l; i++) {
		char c = (char)tolower((unsigned char)s[i]);
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) { out[0] = 0; return false; }
		out[i] = c;
	}
	out[l] = 0;
	return true;
}

static bool aud_tag(char *out, size_t n)
{
	char raw[160];
	pf_set_str("network.cloudflare_aud", raw, sizeof raw, "");
	size_t w = 0;
	for (const char *p = raw; *p && w + 1 < n; p++) {
		if (isspace((unsigned char)*p)) continue;
		if (!isalnum((unsigned char)*p)) { out[0] = 0; return false; }
		out[w++] = *p;
	}
	out[w] = 0;
	return w > 0;
}

/* ---------------- helper actions ---------------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_busy;
static char g_action[16], g_output[512];
static int g_last_rc = -1;
static bool g_have_result;

typedef struct { char verb[16]; char *token; } job;

static void *runner(void *arg)
{
	job *j = arg;
	pthread_setname_np(pthread_self(), "pf-cloudflare");
	const char *argv[] = { "sudo", "-n", HELPER, j->verb, NULL };
	char out[512];
	int rc = !strcmp(j->verb, "start")
		? pf_run_capture_in(argv, j->token ? j->token : "", out, sizeof out, 60)
		: pf_run_capture(argv, out, sizeof out, !strcmp(j->verb, "install") ? 600 : 60);
	LOGI(TAG, "%s -> rc %d: %.200s", j->verb, rc, out);
	pthread_mutex_lock(&g_mu);
	g_last_rc = rc;
	g_have_result = true;
	pf_strlcpy(g_output, out, sizeof g_output);
	pthread_mutex_unlock(&g_mu);
	if (rc == 0 && !strcmp(j->verb, "start")) pf_set_put_bool("network.cloudflare_enabled", true);
	if (rc == 0 && (!strcmp(j->verb, "stop") || !strcmp(j->verb, "forget"))) pf_set_put_bool("network.cloudflare_enabled", false);
	if (j->token) { explicit_bzero(j->token, strlen(j->token)); free(j->token); }
	free(j);
	atomic_store(&g_busy, false);
	return NULL;
}

int pf_cloudflare_action(const char *verb, const char *token, char *err, size_t n)
{
	static const char *const verbs[] = { "install", "start", "stop", "forget", NULL };
	bool ok = false;
	for (int i = 0; verbs[i]; i++) if (!strcmp(verbs[i], verb)) ok = true;
	if (!ok) { snprintf(err, n, "unknown action"); return -1; }
	pf_status st;
	pf_status_get(&st);
	if (st.sim) { snprintf(err, n, "not available in simulator"); return -1; }
	if (token && strlen(token) > 8192) { snprintf(err, n, "token too long"); return -1; }
	job *j = calloc(1, sizeof *j);
	if (!j) { snprintf(err, n, "out of memory"); return -1; }
	pf_strlcpy(j->verb, verb, sizeof j->verb);
	if (!strcmp(verb, "start") && token && *token && !(j->token = strdup(token))) { free(j); snprintf(err, n, "out of memory"); return -1; }
	if (atomic_exchange(&g_busy, true)) { free(j->token); free(j); snprintf(err, n, "another Cloudflare action is still running"); return -1; }
	pthread_mutex_lock(&g_mu);
	pf_strlcpy(g_action, verb, sizeof g_action);
	g_have_result = false;
	g_output[0] = 0;
	pthread_mutex_unlock(&g_mu);
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, runner, j)) {
		pthread_attr_destroy(&at);
		if (j->token) { explicit_bzero(j->token, strlen(j->token)); free(j->token); }
		free(j);
		atomic_store(&g_busy, false);
		snprintf(err, n, "cannot start worker");
		return -1;
	}
	pthread_attr_destroy(&at);
	return 0;
}

/* ---------------- is the tunnel up ---------------- */

/* cloudflared answers GET /ready on its metrics port with 200 and {"readyConnections":N} while it
 * holds connections to Cloudflare's edge, 503 while it does not. Asked directly over loopback: no
 * sudo, no process, cheap enough for the status stream. Returns the connection count, -1 when
 * nothing answers. */
static int ready_connections(void)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return -1;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(METRICS_PORT) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS) { close(fd); return -1; }
	struct pollfd p = { .fd = fd, .events = POLLOUT };
	if (poll(&p, 1, 1000) <= 0) { close(fd); return -1; }
	int soerr = 0;
	socklen_t sl = sizeof soerr;
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) || soerr) { close(fd); return -1; }
	static const char req[] = "GET /ready HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
	if (send(fd, req, sizeof req - 1, MSG_NOSIGNAL) != (ssize_t)(sizeof req - 1)) { close(fd); return -1; }
	char buf[1024];
	size_t w = 0;
	double deadline = pf_now() + 2;
	while (w + 1 < sizeof buf) {
		int left = (int)((deadline - pf_now()) * 1000);
		if (left <= 0) break;
		p.events = POLLIN;
		if (poll(&p, 1, left) <= 0) break;
		ssize_t r = recv(fd, buf + w, sizeof buf - 1 - w, 0);
		if (r <= 0) break;
		w += (size_t)r;
	}
	close(fd);
	buf[w] = 0;
	int code = 0;
	if (sscanf(buf, "HTTP/%*s %d", &code) != 1) return -1;
	if (code != 200) return 0;
	const char *rc = strstr(buf, "\"readyConnections\"");
	int nconn = 1;
	if (rc && (rc = strchr(rc, ':'))) nconn = atoi(rc + 1);
	return nconn > 0 ? nconn : 0;
}

static pthread_mutex_t g_brief_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_brief_online;
static double g_brief_t;
static atomic_bool g_brief_busy;

static void *brief_refresh(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-cf-brief");
	int c = ready_connections();
	pthread_mutex_lock(&g_brief_mu);
	g_brief_online = c > 0;
	g_brief_t = pf_now();
	pthread_mutex_unlock(&g_brief_mu);
	atomic_store(&g_brief_busy, false);
	return NULL;
}

void pf_cloudflare_brief(bool *configured, bool *online, char *name, size_t n)
{
	bool on = pf_set_bool("network.cloudflare_enabled", false);
	if (configured) *configured = on;
	if (name) pf_set_str("network.cloudflare_hostname", name, n, "");
	pthread_mutex_lock(&g_brief_mu);
	bool stale = pf_now() - g_brief_t > 15;
	if (online) *online = on && g_brief_online;
	pthread_mutex_unlock(&g_brief_mu);
	if (!on || !stale || atomic_exchange(&g_brief_busy, true)) return;
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, brief_refresh, NULL)) atomic_store(&g_brief_busy, false);
	pthread_attr_destroy(&at);
}

/* ---------------- Cloudflare Access tokens ---------------- */

#if !defined(PF_HAVE_WEBPUSH)

/* Built without libcrypto: nothing can be verified, so nothing through the tunnel is let in. */
int pf_cloudflare_check_with_pem(const char *jwt, const char *pem, const char *team, const char *aud, double now, char *why, size_t n)
{
	(void)jwt; (void)pem; (void)team; (void)aud; (void)now;
	snprintf(why, n, "this build cannot verify Cloudflare Access tokens");
	return -1;
}
int pf_cloudflare_check(const char *jwt, const char *host, char *why, size_t n)
{
	(void)jwt; (void)host;
	snprintf(why, n, "this build cannot verify Cloudflare Access tokens");
	return -1;
}
static bool can_verify(void) { return false; }

#else

#include <curl/curl.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

static bool can_verify(void) { return true; }

/* The hostname the tunnel is reached by, learned from the requests that pass: the dashboard holds
 * it, not the grill, and it is what the app needs to know it is being used through Cloudflare. */
static void remember_host(const char *host)
{
	if (!host) return;
	char h[128];
	size_t l = strcspn(host, ":");
	if (!l || l >= sizeof h) return;
	for (size_t i = 0; i < l; i++) {
		char c = (char)tolower((unsigned char)host[i]);
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return;
		h[i] = c;
	}
	h[l] = 0;
	char cur[128];
	pf_set_str("network.cloudflare_hostname", cur, sizeof cur, "");
	if (strcmp(cur, h)) { LOGI(TAG, "reached through Cloudflare as %s", h); pf_set_put_str("network.cloudflare_hostname", h); }
}

static int b64v(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '-' || c == '+') return 62;
	if (c == '_' || c == '/') return 63;
	return -1;
}

/* base64url without padding, as JWTs use it. Returns the byte count, -1 on a bad character or
 * overflow. */
static long b64url_dec(const char *in, size_t len, unsigned char *out, size_t cap)
{
	size_t w = 0;
	unsigned acc = 0;
	int bits = 0;
	for (size_t i = 0; i < len; i++) {
		if (in[i] == '=') break;
		int v = b64v(in[i]);
		if (v < 0) return -1;
		acc = (acc << 6) | (unsigned)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (w >= cap) return -1;
			out[w++] = (unsigned char)(acc >> bits);
		}
	}
	return (long)w;
}

static cJSON *b64url_json(const char *in, size_t len)
{
	unsigned char buf[4096];
	long l = b64url_dec(in, len, buf, sizeof buf - 1);
	if (l < 0) return NULL;
	buf[l] = 0;
	return cJSON_Parse((const char *)buf);
}

typedef struct {
	size_t signed_len;       /* header.payload, the part the signature covers */
	unsigned char sig[1024];
	size_t sig_len;
	char kid[96];
} jwt_parts;

/* Split and check the claims; the signature is checked by the caller once it has the key. */
static int jwt_claims(const char *jwt, const char *team, const char *aud, double now, jwt_parts *jp, char *why, size_t n)
{
	size_t len = strlen(jwt);
	if (len > 8192) { snprintf(why, n, "Access token too long"); return -1; }
	const char *d1 = strchr(jwt, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
	if (!d1 || !d2 || strchr(d2 + 1, '.')) { snprintf(why, n, "malformed Access token"); return -1; }
	cJSON *h = b64url_json(jwt, (size_t)(d1 - jwt));
	cJSON *p = b64url_json(d1 + 1, (size_t)(d2 - d1 - 1));
	long sl = b64url_dec(d2 + 1, len - (size_t)(d2 + 1 - jwt), jp->sig, sizeof jp->sig);
	int rc = -1;
	if (!h || !p || sl <= 0) { snprintf(why, n, "malformed Access token"); goto out; }
	/* RS256 and nothing else: the algorithm is ours to pick, never the token's */
	if (strcmp(pf_json_str(h, "alg", ""), "RS256")) { snprintf(why, n, "Access token is not RS256"); goto out; }
	pf_strlcpy(jp->kid, pf_json_str(h, "kid", ""), sizeof jp->kid);
	char iss[160];
	snprintf(iss, sizeof iss, "https://%s.cloudflareaccess.com", team);
	if (strcmp(pf_json_str(p, "iss", ""), iss)) { snprintf(why, n, "Access token is from another team"); goto out; }
	cJSON *a = cJSON_GetObjectItem(p, "aud"), *e;
	bool aud_ok = cJSON_IsString(a) && !strcmp(a->valuestring, aud);
	if (cJSON_IsArray(a)) cJSON_ArrayForEach(e, a) if (cJSON_IsString(e) && !strcmp(e->valuestring, aud)) aud_ok = true;
	if (!aud_ok) { snprintf(why, n, "Access token is for another application"); goto out; }
	cJSON *exp = cJSON_GetObjectItem(p, "exp"), *nbf = cJSON_GetObjectItem(p, "nbf");
	/* a minute's grace either way for a Pi clock that is a little off */
	if (!cJSON_IsNumber(exp) || exp->valuedouble < now - 60) { snprintf(why, n, "Access session expired"); goto out; }
	if (cJSON_IsNumber(nbf) && nbf->valuedouble > now + 60) { snprintf(why, n, "Access token not yet valid"); goto out; }
	jp->signed_len = (size_t)(d2 - jwt);
	jp->sig_len = (size_t)sl;
	rc = 0;
out:
	cJSON_Delete(h);
	cJSON_Delete(p);
	return rc;
}

static bool sig_ok(EVP_PKEY *k, const char *jwt, const jwt_parts *jp)
{
	if (!k || EVP_PKEY_base_id(k) != EVP_PKEY_RSA) return false;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!ctx) return false;
	bool ok = EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, k) == 1
		&& EVP_DigestVerify(ctx, jp->sig, jp->sig_len, (const unsigned char *)jwt, jp->signed_len) == 1;
	EVP_MD_CTX_free(ctx);
	return ok;
}

static EVP_PKEY *pem_key(const char *pem)
{
	BIO *b = BIO_new_mem_buf(pem, -1);
	if (!b) return NULL;
	EVP_PKEY *k = NULL;
	X509 *x = PEM_read_bio_X509(b, NULL, NULL, NULL);
	if (x) { k = X509_get_pubkey(x); X509_free(x); }
	else { (void)BIO_reset(b); k = PEM_read_bio_PUBKEY(b, NULL, NULL, NULL); }
	BIO_free(b);
	return k;
}

int pf_cloudflare_check_with_pem(const char *jwt, const char *pem, const char *team, const char *aud, double now, char *why, size_t n)
{
	jwt_parts jp;
	if (!jwt || !*jwt) { snprintf(why, n, "no Access token"); return -1; }
	if (jwt_claims(jwt, team, aud, now, &jp, why, n)) return -1;
	EVP_PKEY *k = pem_key(pem);
	bool ok = sig_ok(k, jwt, &jp);
	EVP_PKEY_free(k);
	if (!ok) { snprintf(why, n, "Access token signature does not verify"); return -1; }
	return 0;
}

/* The team's signing keys, from https://<team>.cloudflareaccess.com/cdn-cgi/access/certs. Cloudflare
 * rotates them and publishes the old and the new side by side, so a token signed by a key we have
 * not seen sends us back for the list -- at most every half minute, so a stream of forged kids
 * cannot turn the grill into a fetch loop. */
#define MAX_KEYS 8
static pthread_mutex_t g_keys_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_fetch_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char kid[96]; EVP_PKEY *k; } g_keys[MAX_KEYS];
static int g_nkeys;
static char g_keys_team[64], g_try_team[64];
static double g_keys_t, g_keys_try;

typedef struct { char *p; size_t n; } mem;
static size_t collect(char *p, size_t s, size_t n, void *ud)
{
	mem *m = ud;
	size_t add = s * n;
	if (m->n + add > 256 * 1024) return 0;
	char *np = realloc(m->p, m->n + add + 1);
	if (!np) return 0;
	memcpy(np + m->n, p, add);
	m->p = np;
	m->n += add;
	m->p[m->n] = 0;
	return add;
}

static void fetch_keys(const char *team)
{
	char url[160];
	snprintf(url, sizeof url, "https://%s.cloudflareaccess.com/cdn-cgi/access/certs", team);
	mem m = { 0 };
	CURL *c = curl_easy_init();
	if (!c) return;
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "PiFire/" PF_VERSION);
	CURLcode rc = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	cJSON *j = rc == CURLE_OK && code == 200 && m.p ? cJSON_Parse(m.p) : NULL;
	free(m.p);
	if (!j) { LOGW(TAG, "could not fetch Access keys for team %s (curl %d, HTTP %ld)", team, (int)rc, code); return; }
	struct { char kid[96]; EVP_PKEY *k; } fresh[MAX_KEYS];
	int nf = 0;
	cJSON *e;
	cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "public_certs")) {
		if (nf >= MAX_KEYS) break;
		EVP_PKEY *k = pem_key(pf_json_str(e, "cert", ""));
		if (!k) continue;
		pf_strlcpy(fresh[nf].kid, pf_json_str(e, "kid", ""), sizeof fresh[nf].kid);
		fresh[nf++].k = k;
	}
	cJSON_Delete(j);
	if (!nf) { LOGW(TAG, "Access keys for team %s: none usable", team); return; }
	pthread_mutex_lock(&g_keys_mu);
	for (int i = 0; i < g_nkeys; i++) EVP_PKEY_free(g_keys[i].k);
	for (int i = 0; i < nf; i++) { pf_strlcpy(g_keys[i].kid, fresh[i].kid, sizeof g_keys[i].kid); g_keys[i].k = fresh[i].k; }
	g_nkeys = nf;
	pf_strlcpy(g_keys_team, team, sizeof g_keys_team);
	g_keys_t = pf_now();
	pthread_mutex_unlock(&g_keys_mu);
	LOGI(TAG, "loaded %d Access signing key(s) for team %s", nf, team);
}

/* The key for `kid`, with a reference the caller frees; NULL when the team has no such key. */
static EVP_PKEY *find_key(const char *team, const char *kid, bool *fresh)
{
	EVP_PKEY *k = NULL;
	pthread_mutex_lock(&g_keys_mu);
	if (!strcmp(g_keys_team, team))
		for (int i = 0; i < g_nkeys; i++) if (!strcmp(g_keys[i].kid, kid)) { k = g_keys[i].k; EVP_PKEY_up_ref(k); break; }
	if (fresh) *fresh = !strcmp(g_keys_team, team) && pf_now() - g_keys_t < 12 * 3600;
	pthread_mutex_unlock(&g_keys_mu);
	return k;
}

static EVP_PKEY *key_for(const char *team, const char *kid)
{
	bool fresh;
	EVP_PKEY *k = find_key(team, kid, &fresh);
	if (k && fresh) return k;
	/* one fetch at a time: whoever waited behind it finds the result rather than fetching again */
	pthread_mutex_lock(&g_fetch_mu);
	EVP_PKEY_free(k);
	k = find_key(team, kid, &fresh);
	if (!k || !fresh) {
		pthread_mutex_lock(&g_keys_mu);
		/* the throttle is by the team last tried, not the team last loaded: a fetch that fails
		 * loads nothing, and must not leave every request after it free to try again */
		bool may = strcmp(g_try_team, team) || pf_now() - g_keys_try > 30;
		if (may) { g_keys_try = pf_now(); pf_strlcpy(g_try_team, team, sizeof g_try_team); }
		pthread_mutex_unlock(&g_keys_mu);
		if (may) {
			fetch_keys(team);
			EVP_PKEY_free(k);
			k = find_key(team, kid, NULL);
		}
	}
	pthread_mutex_unlock(&g_fetch_mu);
	return k;   /* a key we had still stands when the refresh failed */
}

int pf_cloudflare_check(const char *jwt, const char *host, char *why, size_t n)
{
	char team[64], aud[160];
	if (!team_name(team, sizeof team) || !aud_tag(aud, sizeof aud)) {
		snprintf(why, n, "Cloudflare Access is not set up on the grill: enter the team name and the application's audience (AUD) tag in Settings, Network, Cloudflare, from the local network");
		return -1;
	}
	if (!jwt || !*jwt) { snprintf(why, n, "this request did not come through Cloudflare Access: add an Access application with a policy for this hostname"); return -1; }
	jwt_parts jp;
	if (jwt_claims(jwt, team, aud, pf_now(), &jp, why, n)) return -1;
	EVP_PKEY *k = key_for(team, jp.kid);
	if (!k) { snprintf(why, n, "no signing key for this Access token (cannot reach %s.cloudflareaccess.com?)", team); return -1; }
	bool ok = sig_ok(k, jwt, &jp);
	EVP_PKEY_free(k);
	if (!ok) { snprintf(why, n, "Access token signature does not verify"); return -1; }
	remember_host(host);
	return 0;
}

#endif

/* ---------------- the page ---------------- */

cJSON *pf_cloudflare_status_json(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "busy", atomic_load(&g_busy));
	pthread_mutex_lock(&g_mu);
	cJSON_AddStringToObject(o, "last_action", g_action);
	if (g_have_result) { cJSON_AddBoolToObject(o, "last_ok", g_last_rc == 0); cJSON_AddStringToObject(o, "last_output", g_output); }
	pthread_mutex_unlock(&g_mu);
	char team[64] = "", aud[160] = "", host[128];
	team_name(team, sizeof team);
	aud_tag(aud, sizeof aud);
	pf_set_str("network.cloudflare_hostname", host, sizeof host, "");
	cJSON_AddStringToObject(o, "team", team);
	cJSON_AddStringToObject(o, "aud", aud);
	cJSON_AddStringToObject(o, "hostname", host);
	cJSON_AddBoolToObject(o, "verify", can_verify());
	cJSON_AddNumberToObject(o, "port", pf_set_int("web.port", 80));

	pf_status st;
	pf_status_get(&st);
	if (st.sim || !pf_file_exists(HELPER)) { cJSON_AddBoolToObject(o, "installed", false); cJSON_AddStringToObject(o, "state", st.sim ? "Simulator" : "NoHelper"); return o; }
	const char *argv[] = { "sudo", "-n", HELPER, "status", NULL };
	char out[512];
	int rc = pf_run_capture(argv, out, sizeof out, 15);
	cJSON *js = rc == 0 ? cJSON_Parse(out) : NULL;
	if (!js) { cJSON_AddBoolToObject(o, "installed", false); cJSON_AddStringToObject(o, "state", "Unknown"); return o; }
	bool active = pf_json_bool(js, "active", false);
	int conns = active ? ready_connections() : -1;
	cJSON_AddBoolToObject(o, "installed", pf_json_bool(js, "installed", false));
	cJSON_AddStringToObject(o, "version", pf_json_str(js, "version", ""));
	cJSON_AddBoolToObject(o, "token", pf_json_bool(js, "token", false));
	cJSON_AddBoolToObject(o, "enabled", pf_json_bool(js, "enabled", false));
	cJSON_AddBoolToObject(o, "active", active);
	cJSON_AddBoolToObject(o, "online", conns > 0);
	cJSON_AddNumberToObject(o, "connections", conns > 0 ? conns : 0);
	cJSON_AddStringToObject(o, "state", !pf_json_bool(js, "installed", false) ? "NotInstalled" : conns > 0 ? "Connected" : active ? "Connecting" : pf_json_bool(js, "token", false) ? "Stopped" : "NoToken");
	cJSON_Delete(js);
	return o;
}
