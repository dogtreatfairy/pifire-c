#pragma once
/* Cook files: when a cook ends, the history slice, events and summary metrics are written to a
 * JSON file under <data>/cookfiles and indexed in the cooks table. */
#include <cJSON.h>

void pf_cookfile_init(const char *data_dir);
/* Called by control when a cook that started at start_wall ends. Returns the new cook id or <0.
 * Blocks: a query over the whole cook plus a file write. Not for the control thread. */
int  pf_cookfile_finish(double start_wall, double end_wall, double auger_on_s, double max_pit_c);
/* Queue that work instead of doing it. The control loop must tick every 100 ms and the watchdog
 * aborts the daemon after two seconds without one, while writing a long cook to an SD card runs
 * into several; ending a cook used to do it inline and take the daemon down with it. */
void pf_cookfile_request(double start_wall, double end_wall, double auger_on_s, double max_pit_c);
/* Write whatever is queued. Call from a thread that is allowed to block. */
void pf_cookfile_pending_run(void);
cJSON *pf_cookfile_list(void);
/* Full file contents (caller frees) or NULL. */
char *pf_cookfile_read(int id);
int  pf_cookfile_delete(int id);
int  pf_cookfile_rename(int id, const char *name);
