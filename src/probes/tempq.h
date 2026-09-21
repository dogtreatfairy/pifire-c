#pragma once
/* Per-probe spike rejection and smoothing, values in Celsius.
 *
 * A thermistor on a long lead in a noisy electrical environment throws the occasional reading that
 * is nowhere near the truth but still inside the plausible range, so the Steinhart-Hart conversion
 * cannot catch it. Those are removed here, by a median: an isolated wrong sample is never the
 * middle value of its neighbours, while a genuine climb passes through a median untouched, which
 * matters because the fastest real movement a grill makes is the startup ramp.
 *
 * The median feeds a short running mean that takes the jitter off the rest. Together they cost
 * about two seconds of lag on a pit whose dead time is closer to a minute. */
#include <stdbool.h>

#define PF_TEMPQ_MED 5     /* raw samples the median spans: removes up to two bad ones in a row */
#define PF_TEMPQ_LEN 10    /* samples the running mean averages */

typedef struct {
	double med[PF_TEMPQ_MED];
	double q[PF_TEMPQ_LEN];
	int len;
	double last_avg;
	bool have_avg;
} pf_tempq;

void   pf_tempq_init(pf_tempq *t);
void   pf_tempq_reset(pf_tempq *t);
/* Push a raw sample and return the filtered value, or NAN before any sample has arrived. */
double pf_tempq_push(pf_tempq *t, double c);
