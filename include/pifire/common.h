#pragma once
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PF_VERSION
#define PF_VERSION "0.0.0"
#endif

#define PF_MAX_PROBES   16
#define PF_MAX_DEVICES  8
#define PF_LABEL_LEN    32
#define PF_NAME_LEN     64

typedef enum { PF_UNITS_F = 0, PF_UNITS_C = 1 } pf_units;

static inline double pf_c_to_f(double c) { return c * 9.0 / 5.0 + 32.0; }
static inline double pf_f_to_c(double f) { return (f - 32.0) * 5.0 / 9.0; }
static inline double pf_to_c(double v, pf_units u) { return u == PF_UNITS_C ? v : pf_f_to_c(v); }
static inline double pf_from_c(double c, pf_units u) { return u == PF_UNITS_C ? c : pf_c_to_f(c); }
/* temperature *differences* (no 32 offset) */
/* There is one model of the grill and one rule for turning it into a tuning.
 *
 * The model is first order plus dead time: a static gain K in degrees per unit of feed, a time
 * constant tau, and a dead time theta. Two quite different measurements produce it. The rise of
 * every startup is fitted directly. A relay test measures a single point on the frequency
 * response, which is turned into the same three numbers below. Whichever arrives, the grill is
 * described the same way and designed for the same way, instead of one measurement meaning
 * Tyreus-Luyben and the other meaning SIMC and the two being averaged together.
 *
 * The rule is SIMC (Skogestad), with the closed-loop time constant set equal to the dead time,
 * which is his recommended "tight but robust" choice. SIMC is a PI rule for a plant that is truly
 * first order, and a PID rule for one with a second lag. A grill is the second kind -- firepot,
 * barrel and probe each lag -- but fitting three numbers to it lumps those together and calls most
 * of them dead time, so a derivative of about a third of that recovers what the lumping hid.
 * Without it the pit overshoots its target and sits there. */
static inline void pf_tuning_from_plant(double K, double tau, double theta,
                                        double *PB_c, double *Ti, double *Td)
{
	if (PB_c) *PB_c = 0;
	if (Ti) *Ti = 0;
	if (Td) *Td = 0;
	if (!(K > 0) || !(tau > 0) || !(theta > 0)) return;
	double tc = theta;
	double kc = tau / (K * (tc + theta));
	if (!(kc > 0)) return;
	if (PB_c) *PB_c = 1.0 / kc;
	if (Ti) *Ti = fmin(tau, 4.0 * (tc + theta));
	if (Td) *Td = theta / 3.0;
}

/* What a relay test measured, turned into a tuning by the rule written for exactly that
 * measurement (Tyreus-Luyben).
 *
 * The relay gives two numbers and two only: the ultimate gain Ku and the period Pu of the limit
 * cycle it drove the grill into. Both are read straight off the swing, and neither needs anything
 * known beforehand. Tyreus-Luyben is the conservative of the classical rules -- Ziegler-Nichols
 * hunts on a process as lag-dominant as a barrel of air -- and it suits a controller whose
 * feed-forward already carries the steady load, so the integral only has to trim.
 *
 * This replaced routing a relay result through the three-parameter model below. The model needs a
 * static gain the relay cannot see, and then splits the measured phase lag between a time constant
 * and a dead time; the band SIMC returns is proportional to that dead time. Two runs on the same
 * grill a day apart measured periods of about 370 s and 603 s, and the split turned that into dead
 * times of 99 s and 168 s and bands of 82 F and 150 F -- while the relay's own answer for the
 * second run was 93 F, thirteen per cent from the first. A tuning that swings by nearly a factor
 * of two because the limit cycle was slower is not a measurement of the grill, and the number that
 * moved was never one the relay measured. */
/* Which rule turns an ultimate gain and period into PID gains. Defined by the daemon (util.c),
 * read here, so the relay, the controller and a test all design the same way.
 *
 *   TYREUS_LUYBEN   Kc = Ku/2.2, Ti = 2.2 Pu, Td = Pu/6.3. Robust and slow: the integral takes
 *                   over two periods, which on a grill whose feed-forward is even slightly off
 *                   means a quarter of an hour of sitting the wrong side of the set point.
 *   TL_FAST_I       the same gain, with the integral at one period and Td = Pu/8. Keeps
 *                   Tyreus-Luyben's margin on the proportional term -- the one that decides
 *                   whether the loop hunts -- and corrects an offset in the time the loop's own
 *                   oscillation takes, which is what a cook waiting at the grill will accept.
 *   ZN_OVERSHOOT    Ziegler-Nichols "some overshoot": Kc = Ku/3, Ti = Pu/2, Td = Pu/3.
 *   COHEN_COON      from the identified first-order model; the rule the grill was hand-tuned
 *                   with, and aggressive on a lag-dominant plant. Needs K, tau and theta and
 *                   falls back to TL_FAST_I without them. */
typedef enum { PF_RULE_TYREUS_LUYBEN = 0, PF_RULE_TL_FAST_I, PF_RULE_ZN_OVERSHOOT, PF_RULE_COHEN_COON } pf_tuning_rule;
extern int pf_tuning_rule_selected;

static inline void pf_tuning_from_relay_plant(double Ku, double Pu, double K, double tau, double theta,
                                              double *PB_c, double *Ti, double *Td)
{
	if (PB_c) *PB_c = 0;
	if (Ti) *Ti = 0;
	if (Td) *Td = 0;
	if (!(Ku > 0) || !(Pu > 0)) return;
	int rule = pf_tuning_rule_selected;
	if (rule == PF_RULE_COHEN_COON && !(K > 0 && tau > 0 && theta > 0)) rule = PF_RULE_TL_FAST_I;
	double kc, ti, td;
	switch (rule) {
	case PF_RULE_TL_FAST_I:   kc = Ku / 2.2; ti = Pu;       td = Pu / 8.0; break;
	case PF_RULE_ZN_OVERSHOOT: kc = Ku / 3.0; ti = Pu / 2.0; td = Pu / 3.0; break;
	case PF_RULE_COHEN_COON: {
		double r = theta / tau;
		kc = (tau / (K * theta)) * (4.0 / 3.0 + r / 4.0);
		ti = theta * (32.0 + 6.0 * r) / (13.0 + 8.0 * r);
		td = 4.0 * theta / (11.0 + 2.0 * r);
		break;
	}
	default:                  kc = Ku / 2.2; ti = 2.2 * Pu; td = Pu / 6.3; break;
	}
	if (PB_c) *PB_c = 1.0 / kc;
	if (Ti) *Ti = ti;
	if (Td) *Td = td;
}

/* The relay's numbers alone, designed by the selected rule (Cohen-Coon needs the plant and so
 * designs as TL_FAST_I here). */
static inline void pf_tuning_from_relay(double Ku, double Pu, double *PB_c, double *Ti, double *Td)
{
	pf_tuning_from_relay_plant(Ku, Pu, 0, 0, 0, PB_c, Ti, Td);
}

/* A relay test fixes one point on the frequency response: at the frequency of the limit cycle the
 * grill's phase lag is 180 degrees and its gain is 1/Ku. That is two equations, and a first order
 * plus dead time model has three unknowns, so the static gain has to come from elsewhere: the
 * steady feed the grill needs per degree, which the feed-forward measures across cooks, or the
 * last fit of a startup rise. With K known the rest follows exactly.
 *
 * Returns false when the numbers cannot describe such a plant, which happens if K*Ku <= 1: the
 * grill would have to have more gain at the oscillation frequency than it has standing still. */
static inline bool pf_plant_from_relay(double Ku, double Pu, double K, double *tau, double *theta)
{
	if (!(Ku > 0) || !(Pu > 0) || !(K > 0)) return false;
	double w = 2.0 * 3.14159265358979323846 / Pu;
	double kku = K * Ku;
	if (kku <= 1.0001) return false;                       /* |G| = K/sqrt(1+(w*tau)^2) = 1/Ku */
	double t = sqrt(kku * kku - 1.0) / w;
	double th = (3.14159265358979323846 - atan(w * t)) / w; /* -w*theta - atan(w*tau) = -pi */
	if (!(t > 0) || !(th > 0)) return false;
	if (tau) *tau = t;
	if (theta) *theta = th;
	return true;
}

static inline double pf_delta_to_c(double d, pf_units u) { return u == PF_UNITS_C ? d : d * 5.0 / 9.0; }
static inline double pf_delta_from_c(double d, pf_units u) { return u == PF_UNITS_C ? d : d * 9.0 / 5.0; }

typedef enum {
	PF_MODE_STOP = 0,
	PF_MODE_MONITOR,
	PF_MODE_PRIME,
	PF_MODE_STARTUP,
	PF_MODE_REIGNITE,
	PF_MODE_SMOKE,
	PF_MODE_HOLD,
	PF_MODE_SHUTDOWN,
	PF_MODE_MANUAL,
	PF_MODE_ERROR,
	PF_MODE_COUNT
} pf_mode;

const char *pf_mode_name(pf_mode m);
int pf_mode_from_name(const char *s); /* -1 if unknown */

/* Is there a fire the grill is keeping alight in this mode?
 *
 * Startup and Reignite are lighting one, Smoke and Hold are feeding one. Shutdown is deliberately
 * not one of them: the fire is still in there, but it is already on its way out and nothing is
 * being asked of the cook. Stop, Monitor, Prime, Manual and Error are not the grill's own fire to
 * answer for. */
static inline bool pf_mode_is_firing(pf_mode m)
{
	return m == PF_MODE_STARTUP || m == PF_MODE_REIGNITE || m == PF_MODE_SMOKE || m == PF_MODE_HOLD;
}

typedef enum { PF_OUT_POWER = 0, PF_OUT_FAN, PF_OUT_AUGER, PF_OUT_IGNITER, PF_OUT_COUNT } pf_output;
const char *pf_output_name(pf_output o);

typedef enum { PF_PROBE_PRIMARY = 0, PF_PROBE_FOOD, PF_PROBE_AUX } pf_probe_role;

typedef enum { PF_IN_SELECTOR = 0, PF_IN_SHUTDOWN, PF_IN_COUNT } pf_input;

/* Services the daemon hands to every plugin. Plugins never include daemon headers. */
typedef struct pf_env pf_env;
struct pf_env {
	void (*log)(int level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
	/* Persistent key/value (JSON text), namespaced per plugin instance by the daemon.
	 * kv_get returns 0 found, 1 missing, <0 error. */
	int (*kv_get)(const pf_env *env, const char *key, char *json_out, size_t n);
	int (*kv_put)(const pf_env *env, const char *key, const char *json);
	const char *ns;   /* namespace, e.g. "controller.pid" */
	void *ctx;        /* daemon-private */
};

enum { PF_LVL_DEBUG = 0, PF_LVL_INFO, PF_LVL_WARN, PF_LVL_ERROR };
