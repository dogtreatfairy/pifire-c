#pragma once
/* Generic JSON webhook: POST every event (or the configured subset) to settings.notify.webhook.url.
 * Delivery runs on its own thread with a small queue and one retry. */
void pf_webhook_init(void);
void pf_webhook_shutdown(void);
