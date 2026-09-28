#define _GNU_SOURCE
#include "web/server.h"
#include "features/alarms.h"
#include "core/cmdq.h"
#include "core/db.h"
#include "core/embedded.h"
#include "core/events.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/util.h"
#include "controllers/registry.h"
#include "net/sysinfo.h"
#include "probes/probes.h"
#include "web/api.h"
#include <civetweb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "web"
/* Each WebSocket holds a civetweb worker thread for as long as it is open. The cap sits well below
 * the worker count, so however many sockets a phone leaves behind, the API still has threads. */
#define MAX_WS 8
#define NUM_THREADS "20"

extern const pf_embedded_file pf_web_files[];
extern const size_t pf_web_count;

static struct mg_context *g_ctx;
static pthread_mutex_t g_ws_mu = PTHREAD_MUTEX_INITIALIZER;
/* One writer per client.
 *
 * Every write to a client used to happen on the push thread, one client after another, under one
 * lock. A phone whose screen has gone off leaves a socket that accepts nothing and never closes:
 * writing to it fills the kernel buffer and then blocks until civetweb's request timeout, and for
 * that long nobody else got an update. That is also what kept the request timeout at four seconds,
 * which was too short for a request arriving through a Tailscale tunnel that is still waking: the
 * tunnel opens its connection to us before the phone has finished its TLS handshake, and we gave
 * up on it before the request arrived.
 *
 * Now each client has a mailbox and a thread that writes it out. Posting is instant; a client
 * that will not take data stalls only its own thread, and is let go of on the first failed write.
 * The newest status replaces an unsent one (only the latest matters); events and alarm notices
 * queue in order. */
typedef struct {
	struct mg_connection *conn;
	pthread_t tid;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	char *status; size_t status_len;
	char *q[16]; size_t qlen[16]; int qn;
	bool stop, dead;
} wsc;
static wsc *g_cl[MAX_WS];
/* sockets whose worker thread is still running: civetweb gives each its own thread for as long as
 * it is open, so this is what is capped, not the table above */
static int g_ws_open;

static void *ws_writer(void *arg)
{
	wsc *c = arg;
	pthread_setname_np(pthread_self(), "pf-wsw");
	pthread_mutex_lock(&c->mu);
	for (;;) {
		while (!c->stop && !c->status && !c->qn) pthread_cond_wait(&c->cv, &c->mu);
		if (c->stop) break;
		char *q[16]; size_t ql[16]; int qn = c->qn;
		memcpy(q, c->q, sizeof q); memcpy(ql, c->qlen, sizeof ql);
		c->qn = 0;
		char *st = c->status; size_t sl = c->status_len;
		c->status = NULL;
		bool dead = c->dead;
		pthread_mutex_unlock(&c->mu);
		for (int i = 0; i < qn; i++) {
			if (!dead) { int n = mg_websocket_write(c->conn, MG_WEBSOCKET_OPCODE_TEXT, q[i], ql[i]); if (n <= 0 || (size_t)n < ql[i]) dead = true; }
			free(q[i]);
		}
		if (st) {
			if (!dead) { int n = mg_websocket_write(c->conn, MG_WEBSOCKET_OPCODE_TEXT, st, sl); if (n <= 0 || (size_t)n < sl) dead = true; }
			free(st);
		}
		pthread_mutex_lock(&c->mu);
		if (dead && !c->dead) { c->dead = true; LOGW(TAG, "websocket client would not take an update, dropping it"); }
	}
	pthread_mutex_unlock(&c->mu);
	return NULL;
}

/* hand a message to every client's writer; `latest` replaces an unsent message of its kind */
static void ws_broadcast_ex(const char *txt, size_t len, bool latest)
{
	if (!txt || !len) return;
	pthread_mutex_lock(&g_ws_mu);
	for (int i = 0; i < MAX_WS; i++) {
		wsc *c = g_cl[i];
		if (!c) continue;
		pthread_mutex_lock(&c->mu);
		if (!c->dead) {
			char *copy = malloc(len);
			if (copy) {
				memcpy(copy, txt, len);
				if (latest) { free(c->status); c->status = copy; c->status_len = len; }
				else if (c->qn < 16) { c->q[c->qn] = copy; c->qlen[c->qn] = len; c->qn++; }
				else free(copy);   /* a client sixteen messages behind is not reading */
				pthread_cond_signal(&c->cv);
			}
		}
		pthread_mutex_unlock(&c->mu);
	}
	pthread_mutex_unlock(&g_ws_mu);
}
static void ws_broadcast(const char *txt, size_t len) { ws_broadcast_ex(txt, len, false); }

/* a direct answer on the client's own thread: civetweb serialises writes to one connection */
static int ws_send(struct mg_connection *conn, int op, const char *data, size_t len)
{
	return mg_websocket_write(conn, op, data, len);
}

static pthread_t g_push_tid;
static atomic_bool g_run;

/* ---------------- helpers ---------------- */

static const char *content_type(const char *name)
{
	const char *ext = strrchr(name, '.');
	if (!ext) return "application/octet-stream";
	if (!strcmp(ext, ".html")) return "text/html; charset=utf-8";
	if (!strcmp(ext, ".js")) return "application/javascript; charset=utf-8";
	if (!strcmp(ext, ".css")) return "text/css; charset=utf-8";
	if (!strcmp(ext, ".json")) return "application/json";
	if (!strcmp(ext, ".webmanifest")) return "application/manifest+json";
	if (!strcmp(ext, ".svg")) return "image/svg+xml";
	if (!strcmp(ext, ".png")) return "image/png";
	if (!strcmp(ext, ".ico")) return "image/x-icon";
	if (!strcmp(ext, ".woff2")) return "font/woff2";
	return "application/octet-stream";
}

static int serve_embedded(struct mg_connection *conn, const char *path)
{
	if (!strcmp(path, "/") || !*path) path = "/index.html";
	/* assets are embedded by basename (web/pages/x.js -> x.js) */
	const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	char gzname[128];
	snprintf(gzname, sizeof gzname, "%s.gz", name);
	const pf_embedded_file *f = NULL;
	bool gz = false;
	for (size_t i = 0; i < pf_web_count; i++) {
		if (!strcmp(pf_web_files[i].name, gzname)) { f = &pf_web_files[i]; gz = true; break; }
		if (!strcmp(pf_web_files[i].name, name)) f = &pf_web_files[i];
	}
	if (!f) {
		/* SPA fallback: unknown non-API paths get the app shell */
		if (strncmp(path, "/api/", 5) && !strchr(name, '.')) return serve_embedded(conn, "/index.html");
		mg_send_http_error(conn, 404, "not found");
		return 404;
	}
	/* weak content hash as ETag: browsers revalidate (no-cache) and get a cheap 304 */
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < f->len; i++) { h ^= f->data[i]; h *= 16777619u; }
	char etag[32];
	snprintf(etag, sizeof etag, "\"%08x-%zx\"", h, f->len);
	const char *inm = mg_get_header(conn, "If-None-Match");
	if (inm && !strcmp(inm, etag)) {
		mg_printf(conn, "HTTP/1.1 304 Not Modified\r\nETag: %s\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", etag);
		return 304;
	}
	mg_printf(conn,
	          "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s"
	          "ETag: %s\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
	          content_type(name), f->len, gz ? "Content-Encoding: gzip\r\n" : "", etag);
	mg_write(conn, f->data, f->len);
	return 200;
}

/* ---------------- websocket ---------------- */

static int ws_connect(const struct mg_connection *conn, void *cbdata)
{
	(void)conn; (void)cbdata;
	/* counted by thread, not by table slot: a ghost that was dropped from the table still holds a
	 * worker, and counting slots let reconnects pile ghosts up until no thread was left for HTTP */
	pthread_mutex_lock(&g_ws_mu);
	bool full = g_ws_open >= MAX_WS;
	if (!full) g_ws_open++;
	pthread_mutex_unlock(&g_ws_mu);
	if (full) { LOGW(TAG, "websocket client limit (%d) reached, rejecting", MAX_WS); return 1; }
	return 0;
}

static void ws_ready(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	wsc *c = calloc(1, sizeof *c);
	if (!c) return;
	c->conn = conn;
	pthread_mutex_init(&c->mu, NULL);
	pthread_cond_init(&c->cv, NULL);
	if (pthread_create(&c->tid, NULL, ws_writer, c)) { free(c); return; }
	pthread_mutex_lock(&g_ws_mu);
	bool placed = false;
	for (int i = 0; i < MAX_WS && !placed; i++) if (!g_cl[i]) { g_cl[i] = c; placed = true; }
	pthread_mutex_unlock(&g_ws_mu);
	if (!placed) {
		pthread_mutex_lock(&c->mu); c->stop = true; pthread_cond_signal(&c->cv); pthread_mutex_unlock(&c->mu);
		pthread_join(c->tid, NULL);
		free(c);
		return;
	}
	pf_web_push_status();
}

static int ws_data(struct mg_connection *conn, int bits, char *data, size_t len, void *cbdata)
{
	(void)cbdata;
	/* Every write to a client goes through the same lock. Two threads writing to one connection
	 * interleave their frames, and a half-written frame is not something a browser recovers from:
	 * the broadcaster writes from the push thread while a pong or an error reply is written from
	 * this one. */
	if ((bits & 0x0f) == MG_WEBSOCKET_OPCODE_PING) { ws_send(conn, MG_WEBSOCKET_OPCODE_PONG, data, len); return 1; }
	/* the app's "are you there" after the phone wakes: answered at once, on this socket, so it can
	 * tell a live socket from one the tunnel has lost without tearing either down first */
	if ((bits & 0x0f) == MG_WEBSOCKET_OPCODE_TEXT && len == 6 && !memcmp(data, "\"ping\"", 6)) {
		ws_send(conn, MG_WEBSOCKET_OPCODE_TEXT, "{\"type\":\"pong\"}", 15);
		return 1;
	}
	if ((bits & 0x0f) == MG_WEBSOCKET_OPCODE_TEXT && len < 4096) {
		/* commands over WS use the same handler as POST /api/v1/cmd */
		char body[4096];
		memcpy(body, data, len);
		body[len] = 0;
		char err[128];
		if (pf_api_command_json(body, err, sizeof err)) {
			char msg[256];
			int n = snprintf(msg, sizeof msg, "{\"type\":\"error\",\"msg\":\"%s\"}", err);
			ws_send(conn, MG_WEBSOCKET_OPCODE_TEXT, msg, (size_t)n);
		}
	}
	return 1;
}

static void ws_close(const struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	wsc *c = NULL;
	pthread_mutex_lock(&g_ws_mu);
	for (int i = 0; i < MAX_WS; i++) if (g_cl[i] && g_cl[i]->conn == conn) { c = g_cl[i]; g_cl[i] = NULL; }
	if (g_ws_open > 0) g_ws_open--;
	pthread_mutex_unlock(&g_ws_mu);
	if (!c) return;
	/* the connection is about to go: its writer must be finished with it first */
	pthread_mutex_lock(&c->mu); c->stop = true; pthread_cond_signal(&c->cv); pthread_mutex_unlock(&c->mu);
	pthread_join(c->tid, NULL);
	free(c->status);
	for (int i = 0; i < c->qn; i++) free(c->q[i]);
	pthread_mutex_destroy(&c->mu);
	pthread_cond_destroy(&c->cv);
	free(c);
}

void pf_web_push_status(void)
{
	pf_status st;
	pf_status_get(&st);
	cJSON *j = pf_status_to_json(&st, pf_settings_units());
	cJSON_AddStringToObject(j, "type", "status");
	char *txt = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	if (!txt) return;
	size_t len = strlen(txt);
	ws_broadcast_ex(txt, len, true);
	free(txt);
}

static void *push_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-wspush");
	unsigned last_gen = 0, last_ev = pf_events_generation(), last_alm = pf_alarms_generation();
	double last_push = 0;
	while (atomic_load(&g_run)) {
		double now = pf_now();
		unsigned gen = pf_status_generation();
		if ((gen != last_gen && now - last_push >= 1.0) || now - last_push >= 5.0) {
			last_gen = gen;
			last_push = now;
			pf_web_push_status();
		}
		unsigned ev = pf_events_generation();
		if (ev != last_ev) {
			last_ev = ev;
			cJSON *arr = pf_events_recent_json(1);
			cJSON *e = cJSON_DetachItemFromArray(arr, 0);
			cJSON_Delete(arr);
			if (e) {
				cJSON_AddStringToObject(e, "type", "event");
				char *txt = cJSON_PrintUnformatted(e);
				cJSON_Delete(e);
				if (txt) {
					ws_broadcast(txt, strlen(txt));
					free(txt);
				}
			}
		}
		/* The alarm table is shared state, so every client is told the moment it changes -- that is
		 * what makes clearing something on one device clear it on the others. Only the generation
		 * goes over the wire; the client fetches the list, so a phone that was asleep gets the
		 * current state rather than a replay of what it missed. */
		unsigned alm = pf_alarms_generation();
		if (alm != last_alm) {
			last_alm = alm;
			char msg[64];
			int len = snprintf(msg, sizeof msg, "{\"type\":\"alarms\",\"gen\":%u}", alm);
			if (len > 0) ws_broadcast(msg, (size_t)len);
		}
		pf_sleep_ms(100);
	}
	return NULL;
}

/* ---------------- HTTP ---------------- */

/* A settings restore is the largest thing anything posts here; past that it is not a request we
 * should be trying to hold in memory on a Zero 2W. */
#define BODY_MAX (1u << 20)

static void send_json(struct mg_connection *conn, int status, const char *json)
{
	mg_printf(conn,
	          "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
	          "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
	          status, status < 300 ? "OK" : "Error", strlen(json));
	mg_write(conn, json, strlen(json));
}

/* Retried commands.
 *
 * Over a tunnel that is waking up, a request can reach us and its answer never make it back. The
 * app then tries again -- and without this, "start the grill" or "install" would run twice. A
 * request that carries X-Request-Id is remembered for a few minutes: the same id again gets the
 * first answer instead of running a second time, and one that arrives while the first is still
 * running waits for it. */
#define REPLAY_MAX 48
static struct { char id[48]; bool done; int status; char *json; double ts; } g_replay[REPLAY_MAX];
static pthread_mutex_t g_replay_mu = PTHREAD_MUTEX_INITIALIZER;

/* -1: new, go ahead (a slot is reserved); otherwise the index of a finished answer to repeat */
static int replay_begin(const char *id)
{
	double now = pf_now();
	for (int tries = 0; tries < 300; tries++) {
		pthread_mutex_lock(&g_replay_mu);
		int oldest = 0, found = -1;
		for (int i = 0; i < REPLAY_MAX; i++) {
			if (g_replay[i].id[0] && !strcmp(g_replay[i].id, id)) { found = i; break; }
			if (g_replay[i].ts < g_replay[oldest].ts) oldest = i;
		}
		if (found >= 0 && g_replay[found].done) { pthread_mutex_unlock(&g_replay_mu); return found; }
		if (found < 0) {
			free(g_replay[oldest].json);
			memset(&g_replay[oldest], 0, sizeof g_replay[oldest]);
			pf_strlcpy(g_replay[oldest].id, id, sizeof g_replay[oldest].id);
			g_replay[oldest].ts = now;
			pthread_mutex_unlock(&g_replay_mu);
			return -1;
		}
		pthread_mutex_unlock(&g_replay_mu);
		pf_sleep_ms(50);   /* the first copy is still running */
	}
	return -2;
}

static void replay_end(const char *id, int status, const char *json)
{
	pthread_mutex_lock(&g_replay_mu);
	for (int i = 0; i < REPLAY_MAX; i++) if (!strcmp(g_replay[i].id, id)) {
		g_replay[i].done = true;
		g_replay[i].status = status;
		free(g_replay[i].json);
		g_replay[i].json = strdup(json ? json : "{}");
		break;
	}
	pthread_mutex_unlock(&g_replay_mu);
}

static int api_handler(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	const struct mg_request_info *ri = mg_get_request_info(conn);
	/* The body is read onto the heap, sized from Content-Length. Eight civetweb workers each
	 * carrying a 64 KB request buffer on their stack was a lot of stack for a request that is
	 * usually a hundred bytes, and anything larger than the buffer used to be truncated into
	 * malformed JSON rather than refused. */
	char *body = NULL;
	size_t cap = 0;
	int blen = 0;
	if (!strcmp(ri->request_method, "POST") || !strcmp(ri->request_method, "PUT") || !strcmp(ri->request_method, "PATCH")) {
		long long cl = ri->content_length;
		/* a backup being restored is a few megabytes of tar.gz; everything else is a few hundred bytes of JSON */
		size_t body_max = strstr(ri->local_uri, "/backup/restore") ? (64u << 20) : BODY_MAX;
		if (cl > (long long)body_max) { send_json(conn, 413, "{\"error\":\"request too large\"}"); return 413; }
		cap = cl > 0 ? (size_t)cl + 1 : 4096;
		if (!(body = malloc(cap))) { send_json(conn, 503, "{\"error\":\"out of memory\"}"); return 503; }
		int r;
		while ((r = mg_read(conn, body + blen, cap - 1 - (size_t)blen)) > 0) {
			blen += r;
			if ((size_t)blen + 1 < cap) continue;
			if (cap >= body_max) { free(body); send_json(conn, 413, "{\"error\":\"request too large\"}"); return 413; }
			cap = cap > body_max / 2 ? body_max : cap * 2;
			char *nb = realloc(body, cap);
			if (!nb) { free(body); send_json(conn, 503, "{\"error\":\"out of memory\"}"); return 503; }
			body = nb;
		}
		body[blen] = 0;
	}

	pf_api_req req = {
		.method = ri->request_method,
		.path = ri->local_uri + 7, /* strip "/api/v1" */
		.query = ri->query_string ? ri->query_string : "",
		.body = body ? body : "",
		.body_len = (size_t)blen,
	};
	pf_api_resp resp = { 0 };
	char rid[48] = "";
	const char *hid = strcmp(ri->request_method, "GET") ? mg_get_header(conn, "X-Request-Id") : NULL;
	if (hid && *hid && strlen(hid) < sizeof rid) pf_strlcpy(rid, hid, sizeof rid);
	if (rid[0]) {
		int r = replay_begin(rid);
		if (r >= 0) {
			pthread_mutex_lock(&g_replay_mu);
			int st = g_replay[r].status;
			char *copy = strdup(g_replay[r].json ? g_replay[r].json : "{}");
			pthread_mutex_unlock(&g_replay_mu);
			send_json(conn, st, copy ? copy : "{}");
			free(copy);
			free(body);
			return st;
		}
		if (r == -2) { free(body); send_json(conn, 503, "{\"result\":\"ERROR\",\"message\":\"still working on it\"}"); return 503; }
	}
	pf_api_dispatch(&req, &resp);
	if (rid[0]) replay_end(rid, resp.status, resp.json);
	send_json(conn, resp.status, resp.json ? resp.json : "{}");
	free(resp.json);
	free(body);
	return resp.status;
}

static int captive_handler(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	/* OS captive-portal probes: answer with a redirect to the setup page when in hotspot mode,
	 * otherwise with the expected success body so phones don't think the network is captive. */
	if (pf_api_hotspot_active()) {
		mg_printf(conn, "HTTP/1.1 302 Found\r\nLocation: http://10.42.0.1/setup\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		return 302;
	}
	const char *uri = mg_get_request_info(conn)->local_uri;
	if (strstr(uri, "generate_204")) { mg_printf(conn, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n"); return 204; }
	const char *body = strstr(uri, "hotspot-detect") ? "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>" :
	                   strstr(uri, "connecttest") ? "Microsoft Connect Test" : "Microsoft NCSI";
	mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
	return 200;
}

static int static_handler(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	const struct mg_request_info *ri = mg_get_request_info(conn);
	if (strcmp(ri->request_method, "GET") && strcmp(ri->request_method, "HEAD")) { mg_send_http_error(conn, 405, "method not allowed"); return 405; }
	return serve_embedded(conn, ri->local_uri);
}

static int log_message(const struct mg_connection *conn, const char *message)
{
	(void)conn;
	LOGW(TAG, "civetweb: %s", message);
	return 1;
}

int pf_web_start(const char *bind_addr, int port)
{
	char listen[64];
	snprintf(listen, sizeof listen, "%s:%d", bind_addr && *bind_addr ? bind_addr : "0.0.0.0", port);
	const char *options[] = {
		"listening_ports", listen,
		/* every worker thread is pinned for the life of a keep-alive or WebSocket connection, so
		 * keep-alive stays off (browsers open 6+ sockets) and WebSocket clients are capped below
		 * the worker count */
		"num_threads", NUM_THREADS,
		"enable_keep_alive", "no",
		/* How long a request may take to arrive, and a write may stall. Through Tailscale the
		 * connection to us opens when the phone connects, before its TLS handshake -- over a tunnel
		 * still waking up that can take seconds, and four was not enough. Writes to WebSocket
		 * clients have their own threads now (see ws_writer), so a stall costs only that client. */
		"request_timeout_ms", "15000",
		/* A socket whose phone has gone to sleep never closes by itself: civetweb's reader just
		 * times out and waits again, for ever, holding its thread. With ping/pong on, every quiet
		 * interval sends a ping, and a socket that leaves five unanswered is closed. A live
		 * browser answers pings on its own. Ten seconds puts a dead socket down within a minute. */
		"websocket_timeout_ms", "10000",
		"enable_websocket_ping_pong", "yes",
		"enable_directory_listing", "no",
		"tcp_nodelay", "1",
		NULL
	};
	struct mg_callbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.log_message = log_message;
	mg_init_library(0);
	g_ctx = mg_start(&cb, NULL, options);
	if (!g_ctx) { LOGE(TAG, "failed to start web server on %s", listen); return -1; }
	mg_set_request_handler(g_ctx, "/api/v1/", api_handler, NULL);
	mg_set_websocket_handler(g_ctx, "/ws", ws_connect, ws_ready, ws_data, ws_close, NULL);
	mg_set_request_handler(g_ctx, "/generate_204", captive_handler, NULL);
	mg_set_request_handler(g_ctx, "/gen_204", captive_handler, NULL);
	mg_set_request_handler(g_ctx, "/hotspot-detect.html", captive_handler, NULL);
	mg_set_request_handler(g_ctx, "/library/test/success.html", captive_handler, NULL);
	mg_set_request_handler(g_ctx, "/connecttest.txt", captive_handler, NULL);
	mg_set_request_handler(g_ctx, "/ncsi.txt", captive_handler, NULL);
	mg_set_request_handler(g_ctx, "/", static_handler, NULL);
	atomic_store(&g_run, true);
	pthread_create(&g_push_tid, NULL, push_thread, NULL);
	LOGI(TAG, "listening on http://%s", listen);
	return 0;
}

void pf_web_stop(void)
{
	if (!g_ctx) return;
	atomic_store(&g_run, false);
	pthread_join(g_push_tid, NULL);
	mg_stop(g_ctx);
	g_ctx = NULL;
	mg_exit_library();
}
