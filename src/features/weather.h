#pragma once
/* Local weather for the grill's location (settings.weather: country + postal code). The postal code
 * is geocoded once (zippopotam.us, no key) and the current conditions come from Open-Meteo (no key)
 * every 15 minutes. The outdoor temperature is the ambient reference for the feed-forward model and
 * the learning observations when no ambient probe exists; wind and humidity are recorded with them. */
#include <cJSON.h>
#include <stdbool.h>

typedef struct {
	bool valid;
	double temp_c, wind_kmh, gust_kmh, humidity_pct;
	double ts;          /* wall time of the observation */
	char place[64];
} pf_weather;

void pf_weather_init(bool sim);
void pf_weather_shutdown(void);
/* Latest conditions (valid=false when disabled, unconfigured or older than 2 h). */
void pf_weather_get(pf_weather *out);
/* Ask the worker to fetch now (after the postal code changed). */
void pf_weather_refresh(void);
/* {enabled, place, temp, wind_kmh, gust_kmh, humidity, age_s, valid, error} for the UI */
cJSON *pf_weather_json(void);
