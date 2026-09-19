<div align="center">

# 🌤️ CYD Weather Station

**A multi-city weather display for the $15 "Cheap Yellow Display"**

Live conditions from around the world, a big bedside clock, and WiFi you set up
on the device itself — no credentials baked into the firmware.

[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-ESP32-black.svg?logo=espressif&logoColor=white)](https://www.espressif.com/)
[![Board](https://img.shields.io/badge/board-ESP32--2432S028R-e8b93f.svg)](https://randomnerdtutorials.com/cheap-yellow-display-esp32-2432s028r/)
[![Built with](https://img.shields.io/badge/built%20with-PlatformIO-orange.svg?logo=platformio&logoColor=white)](https://platformio.org/)
[![Issues welcome](https://img.shields.io/badge/issues-welcome-brightgreen.svg)](../../issues)

</div>

---

```
   ┌────────────────────────────────────────┐      ┌────────────────────────────────────────┐
   │  WEATHER              07:42 PM  Sep 18 │      │                                        │
   ├────────────────────────────────────────┤      │            07:42  PM                   │
   │            │              72F          │      │                                        │
   │     ☀      │        Partly cloudy      │      │        Thursday, Sep 18                │
   │            │        New York, NY       │      │  ────────────────────────────────────  │
   │            │          02:42 PM         │      │   ☀   72F  Partly cloudy               │
   ├────────────────────────────────────────┤      │       New York, NY     Auto 10s     ●  │
   │ Humidity: 64%  Wind: 8mph   Auto 10s ● │      │                                        │
   └────────────────────────────────────────┘      └────────────────────────────────────────┘
                 weather view                          clock view  ·  swipe up to switch
```

<div align="center">

### ✨ What it does

</div>

|   |   |
|---|---|
| 🌍 | **Rotates through your cities** — live temperature, conditions, humidity and wind, every 10 seconds |
| 🕐 | **Two screens** — detailed weather, or a big glanceable clock. Swipe up to switch |
| 📶 | **Sets up its own WiFi** — first boot raises an access point and asks. Nothing compiled in |
| 🌙 | **Knows day from night** — per-city, so Tokyo shows a moon while New York shows sun |
| 🎨 | **Slow ambient colour fade** on the onboard LED, dimming with the room |
| 💡 | **Adapts to the light** — brightens in daylight, settles down at night |
| 👆 | **Touch gestures** — tap, swipe, long-press, double-tap for diagnostics |

---

## 🚀 Get it running

```sh
git clone https://github.com/MSSecCSA/esp32-cyd-weather.git
cd esp32-cyd-weather
pio run --target upload
```

That's it — **there's nothing to configure first.** On first boot the screen shows:

```
                WiFi Setup

        1. Join this WiFi network:
              CYD-Setup-A1B2
                (no password)

        2. Open this address:
              http://192.168.4.1
```

Join it from your phone, pick your network from the list, type the password. It tests the
connection before saving, then restarts into the weather screen.

> 💡 Credentials are stored on the device, not in the firmware. They survive reflashing, and
> there's nothing in the binary to leak.

> ⚠️ The setup network is **open** so it's easy to join. During that short window someone
> nearby could see the password you type. Set `AP_PASSWORD` in `src/main.cpp` if you'd
> rather not.

---

## 🗺️ Pick your cities

Run the helper and answer the prompts — it does the lookups and flashes for you:

```sh
python3 tools/set_cities.py
```

```
  City, US ZIP, or 'City, Country': 28202
    looking up ZIP 28202...
    → Charlotte, North Carolina, US
    checking weather availability...
    ok: 77F right now, timezone America/New_York
    display name [Charlotte, NC]:
    added ✓  (1 so far)
```

Takes a **city name**, a **US ZIP code**, or **"City, Country"**. If there are several
matches it asks which one. If it can't find the place, it asks for a nearby major city.
Then it checks each location really has weather data, works out the timezone, and offers to
upload.

No installs needed — it's plain Python.

<details>
<summary><b>Prefer to edit the file yourself?</b></summary>

<br>

The list lives between the `CITIES:BEGIN` / `CITIES:END` markers in `src/main.cpp`:

```cpp
City cities[] = {
  {40.7128, -74.0060, "New York, NY", "EST5EDT,M3.2.0,M11.1.0"},
  //  lat      lon      display name    POSIX TZ string
};
```

Set `HOME_TZ` just above it to your own zone. Note the timezone is a **POSIX** string, not
an IANA name — `"America/New_York"` won't work here, which is the main reason the script
exists. The board reports on serial at boot if a name or timezone string is too long.

</details>

---

## 🛠️ What you need

| | |
|---|---|
| **Board** | ESP32-2432S028R — the "Cheap Yellow Display", ~$12–18 |
| **Screen** | 2.8" 320×240 colour touchscreen, already attached |
| **Anything else** | Nothing. A USB cable |

The microSD slot, speaker and RGB LED are already on the board.

---

## 🔬 The interesting part

Most of the work here went into finding out what this board *actually does*, rather than
what the community pinouts say. A few things turned out to be wrong in ways that **fail
silently** — they compile, they run, they look nearly fine:

- The **touch controller isn't where the library expects it**, so touch looks flaky when it's
  actually dead
- The **light sensor reads as broken** unless you change one ADC setting
- **`setenv("TZ", …)` leaks memory** — but only when the string's *length* changes

📖 **[Read the hardware findings →](docs/hardware-findings.md)**

If you just want a different app on the same board, that page is probably the most useful
thing in this repo.

<details>
<summary><b>Also in here</b></summary>

<br>

- **[`tools/hwtest/`](tools/hwtest/)** — a serial-driven test rig for the LED, button, light
  sensor, I²C, microSD, speaker and touch calibration. Re-run any finding on your own board
- **[`docs/peripheral-options.html`](docs/peripheral-options.html)** — what else you can
  physically plug in, with a pin-by-pin map and shopping links
- **[`CLAUDE.md`](CLAUDE.md)** — the full engineering notes. Every hardware claim is labelled
  VERIFIED, ASSUMPTION or UNVERIFIED by how it's actually known
- **[`tools/wifi_soak.py`](tools/wifi_soak.py)** — long unattended heap and connectivity check

</details>

---

## 🤝 Fork it, break it, tell me about it

Issues and forks are both very welcome — see **[CONTRIBUTING.md](CONTRIBUTING.md)**.

Especially useful: **"my board does something different."** CYD revisions genuinely vary —
different amplifier chips, different USB connectors, light sensors that behave differently.
Those reports are data, not noise.

Built something on top of this? Open an issue and say so — I'd like to link to it.

---

<div align="center">

### 📄 Licence

**[Apache 2.0](LICENSE)** — use it, change it, sell it. Just keep the [`NOTICE`](NOTICE) file
and say what you changed.

If you fork it, a credit back to this repo is appreciated. Much of what's here was worked out
by measuring real hardware rather than copying documentation, and it's easier for the next
person to find if the trail leads back.

<sub>Built for the ESP32-2432S028R · Weather by <a href="https://open-meteo.com/">Open-Meteo</a></sub>

</div>
