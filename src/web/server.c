#define _GNU_SOURCE
#include "web/server.h"
#include "core/cmdq.h"
#include "core/db.h"
#include "core/embedded.h"
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
#define MAX_WS 6

extern const pf_embedded_file pf_web_files[];
extern const size_t pf_web_count;

static struct mg_context *g_ctx;
static pthread_mutex_t g_ws_mu = PTHREAD_MUTEX_INITIALIZER;
static struct mg_connection *g_ws[MAX_WS];
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
	int n = 0;
	pthread_mutex_lock(&g_ws_mu);
	for (int i = 0; i < MAX_WS; i++) if (g_ws[i]) n++;
	pthread_mutex_unlock(&g_ws_mu);
	if (n >= MAX_WS) { LOGW(TAG, "websocket client limit (%d) reached, rejecting", MAX_WS); return 1; }
	return 0;
}

static void ws_ready(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	pthread_mutex_lock(&g_ws_mu);
	for (int i = 0; i < MAX_WS; i++) if (!g_ws[i]) { g_ws[i] = conn; break; }
	pthread_mutex_unlock(&g_ws_mu);
	pf_web_push_status();
}

static int ws_data(struct mg_connection *conn, int bits, char *data, size_t len, void *cbdata)
{
	(void)cbdata;
	if ((bits & 0x0f) == MG_WEBSOCKET_OPCODE_PING) { mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_PONG, data, len); return 1; }
	if ((bits & 0x0f) == MG_WEBSOCKET_OPCODE_TEXT && len < 4096) {
		/* commands over WS use the same handler as POST /api/v1/cmd */
		char body[4096];
		memcpy(body, data, len);
		body[len] = 0;
		char err[128];
		if (pf_api_command_json(body, err, sizeof err)) {
			char msg[256];
			int n = snprintf(msg, sizeof msg, "{\"type\":\"error\",\"msg\":\"%s\"}", err);
			mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, msg, (size_t)n);
		}
	}
	return 1;
}

static void ws_close(const struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	pthread_mutex_lock(&g_ws_mu);
	for (int i = 0; i < MAX_WS; i++) if (g_ws[i] == conn) g_ws[i] = NULL;
	pthread_mutex_unlock(&g_ws_mu);
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
	pthread_mutex_lock(&g_ws_mu);
	for (int i = 0; i < MAX_WS; i++)
		if (g_ws[i]) mg_websocket_write(g_ws[i], MG_WEBSOCKET_OPCODE_TEXT, txt, len);
	pthread_mutex_unlock(&g_ws_mu);
	free(txt);
}

static void *push_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-wspush");
	unsigned last_gen = 0;
	double last_push = 0;
	while (atomic_load(&g_run)) {
		double now = pf_now();
		unsigned gen = pf_status_generation();
		if ((gen != last_gen && now - last_push >= 1.0) || now - last_push >= 5.0) {
			last_gen = gen;
			last_push = now;
			pf_web_push_status();
		}
		pf_sleep_ms(100);
	}
	return NULL;
}

/* ---------------- HTTP ---------------- */

static int api_handler(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	const struct mg_request_info *ri = mg_get_request_info(conn);
	char body[65536];
	int blen = 0;
	if (!strcmp(ri->request_method, "POST") || !strcmp(ri->request_method, "PUT") || !strcmp(ri->request_method, "PATCH")) {
		int r;
		while (blen < (int)sizeof body - 1 && (r = mg_read(conn, body + blen, sizeof body - 1 - (size_t)blen)) > 0) blen += r;
	}
	body[blen] = 0;

	pf_api_req req = {
		.method = ri->request_method,
		.path = ri->local_uri + 7, /* strip "/api/v1" */
		.query = ri->query_string ? ri->query_string : "",
		.body = body,
		.body_len = (size_t)blen,
	};
	pf_api_resp resp = { 0 };
	pf_api_dispatch(&req, &resp);

	const char *json = resp.json ? resp.json : "{}";
	mg_printf(conn,
	          "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
	          "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
	          resp.status, resp.status < 300 ? "OK" : "Error", strlen(json));
	mg_write(conn, json, strlen(json));
	free(resp.json);
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
		"num_threads", "12",
		"enable_keep_alive", "no",
		"request_timeout_ms", "10000",
		"websocket_timeout_ms", "60000",
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
