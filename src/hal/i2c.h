#pragma once
#include <stddef.h>
#include <stdint.h>

int pf_i2c_open(int bus);                                  /* /dev/i2c-<bus>, returns fd or <0 */
int pf_i2c_write(int fd, uint8_t addr, const uint8_t *buf, size_t n);
/* write `wn` bytes (register address etc.) then read `rn` bytes in one transaction (repeated start) */
int pf_i2c_write_read(int fd, uint8_t addr, const uint8_t *wbuf, size_t wn, uint8_t *rbuf, size_t rn);
void pf_i2c_close(int fd);
