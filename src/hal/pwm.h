#pragma once
/* Hardware PWM via sysfs (/sys/class/pwm/pwmchipN/pwmM). */
#include <stdbool.h>

typedef struct pf_pwm pf_pwm;

pf_pwm *pf_pwm_open(const char *chip_path, int channel);  /* exports the channel (EBUSY tolerated) */
int  pf_pwm_config(pf_pwm *p, int hz, double duty_pct);   /* duty_pct 0..100 = fraction of period HIGH */
int  pf_pwm_enable(pf_pwm *p, bool on);
void pf_pwm_close(pf_pwm *p);
