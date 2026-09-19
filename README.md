# PiFire (C)

A ground-up rewrite of the [PiFire](https://github.com/nebhead/PiFire) pellet grill controller in C: one small daemon (`pifired`, ~600 KB), no Python, a phone-first web UI that a Raspberry Pi Zero 2 W serves without breaking a sweat, pluggable controllers, first-class safety interlocks, a first-boot Wi-Fi hotspot, and a controller that learns your grill across cooks.

* **Control**: Startup / Smoke / Hold / Shutdown / Prime / Manual with the classic P-mode smoke cycle, PID (six variants) or the adaptive learning controller, Smoke+, PWM fan profiles, lid-open detection, cold-start mode for freezing weather.
* **Safety**: over-temperature cut-off, flame-out re-ignite, igniter/auger time caps, probe and controller fault handling, output safe-latch + watchdog, defined relay state after a crash. See `docs/safety.md`.
* **Probes**: ADS1115/1015, MAX31865 RTD, MCP9600 thermocouple, DS18B20, iBBQ/Inkbird and MEATER over Bluetooth, virtual probes; Steinhart–Hart profiles and outlier filtering.
* **Cook**: probe targets with keep-warm/shutdown follow-ups, doneness presets, high/low alarms, timers, ETA, recipes (multi-step programs), pellet manager with hopper level, automatic cook files.
* **Integrations**: MQTT with Home Assistant discovery, JSON webhooks, REST + WebSocket API (`docs/api.md`).
* **Network**: NetworkManager-based Wi-Fi setup with a captive-portal hotspot on first boot, `pifire.local` via Avahi.
* **Learning**: steady-state feed model across set points and ambient temperatures, passive plant identification, relay autotune.

## Build and run on a workstation (simulator)

```
sudo apt install cmake gcc pkg-config libsqlite3-dev libsystemd-dev libmosquitto-dev libcurl4-openssl-dev
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DPF_SANITIZE=ON
cmake --build build -j
ctest --test-dir build
./build/pifired --sim --port 8080 --speed 5      # open http://localhost:8080
```

`--speed N` runs the control loop and the thermal model N× faster. The simulator includes ignition, pellet-pot dynamics, ambient/wind losses, lid-open and food probes, and is what the end-to-end tests drive.

## Install on a Raspberry Pi

Download the latest release archive for your OS (`arm64` for 64-bit, `armhf` for 32-bit), unpack it and run `sudo ./install/install.sh`; or build from source with CMake and run the same script. Updates arrive over the air from GitHub Releases (*More → System → Software*). Full steps, paths and troubleshooting: `docs/install.md`; hardware and wiring: `docs/hardware.md`; publishing releases: `docs/releasing.md`.

## Layout

```
include/pifire/   plugin ABIs: controller, probe, platform, distance, display, notify
src/core/         daemon: state machine, cycle engine, safety, notify/timers, settings, storage, threads
src/hal/          GPIO (kernel uAPI v2), sysfs PWM, I2C, SPI
src/platform/     Raspberry Pi outputs, simulator thermal model
src/probes/       wired and Bluetooth probe drivers, Steinhart-Hart, filtering
src/controllers/  pid, pid_clamping, pid_clamping_percent_pb, pid_ac, pid_sp, pid_parallel, adaptive
src/features/     notifications (mqtt, webhook), pellets, cook files, recipes, learning
src/net/          Wi-Fi / hotspot (nmcli), system info
src/web/          civetweb server, REST/WebSocket API; web/ is the embedded UI
share/            hardware manifest and default settings (embedded in the binary)
install/          systemd unit, installer, udev, board configurator
plugins/          example out-of-tree controller
docs/             api.md, controller-plugins.md, hardware.md, safety.md
```

## License

MIT. Bundled third-party components (civetweb, cJSON, uPlot, Unity) keep their own licenses under `third_party/`.
