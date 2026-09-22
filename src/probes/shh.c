#include "probes/shh.h"
#include "pifire/common.h"
#include <math.h>
#include <stdbool.h>

double pf_shh_mv_to_ohms(double mv, double Rd, double Vs)
{
	if (!(mv > 0) || mv > Vs * 1000.0 * 1.01) return -1;
	double Vo = mv / 1000.0;
	double denom = Vs - Vo;
	/* An unplugged probe leaves the divider's pull-up holding the input at the rail, so this
	 * difference goes to zero. Pinning it to a thousandth of a volt turned that into a resistance
	 * of megohms and handed it on as if it were a reading: for a thermistor, whose resistance
	 * rises as it cools, megohms is simply a very cold probe, and an empty jack can be reported as
	 * a plausible temperature. There is no reading here; say so. */
	if (denom < 0.002) return -1;
	return Vo * Rd / denom;
}

double pf_shh_ohms_to_c(double ohms, const pf_shh *p)
{
	if (!(ohms > 0)) return NAN;
	double ln = log(ohms);
	double invT = p->A + p->B * ln + p->C * ln * ln * ln;
	if (invT == 0) return NAN;
	double c = 1.0 / invT - 273.15;
	double f = pf_c_to_f(c);
	/* The range a probe can sensibly report. The floor used to be 0 F, which threw away real
	 * readings on a winter morning: an ambient probe outside in Wisconsin is below that for weeks.
	 * It can be widened safely only because an open circuit is now rejected before it gets here,
	 * rather than arriving as a very cold thermistor. */
	if (f < -40 || f > 600) return NAN;
	return c;
}

static double inv_t(double L, const pf_shh *p) { return p->A + p->B * L + p->C * L * L * L; }

double pf_shh_c_to_ohms(double c, const pf_shh *p)
{
	double tK = c + 273.15;
	double target = 1.0 / tK;
	if (p->C == 0) return exp((target - p->A) / p->B);

	/* closed form (valid when the cubic discriminant is positive) */
	double x = (1.0 / (2.0 * p->C)) * (p->A - target);
	double disc = pow(p->B / (3.0 * p->C), 3) + x * x;
	if (disc >= 0) {
		double y = sqrt(disc);
		double r = exp(cbrt(y - x) - cbrt(y + x));
		if (!isnan(r) && !isinf(r)) return r;
	}

	/* Otherwise the cubic has three real roots; pick the physically right branch: NTC thermistor
	 * fits (B > 0) have T falling with R (d(1/T)/dL > 0), RTD-style fits (B < 0) the opposite. */
	bool want_positive_slope = p->B > 0;
	double best = NAN, lo = 0, hi = log(1e7), step = 0.02;
	double prev_L = lo, prev_f = inv_t(lo, p) - target;
	for (double L = lo + step; L <= hi; L += step) {
		double f = inv_t(L, p) - target;
		if ((prev_f <= 0 && f >= 0) || (prev_f >= 0 && f <= 0)) {
			double slope = p->B + 3.0 * p->C * L * L;
			if ((slope > 0) == want_positive_slope) {
				double a = prev_L, b = L;
				for (int i = 0; i < 60; i++) {
					double m = 0.5 * (a + b), fm = inv_t(m, p) - target;
					if ((fm >= 0) == (inv_t(a, p) - target >= 0)) a = m; else b = m;
				}
				best = exp(0.5 * (a + b));
				break;
			}
		}
		prev_L = L; prev_f = f;
	}
	return best;
}

int pf_shh_solve(double t1, double r1, double t2, double r2, double t3, double r3, pf_shh *out)
{
	if (!(r1 > 0 && r2 > 0 && r3 > 0)) return -1;
	double L1 = log(r1), L2 = log(r2), L3 = log(r3);
	double Y1 = 1.0 / (t1 + 273.15), Y2 = 1.0 / (t2 + 273.15), Y3 = 1.0 / (t3 + 273.15);
	if (L1 == L2 || L1 == L3 || L2 == L3) return -1;
	double g2 = (Y2 - Y1) / (L2 - L1);
	double g3 = (Y3 - Y1) / (L3 - L1);
	double C = ((g3 - g2) / (L3 - L2)) / (L1 + L2 + L3);
	double B = g2 - C * (L1 * L1 + L1 * L2 + L2 * L2);
	double A = Y1 - (B + L1 * L1 * C) * L1;
	out->A = A; out->B = B; out->C = C;
	return (isnan(A) || isnan(B) || isnan(C)) ? -1 : 0;
}
