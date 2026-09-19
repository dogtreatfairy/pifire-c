#define _GNU_SOURCE
#include "probes/ble/bluez.h"
#include "core/log.h"
#include "core/util.h"
#include <cJSON.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "ble"

#if PF_WITH_BLE
#include <systemd/sd-bus.h>

#define BLUEZ "org.bluez"
#define MAX_DEVS 8
#define MAX_CHARS 48
#define MAX_SERVICES 16
#define QLEN 32

typedef enum { ST_IDLE, ST_FOUND, ST_CONNECTING, ST_CONNECTED, ST_READY, ST_BACKOFF } state_t;

typedef struct { char uuid[40]; char path[128]; } chr_t;

struct pf_ble_dev {
	bool used, want_release;
	pf_ble_spec spec;
	state_t st;
	char path[128];          /* /org/bluez/hci0/dev_XX_XX_.. */
	char address[20], name[64];
	chr_t chars[MAX_CHARS];
	int nchars;
	char services[MAX_SERVICES][40];
	int nservices;
	double next_action, last_poll;
	int backoff_s;
	atomic_bool connected;
	atomic_int battery;
	bool notified[8];
};

typedef struct { pf_ble_dev *d; char uuid[40]; uint8_t data[32]; size_t len; bool resp; } wreq_t;

static pthread_t g_tid;
static atomic_bool g_run, g_avail;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pf_ble_dev g_devs[MAX_DEVS];
static wreq_t g_q[QLEN];
static int g_qhead, g_qlen;
static sd_bus *g_bus;
static char g_adapter[64] = "/org/bluez/hci0";
static bool g_discovering;
static double g_discover_until;
/* scan requests from other threads (the bus is only ever touched by the manager thread) */
static atomic_int g_scan_seconds;      /* >0 = requested */
static double g_scan_deadline;
static cJSON *g_scan_result;
static unsigned g_scan_gen;

const char *pf_ble_uuid16(const char *short4, char out[40])
{
	snprintf(out, 40, "0000%s-0000-1000-8000-00805f9b34fb", short4);
	for (char *p = out; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
	return out;
}

static bool uuid_eq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

/* ---------------- D-Bus helpers ---------------- */

static int call_void(const char *path, const char *iface, const char *method)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *rep = NULL;
	int r = sd_bus_call_method(g_bus, BLUEZ, path, iface, method, &err, &rep, "");
	if (r < 0) LOGD(TAG, "%s.%s on %s: %s", iface, method, path, err.message ? err.message : strerror(-r));
	sd_bus_error_free(&err);
	sd_bus_message_unref(rep);
	return r;
}

static int get_bool_prop(const char *path, const char *iface, const char *prop, bool *out)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	int v = 0;
	int r = sd_bus_get_property_trivial(g_bus, BLUEZ, path, iface, prop, &err, 'b', &v);
	sd_bus_error_free(&err);
	if (r >= 0) *out = v != 0;
	return r;
}

static int get_str_prop(const char *path, const char *iface, const char *prop, char *out, size_t n)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	char *s = NULL;
	int r = sd_bus_get_property_string(g_bus, BLUEZ, path, iface, prop, &err, &s);
	sd_bus_error_free(&err);
	if (r >= 0) { pf_strlcpy(out, s ? s : "", n); free(s); }
	return r;
}

static int get_byte_prop(const char *path, const char *iface, const char *prop)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	uint8_t v = 0;
	int r = sd_bus_get_property_trivial(g_bus, BLUEZ, path, iface, prop, &err, 'y', &v);
	sd_bus_error_free(&err);
	return r < 0 ? -1 : v;
}

static int get_int16_prop(const char *path, const char *iface, const char *prop)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	int16_t v = 0;
	int r = sd_bus_get_property_trivial(g_bus, BLUEZ, path, iface, prop, &err, 'n', &v);
	sd_bus_error_free(&err);
	return r < 0 ? 0 : v;
}

/* Iterate GetManagedObjects: cb(path, iface) for every (object, interface) pair. */
typedef void (*obj_cb)(const char *path, const char *iface, sd_bus_message *props, void *ctx);
static int for_each_object(obj_cb cb, void *ctx)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *rep = NULL;
	int r = sd_bus_call_method(g_bus, BLUEZ, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &err, &rep, "");
	if (r < 0) { sd_bus_error_free(&err); return r; }
	r = sd_bus_message_enter_container(rep, 'a', "{oa{sa{sv}}}");
	while (r > 0 && (r = sd_bus_message_enter_container(rep, 'e', "oa{sa{sv}}")) > 0) {
		const char *path;
		sd_bus_message_read(rep, "o", &path);
		sd_bus_message_enter_container(rep, 'a', "{sa{sv}}");
		while (sd_bus_message_enter_container(rep, 'e', "sa{sv}") > 0) {
			const char *iface;
			sd_bus_message_read(rep, "s", &iface);
			cb(path, iface, rep, ctx);   /* cb may consume the a{sv}; skip whatever is left */
			sd_bus_message_skip(rep, NULL);
			sd_bus_message_exit_container(rep);
		}
		sd_bus_message_exit_container(rep);
		sd_bus_message_exit_container(rep);
	}
	sd_bus_message_unref(rep);
	return 0;
}

/* ---------------- device matching ---------------- */

typedef struct { pf_ble_dev *d; bool found; } match_ctx;

static bool spec_matches(const pf_ble_spec *s, const char *addr, const char *name)
{
	if (s->address && *s->address) return strcasecmp(s->address, addr) == 0;
	if (!s->name_match || !*name) return false;
	if (s->name_exclude && strcasestr(name, s->name_exclude)) return false;
	return strcasestr(name, s->name_match) != NULL;
}

static void match_cb(const char *path, const char *iface, sd_bus_message *props, void *ctx)
{
	(void)props;
	match_ctx *m = ctx;
	if (m->found || strcmp(iface, "org.bluez.Device1")) return;
	char addr[20] = "", name[64] = "";
	get_str_prop(path, "org.bluez.Device1", "Address", addr, sizeof addr);
	get_str_prop(path, "org.bluez.Device1", "Name", name, sizeof name);
	if (!name[0]) get_str_prop(path, "org.bluez.Device1", "Alias", name, sizeof name);
	if (spec_matches(&m->d->spec, addr, name)) {
		pf_strlcpy(m->d->path, path, sizeof m->d->path);
		pf_strlcpy(m->d->address, addr, sizeof m->d->address);
		pf_strlcpy(m->d->name, name, sizeof m->d->name);
		m->found = true;
	}
}

static void chars_cb(const char *path, const char *iface, sd_bus_message *props, void *ctx)
{
	(void)props;
	pf_ble_dev *d = ctx;
	size_t pl = strlen(d->path);
	if (strncmp(path, d->path, pl) || path[pl] != '/') return;
	if (!strcmp(iface, "org.bluez.GattService1") && d->nservices < MAX_SERVICES) {
		get_str_prop(path, iface, "UUID", d->services[d->nservices], 40);
		d->nservices++;
	} else if (!strcmp(iface, "org.bluez.GattCharacteristic1") && d->nchars < MAX_CHARS) {
		get_str_prop(path, iface, "UUID", d->chars[d->nchars].uuid, 40);
		pf_strlcpy(d->chars[d->nchars].path, path, sizeof d->chars[d->nchars].path);
		d->nchars++;
	}
}

static const char *char_path(const pf_ble_dev *d, const char *uuid)
{
	for (int i = 0; i < d->nchars; i++) if (uuid_eq(d->chars[i].uuid, uuid)) return d->chars[i].path;
	return NULL;
}

static void set_discovery(bool on)
{
	if (on == g_discovering) return;
	if (on) {
		sd_bus_error err = SD_BUS_ERROR_NULL;
		sd_bus_message *rep = NULL;
		sd_bus_call_method(g_bus, BLUEZ, g_adapter, "org.bluez.Adapter1", "SetDiscoveryFilter", &err, &rep, "a{sv}", 1, "Transport", "s", "le");
		sd_bus_error_free(&err);
		sd_bus_message_unref(rep);
		if (call_void(g_adapter, "org.bluez.Adapter1", "StartDiscovery") >= 0) { g_discovering = true; LOGI(TAG, "discovery started"); }
	} else {
		call_void(g_adapter, "org.bluez.Adapter1", "StopDiscovery");
		g_discovering = false;
		LOGI(TAG, "discovery stopped");
	}
}

/* ---------------- device state machine ---------------- */

static void dev_disconnected(pf_ble_dev *d, double now)
{
	bool was = atomic_exchange(&d->connected, false);
	if (was && d->spec.on_disconnected) d->spec.on_disconnected(d, d->spec.ctx);
	d->st = ST_BACKOFF;
	d->backoff_s = d->backoff_s ? (d->backoff_s * 2 > 60 ? 60 : d->backoff_s * 2) : 5;
	d->next_action = now + d->backoff_s;
	d->nchars = d->nservices = 0;
	memset(d->notified, 0, sizeof d->notified);
	if (was) LOGW(TAG, "%s (%s) disconnected; retry in %d s", d->name, d->address, d->backoff_s);
}

static void dev_step(pf_ble_dev *d, double now)
{
	switch (d->st) {
	case ST_IDLE: {
		match_ctx m = { d, false };
		for_each_object(match_cb, &m);
		if (m.found) { d->st = ST_FOUND; LOGI(TAG, "found %s (%s)", d->name, d->address); }
		else { set_discovery(true); g_discover_until = now + 30; }
		break;
	}
	case ST_BACKOFF:
		if (now >= d->next_action) d->st = d->path[0] ? ST_FOUND : ST_IDLE;
		break;
	case ST_FOUND: {
		bool conn = false;
		get_bool_prop(d->path, "org.bluez.Device1", "Connected", &conn);
		if (!conn) {
			sd_bus_error err = SD_BUS_ERROR_NULL;
			sd_bus_message *rep = NULL;
			LOGI(TAG, "connecting to %s (%s)", d->name, d->address);
			int r = sd_bus_call_method(g_bus, BLUEZ, d->path, "org.bluez.Device1", "Connect", &err, &rep, "");
			sd_bus_message_unref(rep);
			if (r < 0) {
				LOGW(TAG, "connect %s failed: %s", d->address, err.message ? err.message : strerror(-r));
				sd_bus_error_free(&err);
				if (r == -ENXIO || (err.name && strstr(err.name, "UnknownObject"))) d->path[0] = 0;
				d->st = ST_BACKOFF;
				d->backoff_s = d->backoff_s ? (d->backoff_s * 2 > 60 ? 60 : d->backoff_s * 2) : 5;
				d->next_action = now + d->backoff_s;
				break;
			}
			sd_bus_error_free(&err);
		}
		d->st = ST_CONNECTING;
		d->next_action = now + 20;
		break;
	}
	case ST_CONNECTING: {
		bool resolved = false, conn = false;
		get_bool_prop(d->path, "org.bluez.Device1", "Connected", &conn);
		get_bool_prop(d->path, "org.bluez.Device1", "ServicesResolved", &resolved);
		if (conn && resolved) {
			d->nchars = d->nservices = 0;
			for_each_object(chars_cb, d);
			LOGI(TAG, "%s: %d services, %d characteristics", d->name, d->nservices, d->nchars);
			atomic_store(&d->connected, true);
			d->backoff_s = 0;
			d->st = ST_READY;
			d->last_poll = 0;
			for (int i = 0; i < 8 && d->spec.notify_uuids[i]; i++) {
				const char *p = char_path(d, d->spec.notify_uuids[i]);
				if (p) { call_void(p, "org.bluez.GattCharacteristic1", "StartNotify"); d->notified[i] = true; }
				else LOGW(TAG, "%s: characteristic %s not found", d->name, d->spec.notify_uuids[i]);
			}
			if (d->spec.on_connected) d->spec.on_connected(d, d->spec.ctx);
		} else if (!conn && now > d->next_action) {
			dev_disconnected(d, now);
		} else if (now > d->next_action + 15) {
			LOGW(TAG, "%s: services not resolved, reconnecting", d->name);
			call_void(d->path, "org.bluez.Device1", "Disconnect");
			dev_disconnected(d, now);
		}
		break;
	}
	case ST_READY: {
		bool conn = true;
		if (get_bool_prop(d->path, "org.bluez.Device1", "Connected", &conn) < 0 || !conn) { dev_disconnected(d, now); break; }
		int b = get_byte_prop(d->path, "org.bluez.Battery1", "Percentage");
		if (b >= 0) atomic_store(&d->battery, b);
		if (d->spec.poll_uuids[0] && d->spec.poll_ms > 0 && now - d->last_poll >= d->spec.poll_ms / 1000.0) {
			d->last_poll = now;
			for (int i = 0; i < 4 && d->spec.poll_uuids[i]; i++) {
				const char *p = char_path(d, d->spec.poll_uuids[i]);
				if (!p) continue;
				sd_bus_error err = SD_BUS_ERROR_NULL;
				sd_bus_message *rep = NULL;
				int r = sd_bus_call_method(g_bus, BLUEZ, p, "org.bluez.GattCharacteristic1", "ReadValue", &err, &rep, "a{sv}", 0);
				if (r >= 0) {
					const void *data; size_t len;
					if (sd_bus_message_read_array(rep, 'y', &data, &len) >= 0 && d->spec.on_value) d->spec.on_value(d, d->spec.poll_uuids[i], data, len, d->spec.ctx);
				} else LOGD(TAG, "ReadValue %s: %s", d->spec.poll_uuids[i], err.message ? err.message : "");
				sd_bus_error_free(&err);
				sd_bus_message_unref(rep);
			}
		}
		break;
	}
	default: break;
	}
}

/* ---------------- signals ---------------- */

static int on_props_changed(sd_bus_message *m, void *ud, sd_bus_error *ret)
{
	(void)ud; (void)ret;
	const char *iface = NULL;
	if (sd_bus_message_read(m, "s", &iface) < 0 || strcmp(iface, "org.bluez.GattCharacteristic1")) return 0;
	const char *path = sd_bus_message_get_path(m);
	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key;
		sd_bus_message_read(m, "s", &key);
		if (!strcmp(key, "Value")) {
			sd_bus_message_enter_container(m, 'v', "ay");
			const void *data; size_t len;
			if (sd_bus_message_read_array(m, 'y', &data, &len) >= 0) {
				pthread_mutex_lock(&g_mu);
				for (int i = 0; i < MAX_DEVS; i++) {
					pf_ble_dev *d = &g_devs[i];
					if (!d->used || d->st != ST_READY) continue;
					for (int c = 0; c < d->nchars; c++)
						if (!strcmp(d->chars[c].path, path) && d->spec.on_value) d->spec.on_value(d, d->chars[c].uuid, data, len, d->spec.ctx);
				}
				pthread_mutex_unlock(&g_mu);
			}
			sd_bus_message_exit_container(m);
		} else sd_bus_message_skip(m, "v");
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	return 0;
}

/* ---------------- scan for the UI ---------------- */

static void scan_cb(const char *path, const char *iface, sd_bus_message *props, void *ctx)
{
	(void)props;
	if (strcmp(iface, "org.bluez.Device1")) return;
	char addr[20] = "", name[64] = "";
	get_str_prop(path, iface, "Address", addr, sizeof addr);
	get_str_prop(path, iface, "Name", name, sizeof name);
	if (!name[0]) get_str_prop(path, iface, "Alias", name, sizeof name);
	int rssi = get_int16_prop(path, iface, "RSSI");
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "name", name);
	cJSON_AddStringToObject(o, "address", addr);
	cJSON_AddNumberToObject(o, "rssi", rssi);
	cJSON_AddItemToArray((cJSON *)ctx, o);
}

/* ---------------- manager thread ---------------- */

static bool find_adapter(void)
{
	bool powered = false;
	if (get_bool_prop(g_adapter, "org.bluez.Adapter1", "Powered", &powered) < 0) return false;
	if (!powered) {
		sd_bus_error err = SD_BUS_ERROR_NULL;
		if (sd_bus_set_property(g_bus, BLUEZ, g_adapter, "org.bluez.Adapter1", "Powered", &err, "b", 1) < 0)
			LOGW(TAG, "cannot power adapter: %s", err.message ? err.message : "?");
		sd_bus_error_free(&err);
	}
	return true;
}

static void process_writes(void)
{
	for (;;) {
		wreq_t w;
		pthread_mutex_lock(&g_mu);
		if (g_qlen == 0) { pthread_mutex_unlock(&g_mu); return; }
		w = g_q[g_qhead];
		g_qhead = (g_qhead + 1) % QLEN;
		g_qlen--;
		pthread_mutex_unlock(&g_mu);
		if (w.d->st != ST_READY) continue;
		const char *p = char_path(w.d, w.uuid);
		if (!p) { LOGW(TAG, "%s: write to unknown characteristic %s", w.d->name, w.uuid); continue; }
		sd_bus_error err = SD_BUS_ERROR_NULL;
		sd_bus_message *msg = NULL, *rep = NULL;
		sd_bus_message_new_method_call(g_bus, &msg, BLUEZ, p, "org.bluez.GattCharacteristic1", "WriteValue");
		sd_bus_message_append_array(msg, 'y', w.data, w.len);
		sd_bus_message_open_container(msg, 'a', "{sv}");
		sd_bus_message_append(msg, "{sv}", "type", "s", w.resp ? "request" : "command");
		sd_bus_message_close_container(msg);
		int r = sd_bus_call(g_bus, msg, 5 * 1000000ULL, &err, &rep);
		if (r < 0) LOGW(TAG, "%s: write %s failed: %s", w.d->name, w.uuid, err.message ? err.message : strerror(-r));
		sd_bus_error_free(&err);
		sd_bus_message_unref(msg);
		sd_bus_message_unref(rep);
	}
}

static void *manager(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "pf-ble");
	if (sd_bus_default_system(&g_bus) < 0) { LOGE(TAG, "cannot connect to the system bus"); return NULL; }
	sd_bus_match_signal(g_bus, NULL, BLUEZ, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged", on_props_changed, NULL);
	double last_step = 0;
	while (atomic_load(&g_run)) {
		int r = sd_bus_process(g_bus, NULL);
		if (r > 0) continue;
		if (r < 0) { LOGE(TAG, "bus error %d", r); break; }
		sd_bus_wait(g_bus, 200 * 1000ULL);
		double now = pf_now();
		if (now - last_step < 1.0) continue;
		last_step = now;
		if (!find_adapter()) { if (atomic_exchange(&g_avail, false)) LOGW(TAG, "bluetooth adapter unavailable"); continue; }
		if (!atomic_exchange(&g_avail, true)) LOGI(TAG, "bluetooth adapter ready");
		process_writes();
		pthread_mutex_lock(&g_mu);
		bool need_disc = false;
		for (int i = 0; i < MAX_DEVS; i++) {
			pf_ble_dev *d = &g_devs[i];
			if (d->want_release) {
				if (d->path[0] && (d->st == ST_READY || d->st == ST_CONNECTING)) call_void(d->path, "org.bluez.Device1", "Disconnect");
				d->used = false;
				d->want_release = false;
				continue;
			}
			if (!d->used) continue;
			dev_step(d, now);
			if (d->st == ST_IDLE) need_disc = true;
		}
		/* UI scan request: keep discovery on for the window, then snapshot the object tree */
		int scan_s = atomic_load(&g_scan_seconds);
		if (scan_s > 0) {
			if (g_scan_deadline == 0) { g_scan_deadline = now + scan_s; set_discovery(true); }
			if (now >= g_scan_deadline) {
				cJSON *arr = cJSON_CreateArray();
				for_each_object(scan_cb, arr);
				cJSON_Delete(g_scan_result);
				g_scan_result = arr;
				g_scan_gen++;
				g_scan_deadline = 0;
				atomic_store(&g_scan_seconds, 0);
				g_discover_until = now;
			}
			need_disc = true;
		}
		pthread_mutex_unlock(&g_mu);
		if (!need_disc && g_discovering && now > g_discover_until) set_discovery(false);
	}
	set_discovery(false);
	sd_bus_flush_close_unref(g_bus);
	g_bus = NULL;
	return NULL;
}

int pf_ble_start(void)
{
	if (atomic_load(&g_run)) return 0;
	atomic_store(&g_run, true);
	return pthread_create(&g_tid, NULL, manager, NULL);
}

void pf_ble_stop(void)
{
	if (!atomic_load(&g_run)) return;
	atomic_store(&g_run, false);
	pthread_join(g_tid, NULL);
}

bool pf_ble_available(void) { return atomic_load(&g_avail); }

pf_ble_dev *pf_ble_register(const pf_ble_spec *spec)
{
	pf_ble_start();
	pthread_mutex_lock(&g_mu);
	pf_ble_dev *d = NULL;
	for (int i = 0; i < MAX_DEVS; i++) if (!g_devs[i].used) { d = &g_devs[i]; break; }
	if (d) {
		memset(d, 0, sizeof *d);
		d->used = true;
		d->spec = *spec;
		d->st = ST_IDLE;
		atomic_store(&d->battery, -1);
	}
	pthread_mutex_unlock(&g_mu);
	return d;
}

void pf_ble_unregister(pf_ble_dev *d)
{
	if (!d) return;
	pthread_mutex_lock(&g_mu);
	d->want_release = true;   /* the manager thread disconnects and frees the slot */
	pthread_mutex_unlock(&g_mu);
}

int pf_ble_write(pf_ble_dev *d, const char *uuid, const uint8_t *data, size_t len, bool with_response)
{
	if (!d || len > 32) return -1;
	pthread_mutex_lock(&g_mu);
	int rc = -1;
	if (g_qlen < QLEN) {
		wreq_t *w = &g_q[(g_qhead + g_qlen) % QLEN];
		w->d = d;
		pf_strlcpy(w->uuid, uuid, sizeof w->uuid);
		memcpy(w->data, data, len);
		w->len = len;
		w->resp = with_response;
		g_qlen++;
		rc = 0;
	}
	pthread_mutex_unlock(&g_mu);
	return rc;
}

bool pf_ble_connected(const pf_ble_dev *d) { return d && atomic_load(&d->connected); }
int pf_ble_battery(const pf_ble_dev *d) { return d ? atomic_load(&d->battery) : -1; }
const char *pf_ble_address(const pf_ble_dev *d) { return d ? d->address : ""; }
const char *pf_ble_name(const pf_ble_dev *d) { return d ? d->name : ""; }

bool pf_ble_has_service(const pf_ble_dev *d, const char *uuid_prefix)
{
	if (!d) return false;
	for (int i = 0; i < d->nservices; i++) if (!strncasecmp(d->services[i], uuid_prefix, strlen(uuid_prefix))) return true;
	return false;
}

cJSON *pf_ble_scan_json(int seconds)
{
	if (!pf_ble_available()) return cJSON_CreateArray();
	if (seconds < 2) seconds = 2;
	if (seconds > 30) seconds = 30;
	pthread_mutex_lock(&g_mu);
	unsigned gen = g_scan_gen;
	pthread_mutex_unlock(&g_mu);
	atomic_store(&g_scan_seconds, seconds);
	double deadline = pf_now() + seconds + 5;
	for (;;) {
		pf_sleep_ms(200);
		pthread_mutex_lock(&g_mu);
		bool done = g_scan_gen != gen;
		cJSON *copy = done && g_scan_result ? cJSON_Duplicate(g_scan_result, 1) : NULL;
		pthread_mutex_unlock(&g_mu);
		if (done) return copy ? copy : cJSON_CreateArray();
		if (pf_now() > deadline) return cJSON_CreateArray();
	}
}

#else /* !PF_WITH_BLE */
int pf_ble_start(void) { return -1; }
void pf_ble_stop(void) {}
bool pf_ble_available(void) { return false; }
pf_ble_dev *pf_ble_register(const pf_ble_spec *spec) { (void)spec; LOGW(TAG, "Bluetooth support not built"); return NULL; }
void pf_ble_unregister(pf_ble_dev *d) { (void)d; }
int pf_ble_write(pf_ble_dev *d, const char *u, const uint8_t *p, size_t n, bool r) { (void)d; (void)u; (void)p; (void)n; (void)r; return -1; }
bool pf_ble_connected(const pf_ble_dev *d) { (void)d; return false; }
bool pf_ble_has_service(const pf_ble_dev *d, const char *u) { (void)d; (void)u; return false; }
int pf_ble_battery(const pf_ble_dev *d) { (void)d; return -1; }
const char *pf_ble_address(const pf_ble_dev *d) { (void)d; return ""; }
const char *pf_ble_name(const pf_ble_dev *d) { (void)d; return ""; }
cJSON *pf_ble_scan_json(int s) { (void)s; return cJSON_CreateArray(); }
const char *pf_ble_uuid16(const char *short4, char out[40]) { snprintf(out, 40, "0000%s-0000-1000-8000-00805f9b34fb", short4); return out; }
#endif
