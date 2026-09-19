#pragma once
/* Output arbiter: the single gate through which relays and the fan are driven.
 * Once the safe latch is set (watchdog / fatal error), every write is refused until restart. */
#include "pifire/platform.h"

int  pf_outputs_init(const pf_platform_ops *ops, void *inst);
void pf_outputs_shutdown(void);

int  pf_outputs_set(pf_output o, bool on);
int  pf_outputs_fan_pct(int pct);
int  pf_outputs_pwm_frequency(int hz);
void pf_outputs_all_off(void);
bool pf_outputs_get(pf_output o);
int  pf_outputs_get_fan_pct(void);
unsigned pf_outputs_mask(void);
bool pf_outputs_read_input(pf_input in);
int  pf_outputs_platform_status(char *out, size_t n);

/* Watchdog path: latch safe, try to grab the HAL for `timeout_ms`, force all off. Returns 0 if the
 * outputs were actually driven off, <0 if the lock could not be acquired. */
int  pf_outputs_emergency_off(int timeout_ms);
bool pf_outputs_latched(void);
