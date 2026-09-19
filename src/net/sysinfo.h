#pragma once
#include <cJSON.h>

/* {"version","uptime_s","cpu_temp_c","throttled","under_voltage","mem_total","mem_available",
 *  "load1","wifi_quality_pct","interfaces":[{"name","ip","mac"}],"hostname"} */
cJSON *pf_sysinfo_json(void);
