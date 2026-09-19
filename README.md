# PiFire (C)

A ground-up rewrite of the PiFire pellet grill controller in C: one small daemon (`pifired`),
no Python, a phone-first web UI, pluggable controllers, first-class safety interlocks, and a
first-boot Wi-Fi hotspot. Targets Raspberry Pi OS Lite (Bookworm/Trixie) on a Pi Zero 2W or better.

## Build (workstation, simulator)

```
sudo apt install cmake libsqlite3-dev libsystemd-dev libmosquitto-dev libcurl4-openssl-dev
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DPF_SANITIZE=ON
cmake --build build -j
ctest --test-dir build
./build/pifired --sim --port 8080
```

## Layout

- `include/pifire/` public plugin ABIs (controller, probe, platform, display, distance, notify)
- `src/core/` daemon core: state machine, cycle engine, safety, settings, storage
- `src/hal/` GPIO (kernel uAPI v2), PWM (sysfs), I2C, SPI, 1-Wire
- `src/probes/` wired and Bluetooth probe devices
- `src/controllers/` built-in controllers
- `src/web/` HTTP/WebSocket server and REST API; `web/` the UI assets embedded into the binary
- `share/` hardware manifest and default settings (embedded)
- `install/` systemd unit, udev rules, installer

## License

MIT. Third-party components keep their own licenses (see `third_party/*/LICENSE*`).
