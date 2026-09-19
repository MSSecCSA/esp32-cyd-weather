# CYD hardware test rig

A separate PlatformIO project for exercising board hardware directly. It is **not**
production firmware — flashing it replaces the weather station until you flash back.

It exists because patching diagnostics into `src/main.cpp` went badly once before: a
`DIAG_SKIP_FETCH` build was left on the board serving fake weather data for a while
before anyone noticed. Keeping the rig as its own project means production source is
never edited to run a test.

`platformio.ini` is a copy of production's, so the display and touch behave identically.
Keep it that way — a divergent build flag here produces results that don't transfer.

## Running it

```sh
# flash the rig (production is in git; you will flash it back after)
python3 -m platformio run -d tools/hwtest --target upload --upload-port /dev/cu.usbserial-110

# send a command and capture output until DONE
python3 tools/hwtest/drive.py <cmd> [seconds]

# put production back when finished
python3 -m platformio run --target upload --upload-port /dev/cu.usbserial-110
```

## Commands

| Cmd | Test | Needs you to |
|-----|------|--------------|
| `l` | RGB LED cycle: red → green → blue → white, ~1.4s each | watch the board |
| `b` | BOOT button (GPIO 0), 15s | press it a few times |
| `d` | LDR / R21 with backlight **off**, 24s + attenuation sweep | cover and uncover R21 |
| `L` | LDR / R21 with backlight **on** — the real operating condition | cover and uncover R21 |
| `a` | ADC probe on GPIO 34 vs floating GPIO 35 at all four attenuations | nothing |
| `i` | I²C scan on SDA 27 / SCL 22 | nothing |
| `s` | microSD mount on GPIO 5 | insert a card |
| `p` | Speaker tone sweep on GPIO 26 | listen |
| `r` | Raw touch stream, 10s | drag on the screen |
| `t` | **5-point touch calibration** — prints paste-ready constants | tap 5 crosshairs |
| `v` | Draw test, 20s | draw on the screen |
| `?` | Help | |

## What this rig has already established

- **Touch is on its own SPI pins** — `SCK=25, MISO=39, MOSI=32, CS=33, IRQ=36`, *not* the
  VSPI defaults a bare `SPI.begin()` claims. Production had been reading a floating MISO
  and getting all-ones (`x=8191`), which its own sanity gate discarded — so touch had
  never worked at all.
- **Touch calibration**: 5-point fit, worst residual 6.3 px. Constants live in
  `src/main.cpp` as `TOUCH_AX/BX/AY/BY`.
- **R21 (the LDR) works**, but only at **0 dB** ADC attenuation. Its whole signal sits
  between ~142 mV and ~740 mV, which Arduino's default 11 dB squashes to near nothing.
  0 dB yields 2500 counts covered vs 746 at 11 dB.
- **BOOT button (GPIO 0)** is a usable runtime input — the only software-readable button
  on the board, since RST is wired to EN.
- **I²C pins 27/22 are electrically free** — a full scan completes without hanging.

## Still outstanding

- **RGB LED (GPIO 4/16/17, believed active-LOW)** — the `l` test has been run twice and
  not observed either time. Eight seconds of someone watching the board would settle it.
- **microSD** — no card has been inserted yet.
- **Speaker** — nothing connected to P4 yet.

## setenv_probe.cpp.txt

Kept as a text file, not a buildable project, because it was a single-purpose experiment.
It isolated the `setenv("TZ", ...)` heap leak by calling it in a tight loop with the heap
diffed before and after, which is what showed the leak is driven by the value's *length*
changing (24 bytes/call) rather than the value itself (0 bytes/call), and that `tzset()`
is not involved. Drop it into a bare PlatformIO project if that ever needs re-testing.
