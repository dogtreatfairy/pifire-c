/* HC-SR04 ultrasonic distance sensor: 10 µs trigger pulse, echo pulse width via kernel-timestamped
 * GPIO edge events (no busy-waiting, ~µs accuracy). Median of 5 readings. */
#define _GNU_SOURCE
#include "core/settings.h"
#include "core/util.h"
#include "distance/registry.h"
#include "hal/gpio.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const pf_env *env; int chipfd; pf_gpio_line *trig, *echo; } hc_t;

static void *create(const char *cfg_json, const pf_env *env)
{
	cJSON *c = cJSON_Parse(cfg_json);
	int trig = pf_json_int(c, "trig", 23), echo = pf_json_int(c, "echo", 27);
	const char *chip = pf_json_str(c, "gpiochip", "/dev/gpiochip0");
	hc_t *h = calloc(1, sizeof *h);
	if (!h) return NULL;
	h->env = env;
	h->chipfd = pf_gpio_open_chip(chip);
	cJSON_Delete(c);
	if (h->chipfd < 0) { free(h); return NULL; }
	h->trig = pf_gpio_request_output(h->chipfd, (unsigned)trig, false, false, "pifire-hcsr04-trig");
	h->echo = pf_gpio_request_events(h->chipfd, (unsigned)echo, false, PF_GPIO_BIAS_NONE, "pifire-hcsr04-echo");
	if (!h->trig || !h->echo) { env->log(PF_LVL_ERROR, "hcsr04", "cannot claim GPIO %d/%d", trig, echo); pf_gpio_release(h->trig); pf_gpio_release(h->echo); pf_gpio_close_chip(h->chipfd); free(h); return NULL; }
	env->log(PF_LVL_INFO, "hcsr04", "trigger GPIO%d, echo GPIO%d", trig, echo);
	return h;
}

static void destroy(void *self)
{
	hc_t *h = self;
	pf_gpio_release(h->trig);
	pf_gpio_release(h->echo);
	pf_gpio_close_chip(h->chipfd);
	free(h);
}

static double one_reading(hc_t *h)
{
	/* drain stale events */
	uint64_t ts;
	while (pf_gpio_wait_edge(h->echo, 0, &ts) >= 0) {}
	pf_gpio_set(h->trig, true);
	struct timespec d = { 0, 10000 };
	nanosleep(&d, NULL);
	pf_gpio_set(h->trig, false);
	uint64_t t0 = 0, t1 = 0;
	int e = pf_gpio_wait_edge(h->echo, 40, &t0);
	if (e != 1) return -1;
	e = pf_gpio_wait_edge(h->echo, 40, &t1);
	if (e != 0 || t1 <= t0) return -1;
	double us = (double)(t1 - t0) / 1000.0;
	if (us > 38000) return -1; /* no echo */
	return us * 0.0343 / 2.0;
}

static double read_cm(void *self)
{
	hc_t *h = self;
	double v[5];
	int n = 0;
	for (int i = 0; i < 5; i++) {
		double r = one_reading(h);
		if (r > 0) v[n++] = r;
		pf_sleep_ms(60);
	}
	if (n < 3) return -1;
	for (int i = 1; i < n; i++) for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
	return v[n / 2];
}

static const pf_distance_ops ops = { .abi = PF_DISTANCE_ABI, .id = "hcsr04", .name = "HC-SR04 ultrasonic", .create = create, .destroy = destroy, .read_cm = read_cm };
const pf_distance_ops *pf_distance_hcsr04(void) { return &ops; }
