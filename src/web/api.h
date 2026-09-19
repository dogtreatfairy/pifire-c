#pragma once
/* REST API dispatch, independent of the HTTP library so it can be unit-tested. */
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	const char *method;   /* GET/POST/PUT/PATCH/DELETE */
	const char *path;     /* below /api/v1, e.g. "/status" */
	const char *query;    /* raw query string or "" */
	const char *body;     /* NUL-terminated */
	size_t body_len;
} pf_api_req;

typedef struct {
	int status;
	char *json;           /* malloc'd; may be NULL */
} pf_api_resp;

void pf_api_dispatch(const pf_api_req *req, pf_api_resp *resp);
/* {"cmd":"mode","mode":"Hold","setpoint":225} etc. Shared by POST /api/v1/cmd and the WebSocket. */
int  pf_api_command_json(const char *json, char *err, size_t errn);
bool pf_api_hotspot_active(void);
void pf_api_set_hotspot_active(bool on);
