#pragma once
#include <stdbool.h>

int  pf_web_start(const char *bind_addr, int port);
void pf_web_stop(void);
/* Broadcast the current status to all WebSocket clients now (also done automatically at 1 Hz). */
void pf_web_push_status(void);
