# HTTP API

Base path `/api/v1`. All bodies and responses are JSON. Temperatures are in the configured units (`globals.units`). Errors: `{"result":"ERROR","message":"..."}` with a 4xx/5xx status.

## Live data

| method | path | notes |
|---|---|---|
| GET | `/status` | full state: mode, set point, outputs, probes (temp/target/eta/limits), timer, recipe, safety, controller debug, cycle (`u_raw`, `u_applied`, `u_ff`), autotune. `timers.mode_remaining` counts down Startup/Reignite/Shutdown/Prime; `timers.startup_exit_temp` (0 = none) is the temperature that ends startup early; `coldstart.{active,reached,remaining}` show the cold-start gate |
| WS | `/ws` | pushes `{"type":"status",...}` at 1 Hz and on change, `{"type":"event",...}` for alerts; accepts the same JSON commands as `POST /cmd` |
| GET | `/history?minutes=15` or `?from=&to=&res=` | `{t:[],mode:[],setpoint:[],u:[],probes:{label:{temp:[],target:[]}}}` |
| POST | `/history/clear` | |
| GET | `/events?limit=100` | stored event log (newest first) |
| GET | `/alerts?limit=20` | recent alerts (ring, oldest first) |
| GET | `/logs?limit=300` | daemon log ring |
| GET | `/system` | version, uptime, CPU temp, throttling, memory, Wi-Fi quality, interfaces |

## Commands (`POST /cmd`, body `{"cmd": ..., ...}`)

| cmd | fields |
|---|---|
| `mode` | `mode` (Stop, Monitor, Startup, Smoke, Hold, Shutdown, Manual, Prime), `setpoint` |
| `setpoint` | `setpoint` |
| `stop` | e-stop, always honoured |
| `smoke_plus` / `pwm_control` | `enabled` |
| `duty_cycle` | `duty_cycle` (fan %) |
| `manual` | `output` (power/fan/auger/igniter/pwm), `on`, `pct` |
| `lid` | toggle lid-open pause |
| `prime` | `amount` (g), `next` ("Startup" or "") |
| `clear_error` | |
| `target` | `label`, `target` (0 clears), `after` (0 notify, 1 keep warm, 2 shutdown) |
| `limits` | `label`, `high`, `low` (0 = off) |
| `timer` | `op` start/pause/resume/cancel, `seconds`, `after` |
| `test_notify` | |
| `recipe` | `op` start/next/stop, `id` |
| `autotune` | `start` true/false |
| `apply_tuning` | |

Convenience REST aliases exist: `POST /mode`, `/setpoint`, `/stop`, `/smoke_plus`, `/pwm_control`, `/manual`, `/lid`, `/prime`, `/clear_error`.

## Settings

`GET /settings`, `GET /settings/<group>[/<key>...]`, `PATCH /settings[/<group>]` (deep merge, validated, saved atomically; changing `globals.units` converts every temperature setting). Groups: `globals, platform, modules, cycle_data, controller, safety, startup, shutdown, keep_warm, smoke_plus, pwm, pelletlevel, probe_settings, notify, network, learning, history, web`.

## Hardware, probes, controllers

| method | path |
|---|---|
| GET | `/manifest` — board profiles, probe device schemas (the hardware wizard) |
| GET | `/controllers` — list with option schemas and recommendations |
| GET | `/probes/devices` — per-device status (connected, battery, address) |
| POST | `/probes/tune` | body `{"points":[{"temp":T,"ohms":R},×3]}` in user units → Steinhart-Hart `{A,B,C,check:[T1,T2,T3]}` for a new probe profile (the web Probes page captures live resistance) |
| POST | `/probes/ble/scan?seconds=8` — Bluetooth devices in range |
| GET | `/learning`, POST `/learning/forget` (clear what the grill taught itself), POST `/tune/clear` (throw the measured tuning away and go back to the typed values) |

## Pellets, cook files, recipes

| method | path |
|---|---|
| GET | `/pellets`; POST `/pellets/profile`, `/pellets/load {id}`, `/pellets/delete {id}`, `/pellets/check`, `/pellets/calibrate {as:"full"\|"empty"}` |
| GET | `/cookfiles`; GET `/cookfiles/<id>`; POST `/cookfiles/<id>/rename {name}`, `/cookfiles/<id>/delete` |
| GET | `/recipes`; POST `/recipes` (save `{id?,name,steps:[...]}`); POST `/recipes/<id>/delete` |

Recipe step: `{"mode":"Startup|Smoke|Hold|Shutdown","setpoint":225,"s_plus":false,"timer_min":0,"probe":"Probe1","probe_temp":0,"pause":false,"message":""}`.

## Network

`GET /network/status`, `GET /network/scan?rescan=1`, `GET /network/saved`, `POST /network/connect {ssid,psk}`, `POST /network/forget {ssid}`, `POST /network/hotspot {on}`.

## Admin

`GET /update` — OTA state: `{current, arch, repo, latest, available, installable, asset, notes, html_url, state, message, progress, checked_at, busy}`. `POST /update/check` queries GitHub Releases of `settings.update.repo` in the background; `POST /update/install` (grill stopped, not in the simulator) downloads the matching `pifire-<ver>-<arch>.tar.gz`, verifies it against `SHA256SUMS`, and reinstalls via `pifire-update-apply`.


`GET /backup` — `{busy, message, error, last: {ts, name, size, ok, results: {<loc id>: {ok, message}}}, next_ts, locations: [{id, type, name, enabled, connected?}], pending?: {loc, user_code, url, expires}, smbclient}`. Locations live in `settings.backup.locations`: `[{id, type: gdrive|onedrive|smb|folder, name, enabled, ...}]` with `client_id`/`client_secret`/`cloud_folder` for Google Drive, `client_id` for OneDrive, `host`/`share`/`path`/`user`/`password` for a share (through smbclient), `folder` for a path on the grill. Every backup goes to every enabled location. `POST /backup/run` makes one now: a `.tar.gz` of `settings.json`, a snapshot of the database with the rolling chart history left out, and every cook file, named `pifire-<grill>-YYYYMMDD-HHMM.tar.gz`, pruned at each location to `settings.backup.keep`. `GET /backup/list` — `{files: [{name, size, ts, locations: [ids]}], warning?}` merged across locations, newest first. `POST /backup/test {id}` — `{ok, message}`. `POST /backup/restore` with `{name, loc?}` (fetched from that location, or the nearest that has it) or the archive itself as the body (`Content-Type: application/gzip`, up to 64 MB): the grill must be stopped; the archive is checked, staged, and put in place by the restart that follows. `POST /backup/browse {type: folder|smb, path, host?, share?, user?, password?}` — `{path, dirs, parent?}` for a folder on the grill or inside a share, or `{shares}` for a host when no share is named, so a location can be picked rather than typed. `POST /backup/connect {id}` starts a device sign-in for a cloud location (Google: `drive.file`; Microsoft: `Files.ReadWrite.AppFolder`) using the project's own clients from `share/oauth-clients.json` (see docs/oauth-clients.md), or a location's own `client_id`/`client_secret` when set; `GET /backup` says which built-in clients exist in `clients`, `POST /backup/disconnect {id}` forgets its token. The schedule (`settings.backup.schedule` never/daily/weekly/monthly, `hour`, `weekday`, `monthday`) runs from the services thread; a slot missed while the grill was off runs when it comes back, and a run that failed anywhere is retried an hour later.

`GET /recipes/export` — `{app, kind: "recipes", format, created, recipes: [...]}`; `POST /recipes/import` with that file (or a bare array) — a recipe with the same name replaces it, the rest are added; `{imported, replaced}`. `GET /rules/export` — `{app, kind: "notifications", format, created, units, rules: [...]}`; `POST /rules/import` — converted to the grill's unit, same id replaces, the rest added; `{imported, replaced}`. `GET /tune/export` / `POST /tune/import` do the same for the tuning library.

`POST /admin/reboot`, `POST /admin/poweroff` (refused unless the grill is stopped).

`POST /admin/boardcfg` — applies the boot configuration for `settings.platform` by running `pifire-boardcfg` (relay pulls, PWM overlay for a DC fan, 1-Wire, I2C, SPI, hardware watchdog). Returns `{"reboot": true|false, "output": "..."}`; not available in the simulator.

## MQTT

With `notify.mqtt.enabled`, the daemon publishes under `<id>/`: `control`, `devices`, `probe_data_primary`, `probe_data_food`, `pid`, `pellet`, `system`, `notify_event`, `availability`, and listens for the `/cmd` JSON on `<id>/cmd`. Home Assistant discovery is published under `<homeassistant_autodiscovery_topic>/…`.

## Webhook

With `notify.webhook.enabled`, every event (or those listed in `events`) is POSTed as `{"event","title","body","ts","grill","value1","value2","value3","status":{...}}`.
