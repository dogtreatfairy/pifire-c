# Compiling, installing and updating

## 1. Install from a release (recommended)

On the Pi (Raspberry Pi OS Lite, Bookworm or newer; 64-bit → `arm64`, 32-bit → `armhf`):

```
curl -LO https://github.com/dogtreatfairy/pifire-c/releases/latest/download/pifire-X.Y.Z-arm64.tar.gz
tar -xzf pifire-X.Y.Z-arm64.tar.gz
cd pifire-X.Y.Z-arm64
sudo ./install/install.sh
```

The installer adds the `pifire` service user, the systemd unit (with watchdog), udev/sudoers rules, the captive-portal snippet, enables I2C/SPI and the hardware watchdog, and starts the service. Then open **http://pifire.local/** (or the Pi's IP). If the Pi has no Wi-Fi yet, join the `PiFire-XXXX` hotspot (password `pifire1234`) and the setup page appears. Go to **Settings → Hardware → Grill hardware**, pick your board (e.g. *PiFire Compact PWM PCB*), display and hopper sensor, press *Save hardware* and reboot when asked.

## 2. Compile from source

Build dependencies (Debian/Ubuntu/Raspberry Pi OS):

```
sudo apt install cmake gcc make pkg-config libsqlite3-dev libsystemd-dev libmosquitto-dev libcurl4-openssl-dev
```

On the Pi itself (a Zero 2 W builds it in a few minutes):

```
git clone https://github.com/dogtreatfairy/pifire-c.git
cd pifire-c
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo install/install.sh
```

On a workstation, for development with the simulator:

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DPF_SANITIZE=ON
cmake --build build -j
ctest --test-dir build
./build/pifired --sim --port 8080 --speed 5      # http://localhost:8080
```

Useful CMake options: `-DPF_WITH_MQTT=OFF`, `-DPF_WITH_CURL=OFF` (also disables webhooks and OTA), `-DPF_WITH_BLE=OFF`, `-DPF_BUILD_TESTS=OFF`, `-DPF_VERSION=1.2.3` (version string baked into the binary; releases pass the git tag).

Cross-building Pi binaries on a workstation: `tools/release.sh` runs the same build inside `arm64v8`/`arm32v7` Debian containers (needs podman or docker with QEMU binfmt); `tools/package.sh <arch> <version>` turns a build into the release archive.

## 3. Updating

**Over the air** — *Settings → System → Software updates*. The daemon checks the GitHub Releases of `settings.update.repo` (default `dogtreatfairy/pifire-c`) two minutes after boot and every `update.check_interval_h` hours (`update.auto_check` turns this off); an `UPDATE_AVAILABLE` event is logged when a newer tag exists. *Check for updates* runs it on demand; *Install* downloads `pifire-<ver>-<arch>.tar.gz`, verifies it against the release's `SHA256SUMS`, unpacks it under `/var/lib/pifire/update/stage` and hands it to `pifire-update-apply`, which runs the archive's own `install.sh --upgrade` as a transient systemd unit and restarts `pifired`. Settings, the database and cook files are untouched. The web app reloads itself when the new version is up; the log is in `/var/lib/pifire/update/apply.log`.

**Updating mid-cook.** Off by default; enable *Settings → Software updates → Update while cooking* (`update.hot_update`). Installing while the grill is in Startup, Smoke, Hold or Shutdown is then supported: on its clean stop the daemon writes `/var/lib/pifire/resume.json` (mode, set point, mode timer, probe targets and alerts, cook timer, cook start, controller duty) and the new process re-enters that mode a few seconds later with the same timers, so the countdown and the cook history continue. The relays park OFF for the restart gap (a few seconds without fan and auger), which a pellet fire does not notice. Manual and Prime are not resumed (the updater refuses to start while they run) and a snapshot older than five minutes, or one left by a crash, is ignored: a crash with a hot pit still goes to Shutdown as before. The same handoff happens on a plain `systemctl restart pifired`.

**Manually** — download and unpack a release as in step 1 and run `sudo ./install/install.sh --upgrade`, or rebuild from source and run `sudo install/install.sh` again.

## 3a. Phone notifications, remote access, weather

**Phone notifications** — *Settings → Notifications → Phone notifications*. Two services with iOS/Android apps and a one-call API are built in; the grill posts to them directly, nothing else to host:

* **Pushover** (recommended, $5 once): install the app, copy your *user key* from it, create an application at pushover.net/apps/build and paste its *token*. Priorities are configurable separately for normal events and for alarms (Emergency repeats until acknowledged).
* **ntfy** (free): install the app, subscribe to a private topic on ntfy.sh (or your own server) and enter the topic; add an access token only for protected topics.

**Conditional Notifications** (*Settings → Notifications*) is where your own rules live: pick what
to watch (a class of probes, filtered by role and by wired or Bluetooth, with any of them excluded
by name), describe the condition over its readings, and write the message. A value can be compared
against *another reading of the same probe*, which is how one rule covers every probe at once and
still names the one that matched. Tokens like `{probe}`, `{temp}`, `{target}` and `{eta}` go in the
title and message from a row of chips, and a live preview shows what would be sent against the
current readings, along with how many probes the rule watches and how many match right now.

The Phone Notifications page leads with **Predictive alerts** (how long before a probe reaches its target you want to hear about it), then a collapsible group per service. Each service can receive four categories: *targets & timers* (probe target reached, the **about-N-minutes-to-target** warning, cook timer, recipe steps), *alarms & errors* (probe limits, flame-out, over-temperature), *pellets low* and *system* (autotune/tuning notices). *Send test* checks the credentials immediately. The time-to-target warning fires once per target, after the live estimate has been under `notify.eta_warn_min` (default 15 min) on two consecutive fits.

**Remote access** — *Settings → Network → Remote access*. PiFire joins your [Tailscale](https://tailscale.com) network: *Install Tailscale* adds Tailscale's package repository and installs it, *Connect* joins the tailnet (a sign-in link appears; open it on the phone and approve the machine), and *Enable HTTPS* publishes the web app through `tailscale serve` with a valid certificate. The grill is then reachable from anywhere at `http(s)://<name>.<tailnet>.ts.net/` as long as the phone runs the Tailscale app; add the Home Screen web app from that address so it works at home and away. Privileged steps run through `/usr/local/bin/pifire-tailscale` (sudoers rule installed by `install.sh`); the pifire user is made a Tailscale operator so status and connect need no root.

**Weather** — *Settings → Cooking → Hold Mode → Local Weather*. Enter a country code and postal/ZIP code and the grill looks the location up once (zippopotam.us) and fetches current conditions from Open-Meteo every 15 minutes (no account, no key). The outdoor temperature becomes the ambient reference for the feed-forward model and for every learning observation (an ambient probe still wins), so the fitted `u = a + b·(setpoint − ambient)` learns how the grill behaves at 30 °F versus 100 °F; wind and humidity are shown in the UI and recorded in the status stream.

## 4. Where things live

| path | contents |
|---|---|
| `/usr/local/bin/pifired` | the daemon (`pifired --help`) |
| `/etc/pifire/settings.json` | all settings (JSON; edited by the web UI, safe to back up) |
| `/var/lib/pifire/pifire.db` | history, events, cook files, pellets, learning data (SQLite) |
| `/var/lib/pifire/cookfiles/` | exported cook files |
| `/usr/lib/pifire/{controllers,probes,display}/` | out-of-tree plugins (`.so`) |
| `journalctl -u pifired -f` | live log; also *More → Logs* |

## 5. Uninstall

```
sudo systemctl disable --now pifired
sudo rm -f /usr/local/bin/pifired /usr/local/bin/pifire-boardcfg /usr/local/bin/pifire-update-apply \
  /etc/systemd/system/pifired.service /etc/udev/rules.d/99-pifire.rules /etc/sudoers.d/pifire \
  /etc/NetworkManager/dnsmasq-shared.d/pifire-captive.conf
sudo rm -rf /usr/share/pifire /usr/lib/pifire        # add /etc/pifire /var/lib/pifire to drop settings and data
```

## 6. Troubleshooting

* `systemctl status pifired` / `journalctl -u pifired -n 100` show why the daemon did not start (typical: I2C/SPI not enabled yet → reboot after *Save hardware*).
* The service is `Type=notify` with a 15 s watchdog: a hung control loop is restarted automatically and all outputs are driven off first.
* `pifired --sim -p 8080` runs anywhere without hardware for trying the UI.
