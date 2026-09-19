# Compiling, installing and updating

## 1. Install from a release (recommended)

On the Pi (Raspberry Pi OS Lite, Bookworm or newer; 64-bit → `arm64`, 32-bit → `armhf`):

```
curl -LO https://github.com/dogtreatfairy/pifire-c/releases/latest/download/pifire-X.Y.Z-arm64.tar.gz
tar -xzf pifire-X.Y.Z-arm64.tar.gz
cd pifire-X.Y.Z-arm64
sudo ./install/install.sh
```

The installer adds the `pifire` service user, the systemd unit (with watchdog), udev/sudoers rules, the captive-portal snippet, enables I2C/SPI and the hardware watchdog, and starts the service. Then open **http://pifire.local/** (or the Pi's IP). If the Pi has no Wi-Fi yet, join the `PiFire-XXXX` hotspot (password `pifire1234`) and the setup page appears. Go to **More → Hardware setup**, pick your board (e.g. *PiFire Compact PWM PCB*), display and hopper sensor, press *Save hardware* and reboot when asked.

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

**Over the air** — *More → System → Software*. The daemon checks the GitHub Releases of `settings.update.repo` (default `dogtreatfairy/pifire-c`) two minutes after boot and every `update.check_interval_h` hours (`update.auto_check` turns this off); an `UPDATE_AVAILABLE` event is logged when a newer tag exists. *Check for updates* runs it on demand; *Install* (grill must be stopped) downloads `pifire-<ver>-<arch>.tar.gz`, verifies it against the release's `SHA256SUMS`, unpacks it under `/var/lib/pifire/update/stage` and hands it to `pifire-update-apply`, which runs the archive's own `install.sh --upgrade` as a transient systemd unit and restarts `pifired`. Settings, the database and cook files are untouched. The web app reloads itself when the new version is up; the log is in `/var/lib/pifire/update/apply.log`.

**Manually** — download and unpack a release as in step 1 and run `sudo ./install/install.sh --upgrade`, or rebuild from source and run `sudo install/install.sh` again.

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
