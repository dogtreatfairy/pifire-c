#pragma once
/* Small SHA-256 (FIPS 180-4) for verifying downloaded release archives. */
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t buflen; } pf_sha256;

void pf_sha256_init(pf_sha256 *s);
void pf_sha256_update(pf_sha256 *s, const void *data, size_t n);
void pf_sha256_final(pf_sha256 *s, unsigned char out[32]);
/* hex digest of a whole file; returns 0 on success */
int  pf_sha256_file(const char *path, char hex[65]);
