#pragma once
/* Minimal QR encoder for the panel: byte mode, error-correction level L, versions 1-5.
 * That is up to 106 bytes, which covers any URL the grill needs to show ("http://10.0.0.5/").
 * Versions 1-5 at level L are single-block, so no codeword interleaving is needed. */
#include <stdbool.h>
#include <stdint.h>

#define PF_QR_MAX_SIZE 37   /* version 5: 17 + 4*5 */

typedef struct {
	int size;                                     /* modules per side */
	uint8_t m[PF_QR_MAX_SIZE][PF_QR_MAX_SIZE];    /* m[y][x], 1 = dark */
} pf_qr;

/* Encode text; false when it does not fit in version 5 at level L. */
bool pf_qr_encode(const char *text, pf_qr *out);
