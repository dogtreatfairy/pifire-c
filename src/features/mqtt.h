#pragma once
/* MQTT telemetry + Home Assistant discovery (libmosquitto). Topic layout matches the Python
 * PiFire: {id}/control, {id}/devices, {id}/probe_data_primary, {id}/probe_data_food, {id}/pid,
 * {id}/pellet, {id}/notify_event, {id}/system, {id}/availability. Commands on {id}/cmd. */
#include <stdbool.h>

void pf_mqtt_init(void);           /* reads settings.notify.mqtt; safe to call when disabled */
void pf_mqtt_tick(double now);     /* services thread ~1 Hz: reconnect, telemetry, config changes */
void pf_mqtt_shutdown(void);
bool pf_mqtt_connected(void);
