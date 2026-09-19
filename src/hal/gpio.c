#define _GNU_SOURCE
#include "hal/gpio.h"
#include "core/log.h"
#include "core/util.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TAG "gpio"

struct pf_gpio_line {
	int fd;
	unsigned offset;
};

int pf_gpio_open_chip(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) LOGE(TAG, "open %s: %s", path, strerror(errno));
	return fd;
}

void pf_gpio_close_chip(int fd)
{
	if (fd >= 0) close(fd);
}

static pf_gpio_line *request(int chipfd, unsigned offset, uint64_t flags, int initial, const char *consumer)
{
	struct gpio_v2_line_request req;
	memset(&req, 0, sizeof req);
	req.offsets[0] = offset;
	req.num_lines = 1;
	req.config.flags = flags;
	if (initial >= 0) {
		req.config.num_attrs = 1;
		req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
		req.config.attrs[0].attr.values = initial ? 1 : 0;
		req.config.attrs[0].mask = 1;
	}
	pf_strlcpy(req.consumer, consumer, sizeof req.consumer);
	if (ioctl(chipfd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		LOGE(TAG, "request line %u (%s): %s", offset, consumer, strerror(errno));
		return NULL;
	}
	pf_gpio_line *l = calloc(1, sizeof *l);
	l->fd = req.fd;
	l->offset = offset;
	return l;
}

pf_gpio_line *pf_gpio_request_output(int chipfd, unsigned offset, bool active_low, bool initial_active, const char *consumer)
{
	uint64_t flags = GPIO_V2_LINE_FLAG_OUTPUT | (active_low ? GPIO_V2_LINE_FLAG_ACTIVE_LOW : 0);
	return request(chipfd, offset, flags, initial_active ? 1 : 0, consumer);
}

pf_gpio_line *pf_gpio_request_input(int chipfd, unsigned offset, bool active_low, pf_gpio_bias bias, const char *consumer)
{
	uint64_t flags = GPIO_V2_LINE_FLAG_INPUT | (active_low ? GPIO_V2_LINE_FLAG_ACTIVE_LOW : 0);
	if (bias == PF_GPIO_BIAS_PULL_UP) flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
	else if (bias == PF_GPIO_BIAS_PULL_DOWN) flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
	return request(chipfd, offset, flags, -1, consumer);
}

int pf_gpio_set(pf_gpio_line *l, bool active)
{
	if (!l) return -EINVAL;
	struct gpio_v2_line_values v = { .bits = active ? 1 : 0, .mask = 1 };
	if (ioctl(l->fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0) return -errno;
	return 0;
}

int pf_gpio_get(pf_gpio_line *l)
{
	if (!l) return -EINVAL;
	struct gpio_v2_line_values v = { .bits = 0, .mask = 1 };
	if (ioctl(l->fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) return -errno;
	return (int)(v.bits & 1);
}

void pf_gpio_release(pf_gpio_line *l)
{
	if (!l) return;
	close(l->fd);
	free(l);
}
