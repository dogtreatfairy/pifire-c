#define _GNU_SOURCE
#include "features/mqtt.h"
#include "core/events.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/status.h"
#include "core/util.h"
#include "features/pellets.h"
#include "net/sysinfo.h"
#include "web/api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "mqtt"

#if PF_WITH_MQTT
#include <mosquitto.h>
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
	bool enabled;
	char broker[128], user[64], pass[64], id[48], ha_topic[64];
	int port, update_sec;
} cfg_t;

static struct mosquitto *g_m;
static cfg_t g_cfg;
static unsigned g_cfg_gen;
static atomic_bool g_connected;
static bool g_discovered;
static double g_last_pub;
static int g_last_mode = -1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void load_cfg(cfg_t *c)
{
	c->enabled = pf_set_bool("notify.mqtt.enabled", false);
	pf_set_str("notify.mqtt.broker", c->broker, sizeof c->broker, "homeassistant.local");
	pf_set_str("notify.mqtt.username", c->user, sizeof c->user, "");
	pf_set_str("notify.mqtt.password", c->pass, sizeof c->pass, "");
	pf_set_str("notify.mqtt.id", c->id, sizeof c->id, "PiFire");
	pf_set_str("notify.mqtt.homeassistant_autodiscovery_topic", c->ha_topic, sizeof c->ha_topic, "homeassistant");
	c->port = pf_set_int("notify.mqtt.port", 1883);
	c->update_sec = pf_set_int("notify.mqtt.update_sec", 30);
	if (c->update_sec < 5) c->update_sec = 5;
}

static void pub(const char *topic_suffix, const char *payload, bool retain)
{
	if (!g_m) return;
	char topic[160];
	snprintf(topic, sizeof topic, "%s/%s", g_cfg.id, topic_suffix);
	mosquitto_publish(g_m, NULL, topic, (int)strlen(payload), payload, 0, retain);
}

static void pub_json(const char *topic_suffix, cJSON *j)
{
	char *txt = cJSON_PrintUnformatted(j);
	if (txt) { pub(topic_suffix, txt, false); free(txt); }
	cJSON_Delete(j);
}

/* ---- Home Assistant discovery ---- */
static void discover_one(const char *component, const char *context, const char *field, const char *name,
                         const char *device_class, const char *unit, bool binary)
{
	char obj[96], topic[256], grill[64];
	pf_set_str("globals.grill_name", grill, sizeof grill, "");
	snprintf(obj, sizeof obj, "%s_%s_%s", g_cfg.id, context, field);
	for (char *p = obj; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
	snprintf(topic, sizeof topic, "%s/%s/%s/%s_%s/config", g_cfg.ha_topic, component, g_cfg.id, context, field);
	cJSON *j = cJSON_CreateObject();
	cJSON_AddStringToObject(j, "name", name);
	cJSON_AddStringToObject(j, "object_id", obj);
	cJSON_AddStringToObject(j, "unique_id", obj);
	char st[160];
	snprintf(st, sizeof st, "%s/%s", g_cfg.id, context);
	cJSON_AddStringToObject(j, "state_topic", st);
	char vt[96];
	snprintf(vt, sizeof vt, "{{ value_json.%s }}", field);
	cJSON_AddStringToObject(j, "value_template", vt);
	if (binary) { cJSON_AddBoolToObject(j, "payload_on", true); cJSON_AddBoolToObject(j, "payload_off", false); }
	else if (unit && *unit) { cJSON_AddStringToObject(j, "unit_of_measurement", unit); cJSON_AddStringToObject(j, "state_class", "measurement"); }
	if (device_class && *device_class) cJSON_AddStringToObject(j, "device_class", device_class);
	cJSON *av = cJSON_AddArrayToObject(j, "availability");
	cJSON *a = cJSON_CreateObject();
	char at[160];
	snprintf(at, sizeof at, "%s/availability", g_cfg.id);
	cJSON_AddStringToObject(a, "topic", at);
	cJSON_AddItemToArray(av, a);
	cJSON *dev = cJSON_AddObjectToObject(j, "device");
	cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
	cJSON_AddItemToArray(ids, cJSON_CreateString(g_cfg.id));
	cJSON_AddStringToObject(dev, "manufacturer", "PiFire");
	cJSON_AddStringToObject(dev, "model", "PiFire (C)");
	cJSON_AddStringToObject(dev, "name", grill[0] ? grill : g_cfg.id);
	cJSON_AddStringToObject(dev, "sw_version", PF_VERSION);
	char *txt = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	if (txt) { mosquitto_publish(g_m, NULL, topic, (int)strlen(txt), txt, 2, true); free(txt); }
}

static void discover(const pf_status *s)
{
	const char *u = pf_settings_units() == PF_UNITS_C ? "°C" : "°F";
	discover_one("sensor", "control", "mode", "Mode", "", "", false);
	discover_one("sensor", "control", "primary_setpoint", "Set point", "temperature", u, false);
	discover_one("binary_sensor", "control", "s_plus", "Smoke Plus", "", "", true);
	for (int i = 0; i < s->sensors.n; i++) {
		const pf_probe_reading *p = &s->sensors.p[i];
		if (!p->enabled) continue;
		discover_one("sensor", p->role == PF_PROBE_PRIMARY ? "probe_data_primary" : "probe_data_food", p->label, p->name, "temperature", u, false);
	}
	const char *outs[] = { "auger", "igniter", "power", "fan" };
	for (int i = 0; i < 4; i++) { char n[32]; snprintf(n, sizeof n, "%c%s", outs[i][0] - 32, outs[i] + 1); discover_one("binary_sensor", "devices", outs[i], n, "running", "", true); }
	discover_one("sensor", "pid", "cycle_ratio", "Feed ratio", "", "%", false);
	discover_one("sensor", "pellet", "hopper_level", "Hopper level", "", "%", false);
	discover_one("sensor", "system", "cpu_temp", "CPU temperature", "temperature", "°C", false);
	g_discovered = true;
	LOGI(TAG, "Home Assistant discovery published");
}

/* ---- callbacks ---- */
static void on_connect(struct mosquitto *m, void *ud, int rc)
{
	(void)ud;
	if (rc != 0) { LOGW(TAG, "connect refused (%d)", rc); return; }
	atomic_store(&g_connected, true);
	g_discovered = false;
	g_last_pub = 0;
	char t[160];
	snprintf(t, sizeof t, "%s/availability", g_cfg.id);
	mosquitto_publish(m, NULL, t, 6, "online", 1, true);
	snprintf(t, sizeof t, "%s/cmd", g_cfg.id);
	mosquitto_subscribe(m, NULL, t, 0);
	LOGI(TAG, "connected to %s:%d as %s", g_cfg.broker, g_cfg.port, g_cfg.id);
}
static void on_disconnect(struct mosquitto *m, void *ud, int rc) { (void)m; (void)ud; atomic_store(&g_connected, false); if (rc) LOGW(TAG, "disconnected (%d), will retry", rc); }
static void on_message(struct mosquitto *m, void *ud, const struct mosquitto_message *msg)
{
	(void)m; (void)ud;
	if (!msg->payload || msg->payloadlen <= 0 || msg->payloadlen > 2048) return;
	char body[2049];
	memcpy(body, msg->payload, (size_t)msg->payloadlen);
	body[msg->payloadlen] = 0;
	char err[128];
	if (pf_api_command_json(body, err, sizeof err)) LOGW(TAG, "bad command on %s: %s", msg->topic, err);
}

static void event_sink(const pf_event *e, void *ctx)
{
	(void)ctx;
	const char *code = e->code, *title = e->title, *body = e->body;
	if (!(e->sinks & PF_SINK_MQTT) || !atomic_load(&g_connected)) return;
	cJSON *j = cJSON_CreateObject();
	char msg[400];
	snprintf(msg, sizeof msg, "%s: %s", title, body);
	cJSON_AddStringToObject(j, "msg", msg);
	cJSON_AddStringToObject(j, "code", code);
	pthread_mutex_lock(&g_mu);
	pub_json("notify_event", j);
	pthread_mutex_unlock(&g_mu);
}

static void start(void)
{
	load_cfg(&g_cfg);
	g_cfg_gen = pf_settings_generation();
	if (!g_cfg.enabled) return;
	mosquitto_lib_init();
	g_m = mosquitto_new(g_cfg.id, true, NULL);
	if (!g_m) { LOGE(TAG, "mosquitto_new failed"); return; }
	if (g_cfg.user[0]) mosquitto_username_pw_set(g_m, g_cfg.user, g_cfg.pass);
	char t[160];
	snprintf(t, sizeof t, "%s/availability", g_cfg.id);
	mosquitto_will_set(g_m, t, 7, "offline", 1, true);
	mosquitto_connect_callback_set(g_m, on_connect);
	mosquitto_disconnect_callback_set(g_m, on_disconnect);
	mosquitto_message_callback_set(g_m, on_message);
	mosquitto_reconnect_delay_set(g_m, 5, 60, true);
	int rc = mosquitto_connect_async(g_m, g_cfg.broker, g_cfg.port, 60);
	if (rc != MOSQ_ERR_SUCCESS) LOGW(TAG, "connect to %s:%d: %s (will retry)", g_cfg.broker, g_cfg.port, mosquitto_strerror(rc));
	mosquitto_loop_start(g_m);
	LOGI(TAG, "client started for %s:%d", g_cfg.broker, g_cfg.port);
}

static void stop(void)
{
	if (!g_m) return;
	char t[160];
	snprintf(t, sizeof t, "%s/availability", g_cfg.id);
	if (atomic_load(&g_connected)) mosquitto_publish(g_m, NULL, t, 7, "offline", 1, true);
	mosquitto_disconnect(g_m);
	mosquitto_loop_stop(g_m, false);
	mosquitto_destroy(g_m);
	g_m = NULL;
	atomic_store(&g_connected, false);
}

void pf_mqtt_init(void)
{
	pf_events_add_sink(event_sink, NULL);
	start();
}

void pf_mqtt_shutdown(void) { stop(); mosquitto_lib_cleanup(); }
bool pf_mqtt_connected(void) { return atomic_load(&g_connected); }

static void publish_telemetry(const pf_status *s)
{
	pf_units u = pf_settings_units();
	cJSON *c = cJSON_CreateObject();
	cJSON_AddStringToObject(c, "mode", pf_mode_name(s->mode));
	cJSON_AddStringToObject(c, "next_mode", pf_mode_name(s->next_mode));
	cJSON_AddBoolToObject(c, "s_plus", s->s_plus);
	cJSON_AddBoolToObject(c, "pwm_control", s->pwm_control);
	cJSON_AddNumberToObject(c, "duty_cycle", s->duty_cycle);
	cJSON_AddStringToObject(c, "status", s->mode == PF_MODE_STOP ? "inactive" : "active");
	cJSON_AddNumberToObject(c, "primary_setpoint", s->mode == PF_MODE_HOLD ? (double)(int)(pf_from_c(s->setpoint_c, u) + 0.5) : 0);
	pub_json("control", c);

	cJSON *d = cJSON_CreateObject();
	cJSON_AddBoolToObject(d, "auger", (s->outputs >> PF_OUT_AUGER) & 1);
	cJSON_AddBoolToObject(d, "igniter", (s->outputs >> PF_OUT_IGNITER) & 1);
	cJSON_AddBoolToObject(d, "power", (s->outputs >> PF_OUT_POWER) & 1);
	cJSON_AddBoolToObject(d, "fan", (s->outputs >> PF_OUT_FAN) & 1);
	pub_json("devices", d);

	cJSON *pp = cJSON_CreateObject(), *pf = cJSON_CreateObject();
	for (int i = 0; i < s->sensors.n; i++) {
		const pf_probe_reading *p = &s->sensors.p[i];
		if (!p->enabled) continue;
		double v = p->valid ? (double)(int)(pf_from_c(p->temp_c, u) + 0.5) : 0;
		cJSON_AddNumberToObject(p->role == PF_PROBE_PRIMARY ? pp : pf, p->label, v);
	}
	pub_json("probe_data_primary", pp);
	pub_json("probe_data_food", pf);

	cJSON *pid = cJSON_CreateObject();
	cJSON_AddNumberToObject(pid, "p", s->ctrl_dbg.p);
	cJSON_AddNumberToObject(pid, "i", s->ctrl_dbg.i);
	cJSON_AddNumberToObject(pid, "d", s->ctrl_dbg.d);
	cJSON_AddNumberToObject(pid, "u", s->u_raw);
	cJSON_AddNumberToObject(pid, "error", pf_delta_from_c(s->ctrl_dbg.error, u));
	cJSON_AddNumberToObject(pid, "cycle_ratio", (double)(int)(s->u_applied * 100 + 0.5));
	pub_json("pid", pid);

	cJSON *pel = cJSON_CreateObject();
	cJSON_AddNumberToObject(pel, "hopper_level", s->hopper_pct);
	pub_json("pellet", pel);

	cJSON *sys = pf_sysinfo_json();
	cJSON *sy = cJSON_CreateObject();
	cJSON_AddNumberToObject(sy, "cpu_temp", pf_json_num(sys, "cpu_temp_c", 0));
	cJSON_AddNumberToObject(sy, "available_memory", pf_json_num(sys, "mem_available", 0));
	cJSON_AddNumberToObject(sy, "cpu", pf_json_num(sys, "load1", 0) * 100);
	cJSON_Delete(sys);
	pub_json("system", sy);
}

void pf_mqtt_tick(double now)
{
	if (pf_settings_generation() != g_cfg_gen) {
		cfg_t fresh;
		load_cfg(&fresh);
		g_cfg_gen = pf_settings_generation();
		if (memcmp(&fresh, &g_cfg, sizeof fresh)) { LOGI(TAG, "configuration changed, restarting client"); stop(); start(); }
	}
	if (!g_m || !atomic_load(&g_connected)) return;
	pf_status s;
	pf_status_get(&s);
	bool mode_changed = (int)s.mode != g_last_mode;
	if (!mode_changed && now - g_last_pub < g_cfg.update_sec) return;
	g_last_pub = now;
	g_last_mode = (int)s.mode;
	pthread_mutex_lock(&g_mu);
	if (!g_discovered) discover(&s);
	publish_telemetry(&s);
	if (mode_changed) {
		cJSON *j = cJSON_CreateObject();
		char msg[64];
		snprintf(msg, sizeof msg, "Entered %s mode", pf_mode_name(s.mode));
		cJSON_AddStringToObject(j, "msg", msg);
		pub_json("notify_event", j);
	}
	pthread_mutex_unlock(&g_mu);
}

#else
void pf_mqtt_init(void) { if (pf_set_bool("notify.mqtt.enabled", false)) LOGW(TAG, "MQTT requested but this build has no MQTT support"); }
void pf_mqtt_tick(double now) { (void)now; }
void pf_mqtt_shutdown(void) {}
bool pf_mqtt_connected(void) { return false; }
#endif
