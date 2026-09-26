#pragma once
/* Conditional notifications: user-written rules that watch the grill and send a message when what
 * they describe becomes true. Modelled on Home Assistant's entity/trait idea and Node-RED's
 * if/then feel, and evaluated once a second against the same status JSON the UI receives, so any
 * field added to the status becomes something a rule can test without touching this engine.
 *
 * Rules live in settings.notify.rules[]. See docs/conditional-notifications.md. */
#include "pifire/common.h"
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The clocks behind a condition's "for N seconds", one per node of one tree.
 *
 * A plain value type so anything evaluating a tree over time -- a rule, a recipe step -- can hold
 * its own without allocating. Zero it to start the tree's clocks again from now. */
#define PF_RULES_NODE_TIMERS 12
typedef struct { uint32_t path; double since; } pf_rules_node_timer;
typedef struct { pf_rules_node_timer t[PF_RULES_NODE_TIMERS]; } pf_rules_clocks;

void pf_rules_init(void);
void pf_rules_shutdown(void);
/* Evaluate every enabled rule against this status snapshot and fire whatever has become true. */
void pf_rules_tick(const cJSON *status, double now);

/* The catalogue the editor builds its dropdowns from: domains, their instances, each trait's type,
 * unit and operators, and the tokens a message may use. */
cJSON *pf_rules_catalogue_json(const cJSON *status);
/* The rules with their live state (matched instances, holding since, last fired). */
cJSON *pf_rules_state_json(void);
/* Render one rule's message against the current status and send it. 0 on success. */
int pf_rules_test(const cJSON *rule, const cJSON *status, char *err, size_t n);
/* Render a rule's title and body without sending anything, for the editor's live preview.
 * Reports how many entities the rule currently selects and how many of them match right now. */
void pf_rules_preview(const cJSON *rule, const cJSON *status, char *title, size_t tn, char *body, size_t bn,
                      int *selected, int *matching);

/* Evaluate one condition tree -- the kind the notification editor builds -- against a facts object,
 * as if the tree belonged to `domain`.
 *
 * Exposed so a recipe step can say when it ends in exactly the words and shapes a notification
 * says when it fires: same editor, same operators, same "for N minutes". `clocks` carries those
 * durations across calls and may be NULL to ask only what is true this instant. */
bool pf_rules_eval_tree(const cJSON *node, const cJSON *facts, const char *domain,
                        pf_rules_clocks *clocks, double now);

/* Rewrite the temperatures inside a condition tree from one unit to the other, asking the trait
 * catalogue which of its values are temperatures and which are gaps rather than readings. A recipe
 * is stored in the unit it was written in, so its conditions are converted to the grill's unit
 * before they are compared against it. */
void pf_rules_convert_tree(cJSON *node, const char *domain, pf_units from, pf_units to);
