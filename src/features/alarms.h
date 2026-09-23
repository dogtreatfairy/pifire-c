#pragma once
/* The alarm state table: one shared, authoritative answer to "what is wrong with the grill right
 * now, and what has happened that nobody has looked at yet".
 *
 * The distinction this module exists to make is the one industrial alarm practice (ISA-18.2,
 * EEMUA 191) is built on, and the one the daemon used to lack entirely:
 *
 *   An ALARM is a CONDITION. It becomes true, and later it becomes false again. A probe that has
 *   gone quiet, a hopper that is low, a grill that is running cold: each is a statement about the
 *   present that stops being true when the thing is fixed. An alarm therefore has a return to
 *   normal, and when it returns the alarm goes away by itself. Nobody should have to tell a
 *   controller that the probe they just plugged back in is no longer disconnected.
 *
 *   A NOTICE is a MOMENT. A tuning run finished; a timer expired; an update installed. It never
 *   becomes false, because it was never a state. It stays until somebody acknowledges it.
 *
 * Both live in one table so there is one badge, one list and one "clear". Acknowledgement is
 * recorded here rather than in a browser, so clearing something on a phone clears it on the laptop
 * too: the table is the truth and every client renders it.
 *
 * The table is deliberately not persisted. Alarms are conditions, and conditions are re-evaluated
 * from scratch every tick after a restart, so anything still wrong comes straight back and
 * anything already fixed does not. That is also what stops the app replaying an old event log on
 * every page load and calling it news. */
#include "core/events.h"
#include <cJSON.h>
#include <stdbool.h>

#define PF_ALARMS_MAX 64

void pf_alarms_init(void);

/* Raise a condition, or refresh one already standing. `key` is its identity and must be stable
 * across ticks -- the rule and the instance it is about, not the message, which may carry a
 * changing temperature. Returns true only when this is a new activation, which is the caller's cue
 * to send it to a phone: a condition that is merely still true is not news. */
bool pf_alarms_raise(const char *key, const char *code, const char *name, int crit, unsigned sinks,
                     const char *title, const char *body);

/* The condition is false again. An alarm nobody has acknowledged and that mattered (high or
 * critical) stays in the list marked as returned to normal, so a safety event cannot be erased by
 * fixing itself; anything else simply goes. */
void pf_alarms_clear(const char *key);

/* The rule that raised this has stopped being evaluated -- switched off, or the cook it only
 * watches during has ended. That is not a return to normal: we never saw the condition end, we
 * stopped looking at it. So nothing is kept, not even a critical one, because a record saying
 * "this was wrong when we last looked" is a record nobody can act on and it would sit in the list
 * for ever. A low hopper matters while the grill is burning pellets; once it stops, it does not. */
void pf_alarms_retire(const char *key);

/* A moment rather than a condition: it has no return to normal and waits to be acknowledged. */
void pf_alarms_note(const char *code, const char *name, int crit, unsigned sinks,
                    const char *title, const char *body);

/* Acknowledge: the person has seen it. An alarm still standing stays in the list until its
 * condition clears; anything already clear, and every notice, leaves at once. */
int  pf_alarms_ack(const char *key);
int  pf_alarms_ack_all(void);
/* Silence one for a while without fixing it, and without pretending it is not there. */
int  pf_alarms_shelve(const char *key, double seconds);

cJSON   *pf_alarms_json(void);
unsigned pf_alarms_generation(void);
int      pf_alarms_unacked(void);
