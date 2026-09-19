#pragma once
/* GPIO via the Linux character-device uAPI v2 (/dev/gpiochipN). No libgpiod dependency. */
#include <stdbool.h>

typedef struct pf_gpio_line pf_gpio_line;
typedef enum { PF_GPIO_BIAS_NONE = 0, PF_GPIO_BIAS_PULL_UP, PF_GPIO_BIAS_PULL_DOWN } pf_gpio_bias;

int  pf_gpio_open_chip(const char *path);  /* returns fd or <0 */
void pf_gpio_close_chip(int fd);

/* Request an output line. `active_low` maps logical "active" to physical low. The line is driven
 * to `initial_active` immediately. */
pf_gpio_line *pf_gpio_request_output(int chipfd, unsigned offset, bool active_low, bool initial_active, const char *consumer);
pf_gpio_line *pf_gpio_request_input(int chipfd, unsigned offset, bool active_low, pf_gpio_bias bias, const char *consumer);
int  pf_gpio_set(pf_gpio_line *l, bool active);
int  pf_gpio_get(pf_gpio_line *l);         /* 1 active, 0 inactive, <0 error */
void pf_gpio_release(pf_gpio_line *l);
