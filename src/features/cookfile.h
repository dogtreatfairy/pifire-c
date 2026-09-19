#pragma once
/* Cook files: when a cook ends, the history slice, events and summary metrics are written to a
 * JSON file under <data>/cookfiles and indexed in the cooks table. */
#include <cJSON.h>

void pf_cookfile_init(const char *data_dir);
/* Called by control when a cook that started at start_wall ends. Returns the new cook id or <0. */
int  pf_cookfile_finish(double start_wall, double end_wall, double auger_on_s, double max_pit_c);
cJSON *pf_cookfile_list(void);
/* Full file contents (caller frees) or NULL. */
char *pf_cookfile_read(int id);
int  pf_cookfile_delete(int id);
int  pf_cookfile_rename(int id, const char *name);
