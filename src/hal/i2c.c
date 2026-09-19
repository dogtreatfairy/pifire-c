#define _GNU_SOURCE
#include "hal/i2c.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int pf_i2c_open(int bus)
{
	char path[32];
	snprintf(path, sizeof path, "/dev/i2c-%d", bus);
	int fd = open(path, O_RDWR | O_CLOEXEC);
	return fd < 0 ? -errno : fd;
}

int pf_i2c_write(int fd, uint8_t addr, const uint8_t *buf, size_t n)
{
	struct i2c_msg msg = { .addr = addr, .flags = 0, .len = (uint16_t)n, .buf = (uint8_t *)buf };
	struct i2c_rdwr_ioctl_data d = { .msgs = &msg, .nmsgs = 1 };
	return ioctl(fd, I2C_RDWR, &d) < 0 ? -errno : 0;
}

int pf_i2c_write_read(int fd, uint8_t addr, const uint8_t *wbuf, size_t wn, uint8_t *rbuf, size_t rn)
{
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = 0, .len = (uint16_t)wn, .buf = (uint8_t *)wbuf },
		{ .addr = addr, .flags = I2C_M_RD, .len = (uint16_t)rn, .buf = rbuf },
	};
	struct i2c_rdwr_ioctl_data d = { .msgs = msgs, .nmsgs = 2 };
	return ioctl(fd, I2C_RDWR, &d) < 0 ? -errno : 0;
}

void pf_i2c_close(int fd)
{
	if (fd >= 0) close(fd);
}
