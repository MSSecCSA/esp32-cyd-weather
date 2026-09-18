---
name: esp32-cyd-engineering
description: Evidence-first coding and review guidance for ESP32-2432S028R Cheap Yellow Display projects. Use when scaffolding, modifying, debugging, reviewing, or documenting firmware for the original ESP32-WROOM-32 CYD. Prevents guessed pins, stale APIs, incompatible board variants, blocking loops, unsafe secrets, and unverified library assumptions.
version: 1.0.0
last_verified: 2026-09-16
---

# ESP32 CYD Engineering Skill

## Mission
Produce buildable, maintainable firmware for the **ESP32-2432S028R Cheap Yellow Display** without guessing hardware details or coding from stale model memory.

This skill is for the common original board built around an ESP32-WROOM-32, a 240 x 320 SPI TFT commonly using an ILI9341-compatible controller, and a resistive touch controller commonly identified as XPT2046-compatible. Board clones and revisions vary. **Never treat a community pin map, controller identity, flash size, or touch calibration as proven until verified against the specific device or repository.**

## Non-negotiable operating rules

1. **Inspect before generating.** Read the repository, build files, lock files, board definitions, `sdkconfig`, partition table, display configuration, and existing pin declarations before proposing code.
2. **Identify the target.** Determine the exact board marking, MCU/module, framework, framework version, flash size, display controller, touch controller, USB-to-UART device, and wiring already encoded in the project.
3. **Prefer primary documentation.** Use stable-version Espressif documentation, the selected framework's official docs, and the exact library release docs. Community CYD guides may establish board-specific context, but must not override inspected hardware/configuration.
4. **Do not silently upgrade APIs.** If code targets an older framework or library, explain the compatibility boundary before changing it.
5. **No fabricated symbols.** Never invent function names, Kconfig options, component names, pin numbers, library versions, menu paths, or configuration keys.
6. **Prove the build.** Run the repository's declared build command. If hardware is available, flash and capture serial output. If not, state that runtime behavior is unverified.
7. **Keep uncertainty visible.** Use `UNVERIFIED`, `ASSUMPTION`, and `BLOCKED` labels. Never bury uncertainty in prose.
8. **Make the smallest safe change.** Preserve working configuration and isolate board-specific definitions from application logic.

## Language and framework decision

Choose from this order unless the repository already dictates otherwise:

- **ESP-IDF with C/C++:** Default for long-lived, security-sensitive, OTA-enabled, production-style firmware. ESP-IDF is Espressif's official framework for ESP32 SoCs.
- **Arduino framework with C++:** Use for rapid delivery, broad library compatibility, and smaller maker projects. Prefer PlatformIO or an explicit reproducible Arduino CLI configuration over an undocumented workstation setup.
- **Arduino as an ESP-IDF component:** Use only when the project needs Arduino compatibility plus ESP-IDF services and the team accepts the added integration complexity.
- **MicroPython:** Use for experiments, teaching, and rapid iteration where interpreter overhead, library availability, startup behavior, and deployment model are acceptable.
- **Rust, JavaScript, Lua, Zig, or other ports:** Treat as opt-in ecosystems. Require explicit toolchain, target, driver, and maintenance validation before recommending.

Do not say these languages are “natively understood.” The ESP32 executes compiled Xtensa machine code. MicroPython and similar environments require firmware containing an interpreter/runtime.

## Required discovery pass

Before writing code, report:

```text
Target board/revision:
MCU/module:
Framework and pinned version:
Build system:
Flash size and partition table:
Display controller and SPI host/pins:
Touch controller, SPI host/pins, IRQ, and calibration:
SD-card SPI sharing:
Backlight polarity/control:
Free GPIOs and input-only pins:
Existing libraries/components and versions:
Hardware facts still unverified:
```

Inspect, when present:

- `platformio.ini`
- `idf_component.yml`
- `CMakeLists.txt`
- `sdkconfig`, `sdkconfig.defaults`
- `partitions.csv`
- `boards/*.json`
- `lib_deps`, lock files, component manifests
- `User_Setup.h`, `User_Setup_Select.h`, display bus configuration
- LVGL configuration (`lv_conf.h` or Kconfig)
- pin maps, schematics, README, serial logs, photos, and board silkscreen

## Source freshness gate

For every external recommendation:

1. Locate primary documentation for the exact framework/library **stable release** in use.
2. Record URL, document branch/version, retrieval date, and the claim supported.
3. Prefer stable docs over “latest/master” docs for implementation. “Latest/master” may describe unreleased or changing behavior.
4. If only community documentation describes the CYD wiring, corroborate it with the repository, schematic, continuity check, or a minimal hardware probe.
5. If sources conflict, stop and present the conflict. Do not pick the more convenient answer.

Use `references.json` as the machine-readable evidence baseline and update it when dependencies change.

## Architecture guidance

Separate concerns:

```text
main/app orchestration
  platform services: Wi-Fi, time, storage, OTA, logging
  domain/application logic
  UI controller and view state
  board support package
    display bus/driver
    touch driver/calibration
    backlight
    SD card
    LEDs/sensors
```

Rules:

- Put all board pins and electrical assumptions in one board-support module.
- Keep UI callbacks short. Send work to a queue/task rather than performing network or storage operations in event handlers.
- Avoid `delay()` and unbounded polling in application paths. Use events, timers, task notifications, queues, or bounded waits appropriate to the selected framework.
- Assign ownership for shared SPI buses. Protect shared devices and avoid reconfiguring a bus behind another driver.
- Use bounded buffers and check every allocation that can fail.
- Check and propagate errors. In ESP-IDF, use documented `esp_err_t` patterns rather than ignoring return values.
- Treat watchdog resets as design defects to diagnose, not suppress globally.

## Display and touch guidance

- Confirm the controller and pin map before initializing the panel.
- Configure rotation once and keep display, touch transform, and application coordinates consistent.
- Calibrate resistive touch on the physical unit. Store calibration with a version/checksum so orientation or hardware changes invalidate stale values.
- Use partial invalidation and appropriately sized draw buffers. Do not assume a full-screen framebuffer fits alongside networking, TLS, fonts, and application state.
- Keep large images/fonts out of internal RAM when possible; measure flash and heap impact.
- Throttle redraws. Update widgets only when state changes.
- If using LVGL, follow the documentation for the **pinned LVGL major version**. Do not mix v8 and v9 APIs or configuration examples.

## Networking, security, and updates

- Never commit Wi-Fi credentials, tokens, private keys, or production endpoints.
- Separate development provisioning from production provisioning.
- Use TLS server verification. Do not ship “insecure” certificate bypasses.
- Design the partition table before promising OTA. ESP-IDF OTA requires an OTA data partition and suitable application slots; use rollback-capable designs where appropriate.
- Validate an update before marking it good, and preserve a recoverable image.
- Store configuration in NVS when appropriate; define schema/version migration and reset behavior.
- Enable security features only after validating lifecycle implications for the actual product and hardware. Do not casually enable secure boot, flash encryption, or irreversible eFuses on a development board.
- Log enough to diagnose boot reason, reset reason, heap pressure, network state, and update state without logging secrets.

## Memory and concurrency

- Capture baseline free heap, minimum-ever free heap, task stacks, flash use, and largest free block.
- Budget memory for worst-case TLS/network activity plus UI rendering.
- Avoid Arduino `String` churn in long-running paths when fragmentation is plausible; prefer bounded or owned data structures appropriate to the framework.
- Define which task/context owns UI calls. Do not call UI APIs from arbitrary worker tasks unless the framework and port explicitly allow it.
- Use timeouts. Every queue receive, network operation, peripheral wait, and retry loop needs a bounded failure path.
- Back off retries and surface terminal failure states to the UI/log.

## Build and verification contract

The harness must execute the applicable commands already declared by the repository, such as:

```text
idf.py set-target esp32
idf.py build
idf.py size
idf.py size-components
```

or:

```text
pio run
pio check
pio test -e <environment>
```

Do not claim successful compilation unless the command completed successfully. Do not claim display, touch, Wi-Fi, SD, sleep, or OTA behavior unless tested on hardware or supported by captured evidence.

Minimum hardware smoke test:

1. Clean boot with reset reason logged.
2. Display color bars and orientation markers.
3. Touch raw-coordinate view, then calibrated corner/center test.
4. Backlight and RGB LED test, respecting active-high/active-low behavior.
5. SD mount/read/write/unmount test if used.
6. Wi-Fi reconnect test without blocking the UI.
7. Heap and task-stack snapshot after steady state.
8. Power-cycle test.
9. OTA and rollback/recovery test if OTA is in scope.

## Response format for coding tasks

Return work in this sequence:

1. **Verified facts**
2. **Unverified facts / blockers**
3. **Plan and compatibility impact**
4. **Files changed**
5. **Patch/code**
6. **Commands run and exact results**
7. **Hardware tests performed or not performed**
8. **Evidence ledger updates**
9. **Remaining risks**

## Refusal-to-guess examples

Use direct language:

- `UNVERIFIED: The display CS pin is not established by the repository or supplied schematic. I will not generate a pin map yet.`
- `CONFLICT: This example uses LVGL 8 APIs, but the manifest pins LVGL 9. I will adapt from the v9 documentation rather than copy the example.`
- `RUNTIME UNVERIFIED: The project compiles, but no physical device or serial log is available.`
- `BLOCKED: OTA cannot be implemented safely until flash size and partition layout are confirmed.`

## Definition of done

A task is complete only when:

- target and dependency versions are explicit;
- board-specific assumptions are centralized;
- code builds with the repository's real toolchain;
- errors, timeouts, and recovery paths exist;
- secrets are absent from source;
- evidence and unresolved uncertainty are recorded;
- runtime claims match actual hardware evidence.
