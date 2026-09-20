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
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

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
	atomic_int rssi;         /* dBm, 0 = unknown */
	double last_rssi_t;
	bool notified[8];
	uint8_t mfr_last[32]; size_t mfr_last_len; double last_seen;   /* passive devices */
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
/* ManufacturerData is a{qv} (company id -> ay). Returns the payload length for `id`, 0 if absent, <0 on error. */
static int get_mfr_data(const char *path, uint16_t id, uint8_t *buf, size_t cap)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *rep = NULL;
	int r = sd_bus_get_property(g_bus, BLUEZ, path, "org.bluez.Device1", "ManufacturerData", &err, &rep, "a{qv}");
	if (r < 0) { sd_bus_error_free(&err); return -1; }
	int found = 0;
	if (sd_bus_message_enter_container(rep, 'a', "{qv}") >= 0) {
		while (sd_bus_message_enter_container(rep, 'e', "qv") > 0) {
			uint16_t key = 0;
			sd_bus_message_read(rep, "q", &key);
			if (key == id && sd_bus_message_enter_container(rep, 'v', "ay") >= 0) {
				const void *data; size_t len;
				if (sd_bus_message_read_array(rep, 'y', &data, &len) >= 0) { if (len > cap) len = cap; memcpy(buf, data, len); found = (int)len; }
				sd_bus_message_exit_container(rep);
			} else sd_bus_message_skip(rep, "v");
			sd_bus_message_exit_container(rep);
		}
		sd_bus_message_exit_container(rep);
	}
	sd_bus_message_unref(rep);
	return found;
}

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
	bool ok = spec_matches(&m->d->spec, addr, name);
	if (!ok && m->d->spec.passive && m->d->spec.manufacturer_id && !(m->d->spec.address && *m->d->spec.address)) {
		uint8_t tmp[32];
		ok = get_mfr_data(path, m->d->spec.manufacturer_id, tmp, sizeof tmp) > 0;
	}
	if (ok) {
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
		/* DuplicateData: BlueZ must keep emitting ManufacturerData updates for devices it already knows,
		 * otherwise a passive probe's payload freezes after its first advertisement */
		sd_bus_call_method(g_bus, BLUEZ, g_adapter, "org.bluez.Adapter1", "SetDiscoveryFilter", &err, &rep, "a{sv}", 2, "Transport", "s", "le", "DuplicateData", "b", 1);
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

/* ---------------- link quality ----------------
 * BlueZ only refreshes Device1.RSSI from advertisements, so a connected (GATT) probe would show the
 * value from before it connected. Read_RSSI over a raw HCI socket gives the live value for the
 * connection; it needs CAP_NET_RAW (granted in pifired.service) and is skipped quietly without it.
 * The few kernel structures needed are declared here so libbluetooth is not a build dependency. */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define HCI_BTPROTO 1
#define HCI_SOL 0
#define HCI_FILTER_OPT 2
#define HCI_CMD_PKT 0x01
#define HCI_EVT_PKT 0x04
#define HCI_EVT_CMD_COMPLETE 0x0E
#define HCI_LE_LINK 0x80
#define HCI_OP_READ_RSSI 0x1405   /* OGF 0x05 (status) << 10 | OCF 0x0005 */
#define HCI_IOC_GETCONNINFO _IOR('H', 213, int)
struct hci_sockaddr { sa_family_t family; unsigned short dev; unsigned short channel; };
struct hci_bdaddr { uint8_t b[6]; } __attribute__((packed));
struct hci_conninfo { uint16_t handle; struct hci_bdaddr bdaddr; uint8_t type, out; uint16_t state; uint32_t link_mode; };
struct hci_conninfo_req { struct hci_bdaddr bdaddr; uint8_t type; uint8_t pad; struct hci_conninfo ci[1]; };
struct hci_filter_opt { uint32_t type_mask; uint32_t event_mask[2]; uint16_t opcode; };

static bool g_hci_denied;   /* no CAP_NET_RAW: do not retry every poll */

static int hci_read_rssi(const char *addr_str)
{
	if (g_hci_denied) return 0;
	unsigned b[6];
	if (sscanf(addr_str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return 0;
	int dd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, HCI_BTPROTO);
	if (dd < 0) { if (errno == EPERM || errno == EACCES) { g_hci_denied = true; LOGI(TAG, "no raw HCI access (CAP_NET_RAW): live RSSI for connected probes disabled"); } return 0; }
	int devno = 0;
	const char *h = strstr(g_adapter, "hci");
	if (h) devno = atoi(h + 3);
	struct hci_sockaddr sa = { .family = AF_BLUETOOTH, .dev = (unsigned short)devno, .channel = 0 };
	int rssi = 0;
	if (bind(dd, (struct sockaddr *)&sa, sizeof sa) < 0) { if (errno == EPERM || errno == EACCES) g_hci_denied = true; close(dd); return 0; }
	struct hci_conninfo_req req;
	memset(&req, 0, sizeof req);
	for (int i = 0; i < 6; i++) req.bdaddr.b[i] = (uint8_t)b[5 - i];   /* bdaddr_t is little-endian */
	req.type = HCI_LE_LINK;
	if (ioctl(dd, HCI_IOC_GETCONNINFO, &req) < 0) { close(dd); return 0; }
	uint16_t handle = req.ci[0].handle;
	struct hci_filter_opt flt = { .type_mask = 1u << HCI_EVT_PKT, .event_mask = { 1u << HCI_EVT_CMD_COMPLETE, 0 }, .opcode = HCI_OP_READ_RSSI };
	if (setsockopt(dd, HCI_SOL, HCI_FILTER_OPT, &flt, sizeof flt) < 0) { close(dd); return 0; }
	uint8_t pkt[6] = { HCI_CMD_PKT, HCI_OP_READ_RSSI & 0xFF, HCI_OP_READ_RSSI >> 8, 2, (uint8_t)(handle & 0xFF), (uint8_t)(handle >> 8) };
	if (write(dd, pkt, sizeof pkt) != (ssize_t)sizeof pkt) { close(dd); return 0; }
	struct pollfd pfd = { .fd = dd, .events = POLLIN };
	for (int tries = 0; tries < 4; tries++) {
		if (poll(&pfd, 1, 300) <= 0) break;
		uint8_t ev[64];
		ssize_t n = read(dd, ev, sizeof ev);
		/* 04 0E len ncmd opcode(2) status handle(2) rssi */
		if (n >= 10 && ev[0] == HCI_EVT_PKT && ev[1] == HCI_EVT_CMD_COMPLETE && ev[4] == (HCI_OP_READ_RSSI & 0xFF) && ev[5] == (HCI_OP_READ_RSSI >> 8)) {
			if (ev[6] == 0) rssi = (int8_t)ev[9];
			break;
		}
	}
	close(dd);
	return rssi;
}

/* refresh d->rssi: advertisement RSSI from BlueZ when it has one, else the live connection value */
static void refresh_rssi(pf_ble_dev *d, double now, bool connected_link)
{
	if (now - d->last_rssi_t < 5) return;
	d->last_rssi_t = now;
	int r = 0;
	if (connected_link) r = hci_read_rssi(d->address);
	if (r == 0 && d->path[0]) r = get_int16_prop(d->path, "org.bluez.Device1", "RSSI");
	if (r != 0) atomic_store(&d->rssi, r);
}

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

/* passive device: no connection; read the advertisement payload while discovery keeps it fresh */
static double g_passive_restart_t;

static void passive_step(pf_ble_dev *d, double now)
{
	if (!g_discovering) { set_discovery(true); g_discover_until = now + 3600; }
	/* advertisements stopped arriving for everyone although discovery is "on": BlueZ occasionally goes
	 * quiet after an adapter hiccup; a stop/start once a minute brings it back */
	if (d->st == ST_READY && d->path[0] && d->last_seen > 0 && now - d->last_seen > 20 && now - g_passive_restart_t > 60) {
		g_passive_restart_t = now;
		LOGI(TAG, "no advertisements from %s for 20 s: restarting discovery", d->address);
		set_discovery(false);
		set_discovery(true);
	}
	if (!d->path[0]) {
		match_ctx m = { d, false };
		for_each_object(match_cb, &m);
		if (m.found) { d->st = ST_READY; LOGI(TAG, "found %s (%s), listening to advertisements", d->name, d->address); }
		return;
	}
	double poll = d->spec.poll_ms > 0 ? d->spec.poll_ms / 1000.0 : 1.0;
	if (now - d->last_poll < poll) return;
	d->last_poll = now;
	refresh_rssi(d, now, false);
	uint8_t buf[32];
	int n = get_mfr_data(d->path, d->spec.manufacturer_id, buf, sizeof buf);
	if (n < 0) { d->path[0] = 0; d->st = ST_IDLE; atomic_store(&d->connected, false); return; }   /* object went away */
	if (n > 0 && (n != (int)d->mfr_last_len || memcmp(buf, d->mfr_last, (size_t)n))) {
		memcpy(d->mfr_last, buf, (size_t)n); d->mfr_last_len = (size_t)n;
		d->last_seen = now;
		if (!atomic_exchange(&d->connected, true)) { if (d->spec.on_connected) d->spec.on_connected(d, d->spec.ctx); }
		if (d->spec.on_value) d->spec.on_value(d, "mfr", buf, (size_t)n, d->spec.ctx);
	} else if (atomic_load(&d->connected) && now - d->last_seen > 60) {
		atomic_store(&d->connected, false);
		if (d->spec.on_disconnected) d->spec.on_disconnected(d, d->spec.ctx);
	}
}

static void dev_step(pf_ble_dev *d, double now)
{
	if (d->spec.passive) { passive_step(d, now); return; }
	switch (d->st) {
	case ST_IDLE: {
		match_ctx m = { d, false };
		for_each_object(match_cb, &m);
		if (m.found) { d->st = ST_FOUND; d->last_rssi_t = 0; refresh_rssi(d, now, false); LOGI(TAG, "found %s (%s)", d->name, d->address); }
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
		refresh_rssi(d, now, true);
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

/* manufacturer ids present in a device's advertisement */
static int get_mfr_ids(const char *path, uint16_t *ids, int max)
{
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *rep = NULL;
	if (sd_bus_get_property(g_bus, BLUEZ, path, "org.bluez.Device1", "ManufacturerData", &err, &rep, "a{qv}") < 0) { sd_bus_error_free(&err); return 0; }
	int n = 0;
	if (sd_bus_message_enter_container(rep, 'a', "{qv}") >= 0) {
		while (sd_bus_message_enter_container(rep, 'e', "qv") > 0) {
			uint16_t key = 0;
			sd_bus_message_read(rep, "q", &key);
			if (n < max) ids[n++] = key;
			sd_bus_message_skip(rep, "v");
			sd_bus_message_exit_container(rep);
		}
		sd_bus_message_exit_container(rep);
	}
	sd_bus_message_unref(rep);
	return n;
}

/* advertised service UUIDs, joined for matching */
static void get_uuids(const char *path, char *out, size_t n)
{
	out[0] = 0;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *rep = NULL;
	if (sd_bus_get_property(g_bus, BLUEZ, path, "org.bluez.Device1", "UUIDs", &err, &rep, "as") < 0) { sd_bus_error_free(&err); return; }
	size_t used = 0;
	if (sd_bus_message_enter_container(rep, 'a', "s") >= 0) {
		const char *u;
		while (sd_bus_message_read(rep, "s", &u) > 0) { int w = snprintf(out + used, n - used, "%s%s", used ? "," : "", u); if (w > 0 && used + (size_t)w < n) used += (size_t)w; }
		sd_bus_message_exit_container(rep);
	}
	sd_bus_message_unref(rep);
}

/* what kind of probe this looks like: chefiq | ibbq | meater | "" */
static const char *classify(const char *name, const char *uuids, const uint16_t *ids, int nids)
{
	for (int i = 0; i < nids; i++) if (ids[i] == 0x05CD) return "chefiq";
	if (strcasestr(name, "meater") || strcasestr(uuids, "a75cc7fc") || strcasestr(uuids, "c9e2746c")) return "meater";
	if (strcasestr(name, "ibbq") || strcasestr(name, "xbbq") || strcasestr(name, "tbbq") || strcasestr(name, "inkbird") || strcasestr(uuids, "0000fff0-")) return "ibbq";
	if (strncasecmp(name, "CQ", 2) == 0 && strlen(name) <= 6) return "chefiq";
	return "";
}

static void scan_cb(const char *path, const char *iface, sd_bus_message *props, void *ctx)
{
	(void)props;
	if (strcmp(iface, "org.bluez.Device1")) return;
	char addr[20] = "", name[64] = "", uuids[512];
	get_str_prop(path, iface, "Address", addr, sizeof addr);
	get_str_prop(path, iface, "Name", name, sizeof name);
	if (!name[0]) get_str_prop(path, iface, "Alias", name, sizeof name);
	int rssi = get_int16_prop(path, iface, "RSSI");
	uint16_t ids[8];
	int nids = get_mfr_ids(path, ids, 8);
	get_uuids(path, uuids, sizeof uuids);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "name", name);
	cJSON_AddStringToObject(o, "address", addr);
	cJSON_AddNumberToObject(o, "rssi", rssi);
	cJSON_AddStringToObject(o, "kind", classify(name, uuids, ids, nids));
	cJSON *m = cJSON_AddArrayToObject(o, "manufacturer_ids");
	for (int i = 0; i < nids; i++) cJSON_AddItemToArray(m, cJSON_CreateNumber(ids[i]));
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
				if (!d->spec.passive && d->path[0] && (d->st == ST_READY || d->st == ST_CONNECTING)) call_void(d->path, "org.bluez.Device1", "Disconnect");
				d->used = false;
				d->want_release = false;
				continue;
			}
			if (!d->used) continue;
			dev_step(d, now);
			if (d->st == ST_IDLE || d->spec.passive) need_disc = true;
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
		atomic_store(&d->rssi, 0);
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
int pf_ble_rssi(const pf_ble_dev *d) { return d ? atomic_load(&d->rssi) : 0; }
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
int pf_ble_rssi(const pf_ble_dev *d) { (void)d; return 0; }
const char *pf_ble_address(const pf_ble_dev *d) { (void)d; return ""; }
const char *pf_ble_name(const pf_ble_dev *d) { (void)d; return ""; }
cJSON *pf_ble_scan_json(int s) { (void)s; return cJSON_CreateArray(); }
const char *pf_ble_uuid16(const char *short4, char out[40]) { snprintf(out, 40, "0000%s-0000-1000-8000-00805f9b34fb", short4); return out; }
#endif
