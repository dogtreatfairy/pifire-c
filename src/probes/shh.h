#pragma once
/* Steinhart-Hart thermistor conversions and resistor-divider math (port of probes/base.py). */

typedef struct { double A, B, C; } pf_shh;

/* mV at the divider midpoint -> probe resistance (ohms). Rd = divider resistor, Vs = supply volts.
 * Returns <0 if the reading is outside the valid band (open/short/over-reference). */
double pf_shh_mv_to_ohms(double mv, double Rd, double Vs);
/* resistance -> Celsius. Returns NAN if outside 0..600 F (matches Python sanity clamp). */
double pf_shh_ohms_to_c(double ohms, const pf_shh *p);
/* Celsius -> resistance (inverse Steinhart-Hart). */
double pf_shh_c_to_ohms(double c, const pf_shh *p);
/* Solve A,B,C from three (Celsius, ohms) calibration points. Returns 0 on success. */
int pf_shh_solve(double t1, double r1, double t2, double r2, double t3, double r3, pf_shh *out);
