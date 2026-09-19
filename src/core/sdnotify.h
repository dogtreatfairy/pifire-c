#pragma once
/* Minimal sd_notify(3) without linking libsystemd: writes to $NOTIFY_SOCKET if set. */
void pf_sd_notify(const char *state);   /* e.g. "READY=1", "WATCHDOG=1", "STOPPING=1", "STATUS=..." */
int pf_sd_watchdog_usec(void);          /* $WATCHDOG_USEC or 0 */
