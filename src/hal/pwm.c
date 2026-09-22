#define _GNU_SOURCE
#include "hal/pwm.h"
#include "core/log.h"
#include "core/util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "pwm"

struct pf_pwm {
	char dir[256];
	long period_ns;
};

static int write_attr(const char *dir, const char *attr, const char *val)
{
	char path[320];
	snprintf(path, sizeof path, "%s/%s", dir, attr);
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return -errno;
	ssize_t n = write(fd, val, strlen(val));
	int err = n < 0 ? errno : 0;
	close(fd);
	return err ? -err : 0;
}

pf_pwm *pf_pwm_open(const char *chip_path, int channel)
{
	char val[16];
	snprintf(val, sizeof val, "%d", channel);
	int rc = write_attr(chip_path, "export", val);
	if (rc && rc != -EBUSY) {
		LOGE(TAG, "export %s/pwm%d: %s", chip_path, channel, strerror(-rc));
		return NULL;
	}
	pf_pwm *p = calloc(1, sizeof *p);
	if (!p) return NULL;
	snprintf(p->dir, sizeof p->dir, "%s/pwm%d", chip_path, channel);
	/* udev may take a moment to apply group permissions to the new node */
	for (int i = 0; i < 20; i++) {
		char probe[320];
		snprintf(probe, sizeof probe, "%s/enable", p->dir);
		if (access(probe, W_OK) == 0) break;
		pf_sleep_ms(50);
	}
	return p;
}

int pf_pwm_config(pf_pwm *p, int hz, double duty_pct)
{
	if (!p || hz <= 0) return -EINVAL;
	long period = 1000000000L / hz;
	long duty = (long)(period * pf_clamp(duty_pct, 0, 100) / 100.0);
	char v[32];
	int rc;
	/* duty must never exceed period: shrink duty first when the period shrinks */
	if (period < p->period_ns) {
		snprintf(v, sizeof v, "%ld", duty);
		if ((rc = write_attr(p->dir, "duty_cycle", v))) return rc;
	}
	if (period != p->period_ns) {
		snprintf(v, sizeof v, "%ld", period);
		if ((rc = write_attr(p->dir, "period", v))) return rc;
		p->period_ns = period;
	}
	snprintf(v, sizeof v, "%ld", duty);
	return write_attr(p->dir, "duty_cycle", v);
}

int pf_pwm_enable(pf_pwm *p, bool on)
{
	if (!p) return -EINVAL;
	return write_attr(p->dir, "enable", on ? "1" : "0");
}

void pf_pwm_close(pf_pwm *p)
{
	if (!p) return;
	write_attr(p->dir, "enable", "0");
	free(p);
}
