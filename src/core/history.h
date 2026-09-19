#pragma once
/* RAM ring of history samples written by the control thread (every settings.history.sample_s),
 * flushed to SQLite by the services thread in one transaction. Also feeds the controller's
 * pf_history view. */
#include "core/db.h"
#include "core/status.h"
#include "pifire/controller.h"

void pf_history_init(void);
/* Record from the current status if sample_s has elapsed. */
void pf_history_record(const pf_status *s, double now, double sample_s);
/* Move pending samples to the database. Returns rows written. */
int  pf_history_flush(void);
/* 1 Hz controller view (last 60 min). Valid until the next pf_history_record on the control thread. */
const pf_history *pf_history_ctrl_view(void);
void pf_history_clear(void);   /* clears RAM ring and DB (StartUp with clear_on_startup) */
