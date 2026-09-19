#pragma once
#include <stddef.h>
#include <stdint.h>

int pf_spi_open(int bus, int cs, uint8_t mode, uint32_t hz);   /* /dev/spidev<bus>.<cs> */
/* full-duplex transfer of n bytes */
int pf_spi_xfer(int fd, const uint8_t *tx, uint8_t *rx, size_t n);
void pf_spi_close(int fd);
