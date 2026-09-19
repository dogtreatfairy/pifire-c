# Hardware

## Boards

The hardware wizard (More → Hardware setup) offers the profiles from `share/manifest.json`: `custom`, PiFire PCB v2.00a, v3.01a, PWM board, and the v4.x.x modular PCB. A profile fixes the relay/fan/PWM/input pins; *Custom* exposes every pin. Selecting a board writes `settings.platform`; press *Save hardware* and reboot if the wizard says so (I2C/SPI/PWM/1-Wire overlays are applied by `pifire-boardcfg`).

| output | v4.x.x | v3.01a / v2.00a | PWM board |
|---|---|---|---|
| auger | 23 | 14 | 14 |
| igniter | 18 | 18 | 18 |
| power | 22 | 4 | 4 |
| fan / DC fan gate | 26 | 15 | 15 / 26 |
| PWM | 13 | – | 13 |
| shutdown button | 17 | – | – |
| 1-Wire | 4 | configurable | – |

Relay trigger level (`platform.triggerlevel`) is active-low on most relay boards.

## Probe devices

| module | bus | ports | notes |
|---|---|---|---|
| `ads1x15` | I2C 0x48–0x4B | ADC0–ADC3 | ADS1115 or ADS1015; per-port resistor divider and reference voltage |
| `max31865` | SPI CE0/CE1 | RTD0 | PT-100/PT-1000, 2/3/4 wire; reports resistance |
| `mcp9600` | I2C 0x60–0x67 | KTT0 | K/J/T/… thermocouple |
| `ds18b20` | 1-Wire | DS0 | first sensor found or a specific ID |
| `ibbq` | Bluetooth | BT0–BT5 | iBBQ / xBBQ / Inkbird, 4 or 6 probes; use *Scan* in the wizard to pin an address |
| `meater` | Bluetooth | BT_Tip, BT_Ambient | MEATER Original and Pro (the block/base station is skipped) |
| `virtual` | – | VIRT0 | average / highest / lowest / median of other probes |
| `sim` | – | ADC0–ADC3 | simulator |

Thermistor profiles (Steinhart–Hart A/B/C) live under Settings → Probes; the built-in set matches the original PiFire list. `PT-1000-*` profiles are for the Traeger-style RTD grill probe.

## Distance sensors (hopper level)

`hcsr04` (trigger/echo GPIOs, timing from kernel GPIO edge timestamps) or none. Calibrate `pelletlevel.empty` / `full` in centimetres.

## Ambient reference

Tag an *Aux* probe as *Ambient reference* (Probes page) — for example a DS18B20 outside the barrel or the MEATER ambient port — and the learning model and cold-start baseline will use it. Without one, the pit temperature at startup is used as the ambient estimate.

## Raspberry Pi setup

Fresh Raspberry Pi OS Lite (Bookworm or Trixie, 64-bit recommended):

```
sudo apt install cmake gcc libsqlite3-dev libmosquitto-dev libcurl4-openssl-dev libsystemd-dev pkg-config git
git clone <repo> pifire-c && cd pifire-c
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
sudo install/install.sh
```

The installer creates the `pifire` service user (groups gpio/i2c/spi/dialout/netdev/bluetooth), installs the systemd unit with watchdog, udev rules for PWM, the captive-portal dnsmasq snippet, enables I2C/SPI and the hardware watchdog, and starts the service. On first boot without a known Wi-Fi network the grill starts the **PiFire-XXXX** hotspot (password `pifire1234`); join it and follow the setup page.
