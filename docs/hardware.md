# Hardware

## Boards

The hardware wizard (More → Hardware setup) offers the profiles from `share/manifest.json`: `custom`, PiFire PCB v2.00a, v3.01a, the **Compact PWM PCB** (`pcb_pwm`, James Weber's all-in-one board with PSU, relays and a 12 V DC fan on hardware PWM — oshwlab `pifire-controller-pwm-1.2`), and the v4.x.x modular PCB. A profile fixes the relay/fan/PWM/input pins; *Custom* exposes every pin. Selecting a board writes `settings.platform` and adopts the board's default probe map (ADS1115 at 0x48, PT-1000 pit probe + Thermoworks-profile food probes). *Save hardware* then calls `POST /admin/boardcfg`, which runs `pifire-boardcfg` (via the sudoers rule from `install.sh`) to write the relay pull-downs/ups, the `dtoverlay=pwm,pin=13,func=4` overlay for a DC fan, 1-Wire, I2C and SPI into `config.txt`; the wizard offers a reboot when anything changed.

The Compact PWM PCB's fan amplifier inverts the PWM signal, so the daemon drives `duty = 100 − fan%` at 25 kHz (`pwm.frequency`), exactly like the Python `raspberry_pi_all` platform. Relays are active-high on that board (`triggerlevel: HIGH`), so its GPIOs are parked with pull-downs.

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

## Display

`ili9341e` (TFT + KY-040 rotary encoder) and `ili9341` (view only) drive a 320×240 SPI panel on SPI0 CE0/CE1 with the DC/RST/LED pins from the board profile. The panel renders anti-aliased text in Inter (Regular/SemiBold, embedded as a ~10 KB subset each, via stb_truetype) at the full 320×240: the primary probe is the hero temperature with the set point or mode statement beneath it, the top bar shows the mode and the most urgent clock (time left in Startup/Reignite/Shutdown/Prime, else the cook timer if one runs, else cook time), and up to three enabled food probes get cards that turn green when their target is hit; the outputs row sits at the bottom. Probe names come from the web probe settings and are shown as entered. `display.theme` is the panel's own *dark* / *light* choice (light = black on white for direct sunlight) and is independent of the web theme. The encoder menu is mode-aware — Stop: Start → Smoke / Start → Hold / Monitor; Smoke: Hold, Smoke+ on/off, Shutdown, Stop; Hold: Set point, Smoke, Shutdown, Stop — and a long press (≥1.5 s) is an emergency stop from any screen.

### Bluetooth probes

`ibbq` (Inkbird / iBBQ 4- and 6-probe), `meater` (MEATER Original/Plus and Pro) connect over GATT. `chefiq` (Chef iQ CQ50 / CQ60) is **passive**: the probe broadcasts its readings in manufacturer-specific advertisements (company id 0x05CD) and the daemon keeps LE discovery running to read them — no pairing, no base station, and the phone app can stay connected at the same time. Pair it from Settings → Probes → Add probe → *Pair BT Chef iQ*: the scan lists it as `CQ60`; leaving the address empty adopts the first Chef iQ found. Ports: `BT_Food` (the probe reading the app calls "food"), `BT_Ambient` (the handle sensor). Battery arrives in the probe's status packet. Wire format after the `chefiq-ble` library used by Home Assistant (V3 layout verified on a CQ60; V2/legacy layouts implemented as documented but unverified).

**Signal strength.** Every Bluetooth probe reports its link quality: the pairing list, the probe table, the Home probe cells and the grill display show a Bluetooth icon with 0–4 bars (4 ≥ −60 dBm, 3 ≥ −70, 2 ≥ −80, 1 below; none while the probe is out of reach), and the probe popup shows the raw dBm and battery. Passive probes (Chef iQ) are measured from their advertisements. Connected probes (iBBQ, MEATER) stop advertising, so the daemon reads the live connection RSSI over a raw HCI socket; that needs `CAP_NET_RAW`, which `pifired.service` grants. Without it the bars stay at the value seen while pairing.

**Time to target.** A food probe with a target shows an estimate of when it will get there, fitted from the last 20 minutes of readings with recent samples weighted most (the Python PiFire estimator), refreshed every 20 s and blended with the previous estimate so it counts down smoothly. It disappears while the temperature is flat (a stall) and returns once it climbs again.

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
