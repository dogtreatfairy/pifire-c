#pragma once
/* Per-probe spike rejection, smoothing and a rate-of-change gate, values in Celsius.
 *
 * Three stages, each answering a different kind of wrong reading.
 *
 * A median of five removes an isolated wrong sample, or two in a row: a thermistor on a long lead
 * in a noisy electrical environment throws the occasional reading that is nowhere near the truth
 * but still inside the plausible range, so the Steinhart-Hart conversion cannot catch it, and an
 * isolated wrong sample is never the middle value of its neighbours. A genuine climb passes
 * through a median untouched.
 *
 * A running mean of ten takes the jitter off what survived. Together these cost about two seconds
 * at the four-hertz poll, on a pit whose dead time is closer to a minute.
 *
 * The gate answers what neither can: a wrong level that lasts seconds rather than samples. A
 * sheathed pit probe cannot read a change faster than its own thermal mass lets it, so a filtered
 * value that runs away from what was last reported faster than `slew_c_s` is not yet believed:
 * the reported value follows it at that rate and no faster, and is marked unstable. If the
 * disagreement is still there after `hold_s` the new level is accepted whole, because a change
 * that lasts that long is the grill, however it got there. This is the rate-of-change validation
 * industrial transmitters call spike suppression with a hold time, and it is what the original
 * PiFire's standard-deviation gate was doing by other means -- holding its last average while the
 * readings scattered -- without that gate's habit of freezing through a startup ramp. */
#include <stdbool.h>

#define PF_TEMPQ_MED 5     /* raw samples the median spans: removes up to two bad ones in a row */
#define PF_TEMPQ_LEN 10    /* samples the running mean averages */

typedef struct {
	double med[PF_TEMPQ_MED];
	double q[PF_TEMPQ_LEN];
	int len;
	double last_avg;
	bool have_avg;
	/* the gate; slew_c_s 0 = off */
	double slew_c_s, hold_s;
	double out_c, out_t;        /* what was last reported, and when */
	double hold_since;          /* when the filtered value first ran away faster than the slew allows; 0 = it has not */
	bool unstable;              /* the reported value is being held back from where the filter says */
} pf_tempq;

void   pf_tempq_init(pf_tempq *t);
void   pf_tempq_reset(pf_tempq *t);
/* The gate's limits: the fastest the reported value may move, C per second, and how long a
 * faster disagreement is held off before it is believed. Survives a reset. */
void   pf_tempq_limits(pf_tempq *t, double slew_c_s, double hold_s);
/* Push a raw sample taken at `now` and return the value to report, or NAN before any sample has arrived. */
double pf_tempq_push(pf_tempq *t, double c, double now);
