#define _GNU_SOURCE
#include "hal/spi.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int pf_spi_open(int bus, int cs, uint8_t mode, uint32_t hz)
{
	char path[32];
	snprintf(path, sizeof path, "/dev/spidev%d.%d", bus, cs);
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) return -errno;
	uint8_t bits = 8;
	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
	    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	return fd;
}

int pf_spi_xfer(int fd, const uint8_t *tx, uint8_t *rx, size_t n)
{
	struct spi_ioc_transfer t;
	memset(&t, 0, sizeof t);
	t.tx_buf = (unsigned long)tx;
	t.rx_buf = (unsigned long)rx;
	t.len = (uint32_t)n;
	return ioctl(fd, SPI_IOC_MESSAGE(1), &t) < 0 ? -errno : 0;
}

void pf_spi_close(int fd)
{
	if (fd >= 0) close(fd);
}
