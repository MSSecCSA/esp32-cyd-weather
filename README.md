# CYD Weather Station

A multi-city weather display for the **ESP32-2432S028R** — the ~$15 board everyone calls the
"Cheap Yellow Display". It rotates through cities, shows live conditions from
[Open-Meteo](https://open-meteo.com/) (no API key needed), and has a second full-screen
clock view. WiFi is configured on the device itself; nothing is compiled in.

```
┌──────────────────────────────────────┐   ┌──────────────────────────────────────┐
│ WEATHER            07:42 PM  Sep 18  │   │                                      │
│──────────────────────────────────────│   │          07:42  PM                   │
│         │              72F           │   │                                      │
│   ☀     │         Partly cloudy      │   │      Thursday, Sep 18                │
│         │         New York, NY       │   │ ──────────────────────────────────── │
│         │           02:42 PM         │   │  ☀   72F  Partly cloudy              │
│──────────────────────────────────────│   │      New York, NY      Auto 10s   ●  │
│ Humidity: 64%   Wind: 8mph  Auto 10s●│   │                                      │
└──────────────────────────────────────┘   └──────────────────────────────────────┘
        weather view                              clock view (swipe up)
```

## Why this repo might be useful even if you want a different app

Most of the work here was spent finding out what this board *actually does*, as opposed to
what the community pinouts say. Several things turned out to be wrong in ways that fail
silently. If you own a CYD, these will probably cost you an evening too:

| Finding | Detail |
|---|---|
| **The touch controller is not on the VSPI defaults** | It has its own pins: `SCK=25, MISO=39, MOSI=32, CS=33, IRQ=36`. `XPT2046_Touchscreen::begin()` calls a bare `SPI.begin()`, which claims 18/19/23 — where the chip is not. Reads then return all-ones (`x=8191`) and a typical sanity check discards them, so touch looks *unreliable* rather than broken. It had never worked once here. |
| **The light sensor needs 0 dB ADC attenuation** | R21's entire signal sits between ~142 mV and ~740 mV. Arduino's default 11 dB scales the ADC to ~2.5 V full-scale and throws most of that away, so `analogRead()` floors to 0 and the sensor looks dead. At 0 dB it reads 2500 counts covered vs 746 at 11 dB. |
| **`setenv("TZ", …)` leaks 24 bytes — but only on length change** | Measured: same value 0 B/call, *different value of the same length* 0 B/call, different length 24 B/call, and `tzset()` is not involved. Rotating timezone strings of differing lengths drained ~18 B/s and forced a reboot every 3.5 hours. |
| **microSD does *not* share a bus with touch** | It is on VSPI (18/19/23 + CS 5) while touch has its own pins. They only appear to conflict if the touch driver is squatting on VSPI — which is the bug above. |
| **LEDC channels 0 and 1 share a timer** | `ledcSetup()` assigns timer `(channel/2)%4`. Putting a 12-bit channel on 1 silently re-resolutions the 8-bit backlight on channel 0 and drives the panel nearly black. |

Every hardware claim in [`CLAUDE.md`](CLAUDE.md) is labelled **VERIFIED**, **ASSUMPTION** or
**UNVERIFIED** according to how it is actually known. `tools/hwtest/` is the serial-driven
rig that produced them, and it is in the repo so you can re-run any of it on your own board.

## Features

- **Multi-city rotation** — fetches current conditions plus sunrise/sunset, switches every 10s
- **Two views** — detailed weather, or a 70/30 big-clock view; swipe vertically to toggle
- **On-device WiFi setup** — no credentials in the firmware. A fresh board raises a
  `CYD-Setup-XXXX` access point, serves a config page, validates the network, and stores it
  in NVS. If the saved network disappears it retries every 60s, so a rebooting router
  recovers unattended
- **Per-city day/night theming** — a drawn moon and a tinted sky panel after local sunset
- **Ambient RGB LED** — slow gamma-corrected fade through red, green, blue, white and back,
  driven from its own FreeRTOS task so the animation never competes with the UI loop
- **Adaptive backlight** — ambient light plus a night ceiling, smoothed so a passing shadow
  does not flash the panel
- **Touch gestures** — tap, horizontal swipe, vertical swipe, long-press, double-tap, all in
  calibrated screen pixels rather than raw ADC counts
- **Diagnostics overlay** — double-tap for heap, RSSI, IP, uptime, flash usage

## Hardware

| | |
|---|---|
| Board | ESP32-2432S028R ("CYD"), 2.8" 320×240 ILI9341 + XPT2046 resistive touch |
| Chip | ESP32-D0WD-V3, dual core 240 MHz, 4 MB flash, **no PSRAM** |
| Cost | Roughly $12–18 |

Nothing else is required. The microSD slot, speaker amplifier and RGB LED are already on the
board; the LED is used, the other two are not yet.

## Getting started

```sh
git clone https://github.com/MSSecCSA/esp32-cyd-weather.git
cd esp32-cyd-weather
pio run --target upload          # or: python3 -m platformio run --target upload
pio device monitor               # 115200 baud
```

There are **no credentials to fill in**. On first boot the display shows:

```
        WiFi Setup
1. Join this WiFi network:
        CYD-Setup-A1B2
        (no password)
2. Open this address:
      http://192.168.4.1
```

Join it from a phone, pick your network, enter the password. It validates the connection
before storing it, then restarts into the weather screen. Credentials live in NVS — they
survive reflashing, and there is nothing in the binary to leak.

> The setup AP is deliberately **open**, which is a trade-off rather than an oversight:
> easier to join, but during that window anyone in range could observe the password you
> submit, since it crosses that link over plain HTTP. Set `AP_PASSWORD` if you would rather
> not.

### Choose your own cities

Easiest way — an interactive script that does the lookups for you and then flashes:

```sh
python3 tools/set_cities.py
```

```
  City, US ZIP, or 'City, Country'  (blank when finished): 28202
    looking up ZIP 28202...
    -> Charlotte, North Carolina, US
    checking weather availability...
    ok: 77F right now, timezone America/New_York
    tz: EST5EDT,M3.2.0,M11.1.0  (22/31 chars)
    display name [Charlotte, NC]:
    added: Charlotte, NC   (1 so far)
```

It accepts a **city name**, a **US ZIP**, or **"City, Country"**; disambiguates when there
are several matches (there are eight places called Tokyo); asks for a nearby major city if
nothing is found; confirms Open-Meteo actually serves each point before accepting it;
checks the name fits the display column and the TZ string fits the firmware's limit; then
rewrites `cities[]` and offers to upload. Your previous list is saved as `main.cpp.bak`.

Stdlib only — nothing to install. Flags: `--port` to pick the serial port, `--no-flash` to
write the file without uploading.

**Why a script rather than "just edit the array":** the firmware needs a **POSIX** TZ string
(`EST5EDT,M3.2.0,M11.1.0`), while every geocoder returns an **IANA** name
(`America/New_York`). The script reads the POSIX rule out of your own system's TZif files —
each one ends with it as its last line — so the result matches what the C library would use,
with no mapping table to drift out of date. Getting a DST rule subtly wrong produces a
plausible-looking but incorrect clock, which is exactly the kind of bug that survives a long
time unnoticed.

To edit by hand instead, the array is between the `CITIES:BEGIN` / `CITIES:END` markers in
`src/main.cpp`, and `HOME_TZ` just above it sets your own local zone:

```cpp
City cities[] = {
  {40.7128, -74.0060, "New York, NY", "EST5EDT,M3.2.0,M11.1.0"},
  //  lat      lon      display name    POSIX TZ string
};
```

`checkTimezoneLengths()` and `logLayoutMetrics()` both report on the serial log at boot if a
TZ string or a city name is too long.

## Testing your own board

```sh
pio run -d tools/hwtest --target upload
python3 tools/hwtest/drive.py l        # RGB LED cycle
python3 tools/hwtest/drive.py t        # 5-point touch calibration
python3 tools/hwtest/drive.py a        # ADC probe
```

Commands cover the LED, BOOT button, light sensor at both backlight states, an ADC probe,
an I²C scan, microSD, the speaker and touch calibration — which prints paste-ready
constants. See [`tools/hwtest/README.md`](tools/hwtest/README.md).

`tools/wifi_soak.py` runs a long unattended heap-and-connectivity check.

## Expanding it

[`docs/peripheral-options.html`](docs/peripheral-options.html) is a study of what can
physically be attached. Short version: **three GPIOs are free** (22, 27, 35 — the last
input-only), but 27 and 22 are SDA/SCL on the CN1 header, and I²C is a bus, so the practical
ceiling is addresses rather than pins.

## Contributing

Issues and forks are both welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). Reports that your
board differs from what is documented are especially useful, because CYD revisions genuinely
vary.

## Licence

[Apache License 2.0](LICENSE). You may use, modify and redistribute this, including
commercially. In return the licence asks that you keep the [`NOTICE`](NOTICE) file, retain
the copyright and licence notices, and state significant changes you make.

If you fork it, a credit back to
[MSSecCSA/esp32-cyd-weather](https://github.com/MSSecCSA/esp32-cyd-weather) is appreciated —
much of what is here was established by measurement rather than copied from documentation,
and it is easier for the next person to find if the trail leads back.
