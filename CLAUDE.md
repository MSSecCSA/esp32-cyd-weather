# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Firmware for a "Cheap Yellow Display" (CYD) ESP32 board that shows current weather for a rotating list of cities. Built with PlatformIO + Arduino framework. The entire application is a single file: `src/main.cpp`.

## Commands

This is a PlatformIO project (`platformio.ini`), not npm/make. Standard workflow:

- Build: `pio run`
- Build and flash to the board: `pio run --target upload`
- Open serial monitor (115200 baud): `pio device monitor`
- Flash then immediately monitor: `pio run --target upload --target monitor`
- Clean build artifacts: `pio run --target clean`

If `pio` isn't on PATH, it may still be installed as a Python package — check with `python3 -m platformio run` before assuming it's missing.

There is no test suite or linter in this repo — it's single-target embedded firmware (`env:esp32dev`), verified by building/flashing and watching serial output.

## Secrets

WiFi credentials live in `include/secrets.h` (gitignored, real values) with `include/secrets.h.example` checked in as the template. `main.cpp` does `#include "secrets.h"` and expects it to `#define WIFI_SSID` / `WIFI_PASS`. When setting up a fresh checkout, copy the example file and fill in real values before building — the build fails without it. Never edit `secrets.h` in a way that would land credentials back in tracked history (e.g. don't `git add -f` it).

## Hardware: evidence ledger

This project follows the `esp32-cyd-engineering` skill's rule that board facts are **not** proven by matching a community pin map. Each fact below is labelled by how it is actually known. Re-verify with the commands shown rather than trusting this table.

### VERIFIED — measured on the connected device (2026-09-18)

Hardware validation pass, driven by a serial-command test rig kept out of `src/`:

| Fact | Result | How |
|---|---|---|
| USB-serial bridge | **CH340** (VID `0x1A86`, PID `0x7523`), one device | `ioreg -p IOUSB`; both USB ports feed this one chip |
| BOOT button (GPIO 0) | **Works** — idle 1, pressed 0, clean transitions | 3 presses captured with no bounce artifacts |
| Touch controller | **Works, on SCK=25/MISO=39/MOSI=32/CS=33** | Raw coords track a finger after re-pinning; saturated all-ones before |
| Touch calibration | **Done.** 5-point fit, worst residual **6.3 px**, axes aligned, neither inverted | `TOUCH_AX/BX/AY/BY` in `main.cpp` |
| Gestures end-to-end | **All confirmed by use (2026-09-18):** tap (next city), vertical swipe (view toggle), long-press (pause). | Exercised on the physical unit after calibration |
| Clock view under auto-rotate | **Confirmed.** Strip cycles; the clock is no longer overpainted | Watched through auto-switch cycles on the unit |
| Light sensor (GPIO 34) | **Present but unusable as shipped.** Flat 0 at 0/2.5/6/11 dB; `analogReadMilliVolts` = **142 mV** | GPIO 35 (floating control) showed normal noise, so the ADC is fine. The 142 mV matches this board's documented LDR defect exactly — see below |
| I²C bus on SDA 27 / SCL 22 | **Free** — full scan completes, no hang, 0 devices | Confirms the pins aren't held by other hardware |
| microSD slot | No card present (driver reached CMD0, got no reply) | Untested beyond that |
| RGB LED (GPIO 4/16/17) | **Confirmed.** Active-LOW; red=4, green=16, blue=17, observed in that order | `tools/hwtest` `l` command, watched on the unit |

### VERIFIED — read from the connected device (2026-09-16)

Via `esptool.py --port <port> flash_id` and `read_flash 0x8000 0xc00` + `gen_esp32part.py`:

| Fact | Value | How |
|---|---|---|
| Chip | ESP32-D0WD-V3, revision v3.1 | `esptool flash_id` |
| Cores / clock | Dual core, 240MHz, WiFi+BT | `esptool flash_id` |
| Crystal | 40MHz | `esptool flash_id` |
| Flash size | **4MB** (mfr `0x5e`, device `0x4016`) | `esptool flash_id` |
| Flash voltage | 3.3V (set by strapping pin) | `esptool flash_id` |
| MAC | `88:57:21:2e:11:b8` | `esptool flash_id` |
| Live partition table | `app0` 3M @0x10000, `spiffs` 896K, `coredump` 64K, `nvs` 20K, `otadata` 8K | read back from flash |

The live partition read is what proves the `huge_app.csv` switch actually took effect on the device — not just that the build accepted it. `logBootDiagnostics()` in `setup()` re-confirms chip/flash/PSRAM/heap at every boot, so a serial capture alone is sufficient evidence.

### ASSUMPTION — configured, works, but not independently probed

- **Display controller = ILI9341-compatible.** Asserted by `-D ILI9341_2_DRIVER=1` in `build_flags`. The panel renders correctly, which proves the driver is *compatible*, not that the silicon is an ILI9341 specifically (several clones accept the same command set). Probing the controller's ID register would settle it; not currently done.
- **Touch controller = XPT2046-compatible.** Same reasoning — `XPT2046_Touchscreen` communicates with it successfully.
- **Board = ESP32-2432S028R "CYD".** The pin map matches published CYD pinouts ([Random Nerd Tutorials](https://randomnerdtutorials.com/cheap-yellow-display-esp32-2432s028r/), [ESP32s.com pinout guide](https://esp32s.com/blog/the-complete-esp32-cheap-yellow-display-cyd-pinout-and-gpio-guide/)) and the verified chip/flash are consistent with it — but the silkscreen has not been read. Community sources corroborate; they do not prove.
- **Module type.** An ESP32-D0WD-V3 die is typically packaged as WROOM-32E, but the module marking is unread.
- **Onboard LDR on GPIO34.** Community-documented. GPIO34 is input-only and on **ADC1**, which matters: ADC2 is unusable while WiFi is active, so ADC1 is the correct choice regardless. The code reads it, but that a *light sensor* is what's attached is unconfirmed — and `LDR_HIGHER_MEANS_BRIGHTER` is a **guess** at wiring polarity.

### UNVERIFIED / BLOCKED — needs physical access or user interaction

- **The live, NTP-driven clock display.** The `setenv`/padding behaviour and the zero-leak result are VERIFIED on this board, and padding's DST equivalence was verified by driving the clock to chosen instants with `settimeofday()`. But WiFi cannot associate at the current location (the `secrets.h` SSID is elsewhere; the local Meraki AP has a captive portal the ESP32 can't traverse), so `configTime()` has never actually synced here and **no on-screen clock has been observed showing a correct real time**. Don't let the strength of the probe evidence bleed into a claim about the running display.
- **micro-SD slot.** Wired to GPIO 5 on the VSPI bus (18/19/23). The driver reaches CMD0 and gets no reply, which is consistent with no card inserted; nothing beyond that is confirmed. Note this bus is **not** shared with touch — touch has its own pins (see SPI bus ownership above), so there is no contention to manage.

### SPI bus ownership (important)

Three SPI-ish things share this board and none of them are on the pins you'd assume.

- **Display** — HSPI, via `USE_HSPI_PORT` and the `TFT_*` pins in `build_flags` (12/13/14/15).
- **Touch (XPT2046)** — its own dedicated wiring: `SCK=25, MISO=39, MOSI=32, CS=33, IRQ=36`. **Not** the VSPI default pins.
- **microSD** — the actual VSPI defaults: `SCK=18, MISO=19, MOSI=23, CS=5`.

**This was wrong in this file until it was measured, and the bug it hid is instructive.**
`XPT2046_Touchscreen::begin()` calls a bare `SPI.begin()`, which claims VSPI's *default*
pins — where the touch chip is not. Every read then clocked against a floating MISO and
returned all-ones (`x=8191 y=8191 z=4095`), and `loop()`'s own sanity gate (`p.x < 3900`)
silently discarded all of it. Net effect: **touch never worked at any point in this
project's history**, while presenting as "unreliable" rather than as broken. `setup()` now
calls `SPI.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS)` *before* `ts.begin()`;
ESP32's `SPIClass::begin()` returns early when the bus is already up, so the library adopts
the correct pins. Verified on hardware: raw coordinates now track a finger, and taps drive
city changes.

A corollary worth keeping: **touch and microSD do not share a bus.** They only appeared to
when touch was squatting on VSPI — which is why running `SD.begin(5)` used to hard-hang the
touch driver. With touch on its own pins that conflict is gone.

`TOUCH_CS`/`SPI_TOUCH_FREQUENCY` remain deliberately **undefined** in `build_flags`: those
enable TFT_eSPI's own touch driver, which does `pinMode(TOUCH_CS, OUTPUT); digitalWrite(TOUCH_CS, HIGH)`
at init (`TFT_eSPI.cpp:543-546`) — a second driver claiming GPIO 33. Don't re-add them.

## Transport security (open deviation — read before changing)

`fetchWeather()` uses **plain `http://`**. This is a knowing deviation from the `esp32-cyd-engineering` skill's "use TLS server verification; do not ship insecure certificate bypasses" rule, recorded here rather than left implicit:

- **What the previous `https://` actually did:** nothing security-wise. `HTTPClient::begin(url)` builds a `WiFiClientSecure` via `TLSTraits`, whose `verify()` calls `setInsecure()` when no CA is supplied (`HTTPClient.cpp:74-88`). So it was an *unauthenticated* TLS tunnel — encrypted against passive reading, but trivially MITM-able, which the same rule also forbids. Switching to HTTP removed ceremony, not protection.
- **Exposure:** public, unauthenticated, read-only weather data. No credentials, tokens, or personal data are transmitted. Worst case on-path: someone displays a wrong temperature.
- **Real verification is available** and this is revisitable: `WiFiClientSecure::setCACertBundle(const uint8_t*)` exists in the pinned core (`WiFiClientSecure.h:75`), backed by `arduino_esp_crt_bundle_set()`. Note it does **not** fall back to a built-in bundle — you must supply one.
- **Certificate facts (checked 2026-09-16):** `api.open-meteo.com` presents `CN=*.open-meteo.com` issued by **Let's Encrypt** (`CN=YE2`), valid 2026-08-31 → 2026-11-29. Pinning the leaf or intermediate would break roughly quarterly; only **ISRG Root X1** (valid to 2035) is a sane anchor. That also means the device breaks if Open-Meteo ever changes CA — an availability risk for an unattended display.
- **Hard requirement if scope changes:** if OTA is ever added, verified TLS stops being optional, because you'd be accepting executable code. Note the current `huge_app.csv` layout has no second app slot, so OTA is already blocked on a partition redesign.
- **Not yet handled:** TLS validation needs correct wall-clock time. `configTime()` is called after WiFi connects, but the first fetch is not gated on NTP having actually synced — enabling verified TLS without adding that gate would likely fail the first fetch after every boot.

## Flash/RAM budget

- **Partition table is `huge_app.csv`, not the PlatformIO default.** The stock `default.csv` splits 4MB of flash into two ~1.3MB OTA app slots + a SPIFFS partition — none of which this project uses (there's no OTA update code and no filesystem access). `huge_app.csv` collapses that into a single ~3MB app partition, tripling the usable code+font+asset space. Trade-off: no OTA support and a much smaller leftover SPIFFS region. If OTA updates are ever added, that decision needs revisiting (e.g. `min_spiffs.csv` keeps dual ~1.9MB OTA slots instead).
- **Current usage is ~33% of the 3MB app partition** — check with `pio run` after any change (the size report prints RAM/Flash % after every build). Most of the flash footprint is fonts: `LOAD_GLCD`/`FONT2/4/6/7/8`/`LOAD_GFXFF`/`SMOOTH_FONT` in `platformio.ini`'s `build_flags`, not application logic.
- **`LOAD_GFXFF` silently links all 44 of TFT_eSPI's bundled GFXFF free fonts**, unconditionally, via `Fonts/GFXFF/gfxfont.h` — regardless of which ones the sketch actually references. Never `#include` a specific `Fonts/GFXFF/*.h` file directly in `main.cpp`; they're already declared globally and re-including one is a duplicate-definition compile error. Just reference the font object (e.g. `&FreeSansBold24pt7b`) directly.
- **Static RAM is not the tight constraint — heap fragmentation is.** Static globals use ~15% of the 320KB, and `ESP.getFreeHeap()` typically reports 230KB+ free at runtime. But the *largest contiguous free block* (`heap_caps_get_largest_free_block()`) measured only ~110KB on real hardware — WiFi/lwIP fragment the heap into many smaller pieces. Any single allocation needs to fit in one contiguous block, so "plenty of free heap" doesn't mean "a big allocation will succeed." See the rendering note below for where this actually bit us.

## Architecture

- **Single-file firmware.** All logic — WiFi/NTP setup, HTTP fetch, JSON parsing, UI drawing, touch handling — lives in `src/main.cpp` following the Arduino `setup()`/`loop()` model. There's no separation into headers/modules yet.

- **Display config lives in `platformio.ini`, not a library `User_Setup.h`.** TFT_eSPI is configured entirely via `build_flags` (`USER_SETUP_LOADED=1` plus pin/driver/SPI-speed defines). This is the standard pattern for CYD boards since the shared library source can't hold per-project settings. When changing display wiring or driver, edit `build_flags`, never look for a `User_Setup.h`.

- **Flicker-free rendering — a full-frame sprite was tried and doesn't work on this hardware.** The first version rendered `drawWeatherScreen()` into a full 320x240x16-bit `TFT_eSprite` (150KB) and blitted it in one `pushSprite()` call to avoid the black flash a direct `fillScreen()`-then-redraw causes. **On real hardware this allocation failed every time**, including on the very first draw right after boot — logged via `Serial.println("Sprite alloc failed...")` in the fallback path that (fortunately) already existed. Measuring `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` showed why: the largest contiguous free block is ~110KB, well under the 150KB needed, even though `ESP.getFreeHeap()` reports ~235KB+ free overall — free heap being large doesn't mean a large *contiguous* allocation will succeed, since WiFi/TLS/lwIP leave it fragmented into many smaller blocks. **What ships instead:** nothing gets a full-panel wipe on every redraw. Every zone in `drawWeatherScreen()` either fully repaints its own opaque rectangle (the title/bottom bars) or sets an explicit text background color (`setTextColor(fg, bg)`) so a shorter new string can't leave the old one's tail visible — the one genuinely irregular shape is the weather icon (built from many overlapping circles/triangles, not a rectangle), which gets its own small dedicated sprite (`iconSprite`, 113x130x16-bit ≈ 29KB — comfortably inside the ~110KB block) so at least *that* element swaps atomically instead of visibly building up stroke-by-stroke. The one-time `tft.fillScreen(TFT_BLACK)` needed to clear the boot/connecting screen's leftover text now happens once in `setup()`, not on every redraw. `drawDiagnosticsScreen()` keeps a plain `fillScreen()` since it only redraws on a deliberate double-tap/dismiss, not a recurring timer — the trade-off there is deliberately different because the frequency is different.

- **Heap leak — FIXED. Root cause: `setenv("TZ", ...)` with a value of *changing length*.** Free heap used to drain ~18 B/s (~180 bytes per 10s city-switch cycle), forcing a watchdog reboot roughly every 3.5 hours. It is now flat.

  Isolated in two stages. First, bisection on real hardware (each step measured over 4-6 minutes) ruled out everything else:

  | Variant | Rate |
  |---|---|
  | Baseline (HTTPS, Arduino `String`, per-call client) | 19.4 B/s |
  | Plain HTTP, stream parse, HTTP/1.0, `char` buffers | 17.5 B/s |
  | HTTP/1.1 keep-alive, persistent `WiFiClient` | 18.7 B/s |
  | **Radio off entirely** (`WIFI_OFF`, synthetic data) | 18.1 B/s |
  | Radio off **+ iconSprite disabled** | 17.5 B/s |
  | Radio off **+ one fixed TZ string** | **0.00 B/s — flat** |

  The rate is completely invariant to TLS, `String` vs `char` buffers, stream vs `getString()`, connection reuse, and the icon sprite.

  Second, an isolated probe (no WiFi, no display, no app code — just `setenv` in a loop, 1000 calls per phase with the heap diffed before and after, so the figure is bytes **per call** rather than per second) pinned down the actual trigger:

  | Phase | TZ transition | bytes/call |
  |---|---|---|
  | A | same value repeatedly | **0.00** |
  | B | different value, **same** `strlen` (`EST5EDT,M3.2.0,M11.1.0` → `PST8PDT,M3.2.0,M11.1.0`) | **0.00** |
  | C | `strlen` 22 ↔ 26 | **24.00** |
  | D | `strlen` 22 ↔ 5 | **21.96** |
  | E | same `strlen`, **`tzset()` removed** | **0.00** |
  | F | different `strlen`, **`tzset()` removed** | **24.00** |

  Two things follow, and neither is what the bisection alone suggested. **It is the value's length that matters, not the value** — a different zone of identical length is free. And **`tzset()` is innocent**: phase F leaks identically without it. newlib's `setenv` compares `strlen(new)` against `strlen(old)` rather than against the size of the block it already allocated, so a longer value forces a fresh allocation that is never reclaimed, while a shorter value is written in place but leaves the recorded length short — so the next longer value allocates again.

  This model *predicts* the observed rate. Only Paris (26) and Arusha (5) differ from home's 22 chars, so only those two cities leaked: 2 length changes/sec × 24 B × (2 of 5 cities) ≈ 19 B/s, against 18.1-19.4 B/s measured. It also explains the earlier misleading "pausing auto-rotate looks flat" result — paused, the city sat on California, KY, whose TZ string is 22 chars, exactly the same length as home's. TZ was still being set ~3×/second; it just never changed length.

  **The fix is `setTimezone()` in `main.cpp`** — the single `setenv()` in the firmware. It right-pads every TZ string with spaces to a fixed `TZ_PADDED_LEN` (31) so `strlen` never changes and every write is an in-place copy. This was chosen over the more obvious "cache each zone's UTC offset and render with `gmtime_r(now + offset)`" because padding keeps `localtime_r`'s exact DST semantics, whereas an offset cache both goes stale across a DST transition and produces a `struct tm` with wrong `tm_isdst`/`tm_zone` (harmless for today's `%I:%M %p` format, a latent bug the moment anyone adds `%Z`). It also avoided needing `tm_gmtoff` (**not** present in this toolchain's `struct tm` — verified by compile) or `timegm` (**not** declared in these headers).

  Padding is semantically neutral *on this toolchain*, verified rather than assumed: the padded and unpadded strings were compared at six instants — midwinter, midsummer, and both sides of the US and EU DST fallbacks — across all four distinct zones, giving identical rendered time and identical `tm_isdst` in all 24 comparisons. Don't "tidy" the padding away. `checkTimezoneLengths()` runs at boot and logs loudly if any city's TZ string exceeds `TZ_PADDED_LEN`, because `setTimezone()` would silently truncate it and a truncated POSIX rule still parses — just to a *different* rule, so the failure mode is a plausible-looking wrong clock rather than a crash.

  **Verified after the fix:** 30 heap samples over 292s with the city rotating through all five zones (confirmed via the `Auto-switch ->` log, including Paris and Arusha) held `free=242656` on every single sample — spread of 0 bytes, versus ~5.3KB that would have leaked over the same window before. `checkHeapHealth()` is deliberately **kept**: it is cheap and it is the only guard against the next leak, but it should now never fire.


- **Hardware:** ILI9341 320x240 TFT over SPI, plus an XPT2046 resistive touch controller on its own CS/IRQ pins (`TOUCH_CS`/`TOUCH_IRQ`, defined near the top of `main.cpp`; touch SPI speed is set separately via `SPI_TOUCH_FREQUENCY`). See the CYD hardware section above for board-level detail.

- **Two full-screen views, and three redraw entry points. Getting these wrong is the most common bug in this file.** `VIEW_WEATHER` is the detailed layout; `VIEW_CLOCK` is a roughly 70/30 split with a large home clock over a condensed weather strip, reached by a **vertical swipe**. The two layouts share no zones, so switching does a single `fillScreen()` — the per-zone erase discipline used inside each layout cannot clean up after the other one.

  Never call `drawWeatherScreen()` or `drawWifiStatusDot()` directly from `loop()` or the gesture handler: both draw at the weather layout's coordinates unconditionally, so in the clock view they land on top of whatever is already there. Use the dispatcher that matches the granularity you need:

  | Call | Use when |
  |---|---|
  | `drawCurrentView()` | The whole panel needs repainting — view switch, overlay dismissed, first draw |
  | `redrawWeatherContent()` | The weather *data* changed — new city, fresh fetch, pause toggled |
  | `redrawWifiIndicator()` | Only the connection dot changed |

  **This has bitten twice, both times from editing by pattern-match instead of enumerating call sites.** Once a blanket replace matched the dispatcher's own `else` branch and produced `drawCurrentView()` calling itself — which hung *silently* rather than crashing, because a tail call at `-O2` becomes an unconditional jump, so there was no stack overflow and no panic. The second time the same replace *missed* the auto-switch call site (indented 4 spaces where the pattern expected 2, 6 or 8), so the full weather layout repainted over the clock view every 10 seconds. If you change redraw routing, `grep` every call site and convert them individually.

  `redrawWeatherContent()` exists specifically so a city change doesn't repaint the big clock — doing so makes the time visibly blink every 10 seconds under auto-rotate.

- **Font 8 is the only face large enough for the clock view, and it has no letters.** `LOAD_FONT8` gives a 75px face containing **only `1234567890:-.`** — so the AM/PM tag is drawn separately in font 4 beside the digits, not concatenated into the time string. `logLayoutMetrics()` measures it at boot: `"07:42"` renders 249px of the 320px width.

- **`logLayoutMetrics()` measures real rendered text widths at boot** rather than trusting estimates, and logs `*** COLLIDES ***` / `*** OVERFLOWS ***` if anything stops fitting its zone. Added after three layout faults shipped at once (a clipped header, a clock box over the city name, and stale text left by centred strings that got shorter). Adding a city with a long name or a wordier condition string now reports itself on serial instead of appearing as a smear on the panel.

- **Global-state redraw model, with one exception.** A handful of globals (`weatherTemp`, `weatherDesc`, `weatherHumidity`, `weatherWind`, `weatherCodeInt`, `wifiConnected`, `currentCityIndex`, ...) hold the latest fetched values, and most updates trigger a full `drawWeatherScreen()` repaint. The exception is the clock: `drawHomeClock()`/`drawCityClock()` are factored out so `loop()` can refresh just those small text regions once a second (`CLOCK_TICK_INTERVAL`) via a direct `fillRect`-then-`drawString` on `tft`, without paying for a full sprite-buffered redraw. Before this, the on-screen clocks only updated on the 10s city-switch cadence and visibly froze in between.

- **City rotation drives both data and UI.** The `cities[]` array (`{lat, lon, name, tz}`, `tz` a POSIX TZ string) is the single source of truth for which city's weather is fetched and which local time is shown. `NUM_CITIES` is derived via `sizeof(cities)/sizeof(cities[0])`, so adding/removing a city is a one-line change to the array. Advancing city happens three ways, all converging on the same fetch+redraw path — see Gestures below.

- **Two independent clocks, both routed through `setTimezone()`.** `drawHomeClock()` sets `TZ` to `HOME_TZ`; `drawCityClock()` and `isCurrentCityNight()` set it to `cities[currentCityIndex].tz`. Since the TZ env var is process-global state (not thread-local or scoped), any new time-reading code must set the TZ it needs immediately before calling `getLocalTime()` — never assume the previous call left the "right" zone active. **Call `setTimezone()`, never `setenv("TZ", ...)` directly** — it is the only `setenv` in the file and bypassing it reintroduces the heap leak documented above.

- **Weather fetch + rendering pipeline:** `fetchWeather(cityIdx)` hits Open-Meteo's forecast API (no key required) via `HTTPClient`, requesting both `current` (temperature/humidity/weather code/wind) and today's `daily=sunrise,sunset`. It derives two things from the returned WMO `weather_code`: a text description (`wmoToString`) and a hand-drawn icon (`drawWeatherIcon`). These two mappings are separate `if`/switch ladders over the same WMO code ranges — extend both together when adding new weather code handling.

- **Chunked transfer encoding dictates how the body is read.** Open-Meteo replies with `Transfer-Encoding: chunked`. `http.getStream()` hands back the raw socket *including* chunk-length framing, which is not valid JSON — feeding it to `deserializeJson()` fails with `InvalidInput` on every fetch. This compiles cleanly either way and was only ever caught on hardware. There are two ways out and this project has used both: `http.useHTTP10(true)` (forces HTTP/1.0, so the server doesn't chunk — but it also sets `_reuse = false`, killing keep-alive), or `http.getString()`, which performs the chunked de-framing itself. **What ships now is `getString()` + `setReuse(true)`** (`fetchWeather()`, `main.cpp:531-542`), keeping the TCP connection alive across fetches. `useHTTP10()` is no longer called anywhere — don't re-add it without also switching back to `getStream()`.

- **Arduino `String` is kept out of the repeating path, with one deliberate exception.** `weatherTemp`/`weatherDesc`/`weatherHumidity`/`weatherWind` are fixed `char` buffers written with `snprintf`/`strlcpy`, `wmoToString()` returns a pointer to a string literal, and the URL is built with `snprintf` into a stack buffer. TFT_eSPI's `const char*` `drawString()` overloads mean nothing downstream needs `String`. Log lines use `Serial.printf` rather than `"literal " + value` concatenation for the same reason. This is deliberate: these run every 10s forever, and the skill warns specifically against `String` churn in long-running paths. **The exception is `String payload = http.getString()`** in `fetchWeather()`, which is the price of letting `HTTPClient` de-frame the chunked response (see the bullet above). It is a balanced alloc/free per fetch, not a leak — but note it is the only repeating heap allocation that the zero-leak measurement did **not** exercise, because that run had the radio off. Re-measure with WiFi up before calling the heap definitively flat.

- **Displayed data is explicitly validity-gated.** `weatherDataValid` is true only while every weather global describes `cities[currentCityIndex]`. `fetchWeather()` calls `invalidateWeatherData()` on entry and on *every* failure path (no WiFi / HTTP error / parse error), which blanks temp/humidity/wind to `"--"`, sets `weatherCodeInt = WEATHER_CODE_UNKNOWN` (rendering the `?` icon), and puts the reason in the description line — shown in amber rather than cyan. `isCurrentCityNight()` also returns early when invalid, so the night tint can't be driven by another city's sun times. **This fixed a real defect:** previously only `weatherDesc` was overwritten on failure, so a failed Paris fetch displayed Charlotte's temperature, humidity and wind under the heading "Paris, France" — wrong data presented as current, which is worse than no data.

- **Day/night theming per city.** `isCurrentCityNight()` compares the current city's local time (via its own `tz`) against `sunriseMinutes`/`sunsetMinutes` parsed from that fetch's `daily.sunrise`/`daily.sunset` (`isoTimeToMinutes()` just extracts `HH:MM`, no date/DST arithmetic). When it's night, `drawWeatherIcon()` swaps the sun for a drawn crescent moon (`drawMoon()`) on clear/partly-cloudy codes, and `drawWeatherScreen()` tints the icon panel with a dark navy background instead of pure black. Caveat: this compares our hardcoded POSIX `tz` strings against Open-Meteo's auto-resolved IANA timezone for the same coordinates — they agree except in rare DST-transition edge cases.

- **Icons are procedural, not bitmaps.** `drawSun`/`drawCloud`/`drawMoon`/`drawRaindrop`/`drawSnowflake`/`drawLightning` etc. build icons out of TFT_eSPI primitives (`fillCircle`, `fillTriangle`, `drawLine`), all parameterized by a single `scale` value passed down from `drawWeatherIcon()`. Every one of these helpers takes a `TFT_eSPI &d` parameter rather than touching the global `tft` directly, so the same code draws to either the sprite or the real panel.

- **Anti-aliased typography via GFXFF, used selectively.** The title and temperature figure use `setFreeFont(&FreeSansBold12pt7b)` / `&FreeSansBold24pt7b` (TFT_eSPI's smooth/anti-aliased free-font path) instead of the classic bitmap fonts; `setTextFont(2)` immediately afterward resets back to classic fonts for everything else. This reset matters more than it looks: TFT_eSPI reuses font ID `1` to mean "classic GLCD font" when `gfxFont` is null and "use the free font" when it isn't — `setTextFont(n)` clears `gfxFont`, so skipping the reset would make any later `drawString(..., 1)` call unexpectedly render in giant free-font text. The smaller UI text (city name, clocks, humidity/wind, footer) intentionally stays on classic fonts — untested territory for GFXFF vertical alignment at that size, and the classic fonts already look fine that small.

- **The RGB LED runs a slow ambient fade from its own FreeRTOS task.** `ambientLedTask()` walks a ring of colour stops (black → red → green → blue → white → black, then a held dark segment) with a smoothstep ease between them, so each colour brightens, peaks and ebbs into the next. 5s per hand-off, 30s per full ring. Output is gamma-corrected (`powf(level, 2.2f)`) because perceived brightness is roughly the 2.2 power of duty — a linear ramp appears to rush the bright end and crawl the dark end. Brightness is capped at `LED_BRIGHTNESS` and scaled by `currentBacklightDuty`, so a dark room dims the LED along with the panel.

  **Two things to know before touching this.** First, it runs as a task rather than from `loop()`: `loop()` ends in `delay(100)`, so it could only update at 10Hz, which is visibly steppy on a slow fade. Shortening that delay would mean re-tuning gesture timing that is confirmed working, so the animation gets its own 50Hz cadence instead. Second — **the LEDC timer hazard** — Arduino's `ledcSetup()` assigns timer `(channel/2)%4`, so channels 0 and 1 *share* a timer. The backlight owns channel 0 at 8-bit; putting a 12-bit channel on 1 would silently re-resolution the backlight and drive the panel almost black. The LED uses channels 2, 3 and 4 (timers 1, 1, 2). Check this before adding any further LEDC channel.

- **Backlight is PWM-driven with ambient + time-of-day brightness**, not just on/off. `setup()` calls `ledcAttachPin(TFT_BL, BACKLIGHT_CHANNEL)` right after `tft.init()` (which otherwise leaves the pin as a plain digital HIGH), then `updateBacklight()` runs every `BRIGHTNESS_UPDATE_INTERVAL` (2s) in `loop()`. Target brightness comes from the onboard LDR (`analogRead(LDR_PIN)`, mapped to a `BACKLIGHT_MIN_DUTY`–`BACKLIGHT_MAX_DUTY` range), then clamped to `BACKLIGHT_NIGHT_CEILING` during home-local night hours (22:00–07:00) so a lit room at 2am doesn't still force full brightness at a bedside. `currentBacklightDuty` is smoothed toward the target (`/8` per update) rather than snapping, so a passing shadow over the sensor doesn't visibly flash the panel. **Ambient dimming is disabled, because the on-board light sensor is unusable as shipped — not because it is missing.** The sensor is **R21** on the silkscreen: an LDR (part GT36516) wired from GPIO 34 to **ground**, with a pull-up divider to 3V3 from R15/R19 (reported 1M each ≈ 500k parallel; single-sourced, unconfirmed). Measured here: `analogRead` = 0 at every attenuation in room light, under a torch and covered, with `analogReadMilliVolts` = **142 mV** — which matches this board's documented LDR defect exactly. The LDR actually fitted has roughly **20× lower impedance** than the GT36516 the divider assumes, so the junction never climbs out of the ESP32 ADC's bottom dead zone and floors to 0. Backlight spill from the panel edge onto R21 compounds it. **An earlier version of this file wrongly recorded the sensor as absent.**

Because the LDR goes to ground, **dark reads HIGHER** — so the original `LDR_HIGHER_MEANS_BRIGHTER = true` was also inverted. It is now `false`, so that re-enabling the feature behaves correctly rather than backwards.

With raw pinned at 0, `lightFrac` was permanently 0 and the backlight sat at `BACKLIGHT_MIN_DUTY` — **30 of 255, about 12% brightness, for the entire life of the project.** `LDR_PRESENT` is now `false` and `computeTargetBacklightDuty()` returns `BACKLIGHT_DEFAULT_DUTY` (200), still subject to the night ceiling. Two routes to real ambient dimming: solder **~51k in parallel with R15** to restore usable range, or fit a **BH1750 on CN1** and ignore R21 entirely.

- **Touch gestures: tap / swipe / long-press / double-tap**, all from one state machine in `loop()`. It tracks touch-down position/time (`touchStartX/Y`, `touchStartTime`) separately from the continuously-updated "still held" position (`lastTouchX/Y`) — the original implementation re-stamped its timestamp every polling tick, which meant it never actually measured hold duration. See Gestures below for the behavior table.

- **Diagnostics overlay.** `drawDiagnosticsScreen()` (double-tap to open) shows uptime, free/min-free heap, WiFi RSSI, IP, flash usage, CPU frequency, and current backlight duty — otherwise only visible over serial. `showingDiagnostics` gates the periodic redraw blocks in `loop()` (clock tick, auto-switch, WiFi-drop dot) so they don't paint over it; any tap, swipe, or long-press while it's open dismisses back to the weather screen.

- **WiFi resilience.** `setup()` still blocks on the initial connection (as before), but `loop()` runs a non-blocking watchdog every `WIFI_CHECK_INTERVAL` (5s) that detects a dropped connection, calls `WiFi.reconnect()`, and updates the `wifiConnected` flag + status dot immediately via `drawWifiStatusDot()`. `fetchWeather()` early-returns with a "No WiFi" description instead of attempting an HTTP call while disconnected, and sets an 8s HTTP timeout so a bad connection can't stall `loop()` indefinitely.

## Gestures

| Gesture | Effect |
|---|---|
| Tap | Next city |
| Horizontal swipe | Previous city (threshold 45 **screen px**, not raw ADC counts) |
| **Vertical swipe** | **Toggle between the weather view and the 70/30 clock view.** Mutually exclusive with the horizontal test — each axis must dominate the other by 2×, so a sloppy diagonal falls through and is treated as a tap |
| Long-press (~700ms, low drift) | Toggle auto-rotate pause/resume ("Paused" replaces "Auto 10s" in the info bar) |
| Double-tap (2nd tap within 400ms) | Open the diagnostics overlay (first tap still advances the city as normal; the 2nd tap's advance is replaced) |
| Any tap/swipe/long-press while diagnostics is open | Dismiss back to whichever view was active |

All four gestures are confirmed working on the physical unit as of 2026-09-18. Earlier advice in this project to *reduce* the gesture vocabulary was premised on touch being unreliable — it was in fact not working at all (see the SPI bus ownership note), and once re-pinned and calibrated the full set behaves. Keep the diagnostic line below regardless.

Every touch release logs one diagnostic line — held time and per-axis travel — because a gesture that fails to classify is otherwise completely silent:

```
touch release: held=180ms dx=3 dy=62 (swipe needs |d|>45 and 2x the other axis; tap window 50-700ms)
```

That distinguishes the three ways a swipe can fail to register: held too long (past the 700ms long-press threshold), travelled too little (under 45px), or not detected at all.
