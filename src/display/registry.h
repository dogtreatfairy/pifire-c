#pragma once
#include "pifire/display.h"

/* Instantiate settings.modules.display ("none" today; SPI/OLED drivers plug in here). */
int  pf_display_init(void);
void pf_display_shutdown(void);
/* Called ~2 Hz from the services thread with the status JSON the UI receives. */
void pf_display_tick(const char *status_json);
void pf_display_text(const char *msg);
const pf_display_ops *pf_display_none(void);
