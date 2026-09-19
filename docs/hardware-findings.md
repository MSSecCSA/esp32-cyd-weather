# Hardware findings

Things this board actually does, as opposed to what the community pinouts say.

Every one of these compiled cleanly, ran, and looked *nearly* fine. None were findable
without putting an instrument on the board. If you own a CYD, some of them will probably
cost you an evening too.

The rig that produced all of this is in [`tools/hwtest/`](../tools/hwtest/) — you can re-run
any of it on your own board. Labels below follow the convention in [`CLAUDE.md`](../CLAUDE.md):
**VERIFIED** means measured here, with the method recorded.

---

## 1. The touch controller is not on the VSPI defaults

**VERIFIED.** The XPT2046 has its own dedicated wiring:

```
SCK = 25    MISO = 39    MOSI = 32    CS = 33    IRQ = 36
```

`XPT2046_Touchscreen::begin()` calls a bare `SPI.begin()`, which claims VSPI's *default*
pins — 18/19/23 — where the chip simply is not. Every read then clocks against a floating
MISO and returns all-ones:

```
x=8191  y=8191  z=4095        <- 2^13-1 and 2^12-1, i.e. no reply at all
```

The sting is what happens next. A typical sanity check like `if (p.x < 3900)` discards those
readings, so touch presents as **unreliable** rather than **broken**. In this project it had
never registered a single event, while appearing merely flaky. The defensive check was
perfectly hiding a total failure.

**Fix.** Re-pin the global SPI object *before* `ts.begin()`. ESP32's `SPIClass::begin()`
returns early when the bus is already initialised, so the library adopts your pins:

```cpp
SPI.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
ts.begin();
```

**Corollary.** microSD is on the *real* VSPI (18/19/23, CS 5). Touch and SD therefore do
**not** share a bus — they only appear to when the touch driver is squatting on VSPI, which
is the bug above. Running `SD.begin(5)` used to hard-hang the touch driver for exactly this
reason.

---

## 2. The light sensor needs 0 dB ADC attenuation

**VERIFIED.** R21 on the silkscreen is an LDR (part GT36516), wired GPIO 34 → **ground**,
with a pull-up divider to 3V3. Because it goes to ground, **dark reads high**.

Its entire signal lives between ~142 mV and ~740 mV. Arduino's default **11 dB** attenuation
scales the ADC to ~2.5 V full-scale, so almost all of that range is discarded before your
code sees it — `analogRead()` floors to `0` and the sensor looks dead.

Measured on this board, backlight on at duty 220, covering vs uncovering R21:

| Attenuation | Covered (dark) | Uncovered (lit) |
|---|---:|---:|
| **0 dB** | **2500** | 0 |
| 2.5 dB | 1952 | 0 |
| 6 dB | 1391 | 0 |
| 11 dB *(Arduino default)* | 746 | 0 |

One line recovers 3.3× the resolution:

```cpp
analogSetPinAttenuation(LDR_PIN, ADC_0db);
```

**What this cost before it was found.** With `raw` stuck at 0 and the polarity flag set the
wrong way round, the backlight sat at `BACKLIGHT_MIN_DUTY` — **30 of 255, about 12%** —
permanently. The panel had been running dim for the life of the project.

**Known limits.** The bright end is compressed: anything from "lit room" upward reads 0, so
this tells dark from lit but not bright from very bright. Backlight spill from the panel
edge also falls on R21, so the sensor partly watches the display it controls. Soldering
~51 kΩ in parallel with R15 decompresses the bright end if you want proportional daylight
sensing.

---

## 3. `setenv("TZ", …)` leaks — but only on *length* change

**VERIFIED.** Isolated with a standalone probe: 1000 calls per phase, heap diffed before and
after, so the figure is bytes **per call** rather than per second.

| Phase | Transition | bytes/call |
|---|---|---:|
| A | same value repeatedly | **0.00** |
| B | different value, **same** `strlen` | **0.00** |
| C | `strlen` 22 ↔ 26 | **24.00** |
| D | `strlen` 22 ↔ 5 | **21.96** |
| E | same `strlen`, **`tzset()` removed** | **0.00** |
| F | different `strlen`, **`tzset()` removed** | **24.00** |

Two conclusions neither obvious nor commonly documented: **it is the value's length that
matters, not the value**, and **`tzset()` is innocent** — phase F leaks identically without
it.

newlib's `setenv` compares `strlen(new)` against `strlen(old)` rather than against the size
of the block it already allocated. A longer value forces a fresh allocation that is never
reclaimed; a shorter one is written in place but leaves the recorded length short, so the
next longer value allocates again.

**How it showed up.** A per-second clock tick rotating timezone strings of differing lengths
drained ~18 B/s and forced a watchdog reboot roughly every 3.5 hours. The model predicts the
observed rate: only two of five cities differed from the home zone's 22 characters, giving
2 length-changes/sec × 24 B × (2 of 5) ≈ 19 B/s against 18.1–19.4 measured.

**Fix.** Right-pad every TZ string to a fixed width so `strlen` never changes. Padding with
trailing spaces is semantically neutral — verified against the unpadded strings at six
instants including both sides of the US and EU DST fallbacks, with identical output and
identical `tm_isdst` in all 24 comparisons.

This was chosen over caching UTC offsets because padding keeps `localtime_r`'s exact DST
semantics. It also sidesteps two toolchain gaps found by compile-testing: `tm_gmtoff` is
**not** in this toolchain's `struct tm`, and `timegm` is **not** declared.

---

## 4. LEDC channels 0 and 1 share a timer

**VERIFIED by near-miss.** Arduino's `ledcSetup()` assigns timer `(channel/2)%4`. So:

```
ch0 → timer 0     ch2 → timer 1     ch4 → timer 2
ch1 → timer 0     ch3 → timer 1     ch5 → timer 2
```

If the backlight owns channel 0 at 8-bit and you put a 12-bit channel on 1, you silently
re-resolution the backlight. Duty 200 becomes 200/4096 instead of 200/255 and the panel goes
nearly black — with nothing in the logs to say why.

Check the timer mapping before adding any LEDC channel. The ambient LED here uses 2, 3 and 4.

---

## 5. Both USB ports are the same CH340

**VERIFIED** from the host: exactly one `0x1A86:0x7523` enumerates.

The newer CYD revision fits USB-C *and* micro-USB, wired to one USB-serial bridge. Use
either — never both at once, which puts two 5 V supplies across one chip.

Related: there is only **one software-readable button**. BOOT is on GPIO 0 and works fine as
a runtime input (idle 1, pressed 0). RST is wired to EN/CHIP_PU, so reading it *is* a reset.

---

## Expansion budget

Only **three GPIOs are free**: 22, 27 and 35 — and 35 is input-only with no internal
pull-up. The display and touch controller claim twelve between them.

But 27 and 22 are SDA/SCL on the CN1 header, and I²C is a *bus*, so the practical ceiling is
addresses rather than pins. [`peripheral-options.html`](peripheral-options.html) is the full
study, including a pin-by-pin map and what's worth attaching.

Two traps worth repeating here:

- **GPIO 22 appears on both P3 and CN1 — it is one pin, not two.**
- **GPIO 27 is on ADC2, which is dead while WiFi runs.** Fine for I²C, which is digital;
  never `analogRead()` it. That is also why the onboard LDR sits on GPIO 34 (ADC1).

---

## Re-running any of this

```sh
pio run -d tools/hwtest --target upload
python3 tools/hwtest/drive.py '?'      # list the tests
```

Covers the RGB LED, BOOT button, light sensor at both backlight states, an ADC probe, an
I²C scan, microSD, the speaker, a raw touch stream, and 5-point touch calibration that
prints paste-ready constants.

If your board disagrees with anything above, that is genuinely useful —
[open an issue](https://github.com/MSSecCSA/esp32-cyd-weather/issues/new). CYD revisions
vary in real ways.
