#pragma once
/* Conditional notifications: user-written rules that watch the grill and send a message when what
 * they describe becomes true. Modelled on Home Assistant's entity/trait idea and Node-RED's
 * if/then feel, and evaluated once a second against the same status JSON the UI receives, so any
 * field added to the status becomes something a rule can test without touching this engine.
 *
 * Rules live in settings.notify.rules[]. See docs/conditional-notifications.md. */
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>

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
