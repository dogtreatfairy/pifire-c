#pragma once
/* Chef iQ (CQ50 / CQ60) advertisement parser, shared with the unit test. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PF_CHEFIQ_MFR_ID 0x05CD

typedef struct {
	bool has_temps;               /* food / ambient / tips updated */
	double food_c, ambient_c;     /* NAN when absent or out of range */
	double tip_c[4]; int ntips;
	bool has_battery; int battery_pct, soc_c;
	int packet_type;              /* 0 name, 1 temperature, 3 status */
	int ver_major, ver_minor, ver_patch;
} pf_chefiq_reading;

/* Parse the manufacturer-specific payload (after the 0x05CD company id). Returns 0 on success, -1 if
 * the payload is not a probe packet. */
int pf_chefiq_parse(const uint8_t *msg, size_t len, pf_chefiq_reading *out);
