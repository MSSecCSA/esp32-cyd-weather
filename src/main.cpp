#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>   // used directly: the touch bus is re-pinned before ts.begin()
#include <XPT2046_Touchscreen.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <rom/rtc.h>
#include "secrets.h"  // defines WIFI_SSID / WIFI_PASS — gitignored, see secrets.h.example
// FreeSansBold24pt7b/FreeSansBold12pt7b (used below) come from TFT_eSPI's own
// Fonts/GFXFF/gfxfont.h, which unconditionally bundles all 44 GFXFF free fonts
// whenever LOAD_GFXFF is defined — do not #include them again here, it redefines
// the same PROGMEM arrays and fails to compile.

// === Pin Definitions ===
#define TOUCH_CS 33
#define TOUCH_IRQ 36
// The XPT2046 has its OWN SPI wiring on this board -- it is NOT on the VSPI default
// pins (18/19/23) that a bare SPI.begin() would claim. Measured on hardware: talking
// to it on the defaults clocks against a floating MISO and returns all-ones
// (x=8191 y=8191 z=4095), which the sanity gate in loop() then silently discarded --
// so touch appeared merely "unreliable" while in fact never working once.
#define TOUCH_SCK  25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
#define LDR_PIN 34  // onboard light-dependent resistor on this CYD board, unused until now

// === Screen layout (320x240, rotation 1) ===
// Every text zone is an explicit rectangle that gets fillRect-cleared before drawing.
// Relying on an opaque text background (setTextColor(fg,bg)) is NOT enough: it paints
// only behind the glyphs actually drawn, so a CENTRED string that gets shorter leaves
// the previous string's tails sticking out on both sides. That is what smeared the city
// name and description together as the rotation advanced.
//
// The bands below are non-overlapping by construction. logLayoutMetrics() measures the
// real rendered widths at boot and shouts if anything no longer fits, so adding a city
// with a long name or a wordier description can't silently reintroduce a collision.
const char *HEADER_TITLE = "WEATHER";   // "WEATHER NOW" rendered ~180px wide in
                                        // FreeSansBold12pt7b and ran under the clock box
const int HDR_X = 5,  HDR_Y = 5,  HDR_W = 310, HDR_H = 26;
const int HDR_TITLE_X   = 12;           // ML_DATUM baseline x for the title
const int HDR_CLOCK_X   = 140;          // clock box starts here; title must end before it
const int HDR_CLOCK_W   = 168;
const int HDR_CLOCK_R   = 300;          // MR_DATUM right edge for the clock string

const int COL_X = 124, COL_W = 191;     // right-hand data column (vertical divider at x=120)
const int COL_CX = COL_X + COL_W / 2;   // centre used by every TC_DATUM draw in the column

const int TEMP_Y  = 44,  TEMP_H  = 59;  // 44..102  big temperature figure
const int DESC_Y  = 103, DESC_H  = 26;  // 103..128 condition text
const int CITY_Y  = 130, CITY_H  = 16;  // 130..145 city name
const int CTIME_Y = 147, CTIME_H = 16;  // 147..162 city local time (was 140, overlapping)

// --- Alternate "clock" view: roughly 70% clock / 30% weather ---
// The split lands at y=168 (70% of 240). Font 8 is the 75px face and carries ONLY
// "1234567890:-." -- no letters -- so the AM/PM tag is drawn beside it in font 4
// rather than being part of the time string.
const int CLK_TIME_Y  = 30,  CLK_TIME_H = 84;   // 30..113   big HH:MM
const int CLK_DATE_Y  = 120, CLK_DATE_H = 28;   // 120..147  weekday + date
const int CLK_SEP_Y   = 160;                    // hairline between the halves
const int STRIP_X = 5, STRIP_Y = 170, STRIP_W = 310, STRIP_H = 58;  // 170..227
const int STRIP_ICON_CX = 44, STRIP_ICON_CY = 199, STRIP_ICON_SCALE = 15;
const int STRIP_TEXT_X = 86;  // 147..162 city local time (was 140, overlapping)

// === Backlight (PWM via LEDC on TFT_BL, see build_flags) ===
const int BACKLIGHT_CHANNEL = 0;
const int BACKLIGHT_FREQ = 5000;      // Hz — must be >=1kHz to avoid visible/flicker-sensor issues
const int BACKLIGHT_RES_BITS = 8;     // 0-255 duty range
const int BACKLIGHT_MAX_DUTY = 230;   // ~90%, not 255 — CYD's backlight regulator runs hot at 100%
const int BACKLIGHT_MIN_DUTY = 30;    // dim but still legible in a dark room
const int BACKLIGHT_NIGHT_CEILING = 110; // cap even if the room is lit, during home-local night hours
// LDR polarity varies by wiring revision — if brightness moves the wrong way on your board, flip this.
// === Touch calibration (measured on this unit, rotation 1, 320x240) ===
// Five-point fit; worst residual 6.3 px, axes aligned (no swap) and neither inverted.
// Raw span seen: x 466..3437, y 618..3468. Re-run the calibration rig if the panel or
// the rotation ever changes -- these are specific to THIS board.
const float TOUCH_AX = 0.089410f, TOUCH_BX = -14.903f;  // screenX = AX*raw + BX
const float TOUCH_AY = 0.064770f, TOUCH_BY = -13.698f;  // screenY = AY*raw + BY

// === Ambient light sensor: R21, WORKS -- but only at 0 dB ADC attenuation ===
// R21 on the silkscreen is an LDR (a photoresistor is a resistor, hence the R), part
// GT36516, wired GPIO 34 -> GROUND with a pull-up divider to 3V3 (R15/R19, reported 1M
// each; single-sourced, unconfirmed). Because it goes to ground, DARK READS HIGH.
//
// Measured on this board, backlight on at duty 220, covering vs uncovering R21:
//     attenuation   covered(dark)   uncovered(lit)
//     0 dB              2500              0
//     2.5 dB            1952              0
//     6 dB              1391              0
//     11 dB              746              0
// The whole signal lives between ~142 mV and ~740 mV. Arduino's DEFAULT 11 dB stretches
// the ADC to ~2.5 V full scale, squashing that band into the bottom of the range; 0 dB
// scales to ~1.1 V and fits it almost exactly. Same sensor, 3.3x the usable resolution,
// which is why this pin looked dead until the attenuation was set explicitly.
//
// Known limits, accepted deliberately:
//   * The bright end is compressed: anything from "lit room" upward reads 0, so this
//     cannot tell a bright room from direct sunlight. It CAN tell dark from lit, which
//     is the only distinction the night-dimming feature actually needs.
//   * Backlight spill from the panel edge falls on R21, so the sensor partly sees the
//     display it is controlling. Covering R21 still produced a strong response at duty
//     220, so the loop is not saturated, and the existing /8 smoothing in
//     updateBacklight() damps any oscillation. Watch for slow hunting in a dark room.
//   * Soldering ~51k in parallel with R15 would decompress the bright end. Not needed
//     for night dimming; revisit only if proportional daylight sensing is ever wanted.
const bool LDR_PRESENT = true;
const bool LDR_HIGHER_MEANS_BRIGHTER = false;  // LDR to GND: dark reads HIGH
const int  LDR_RAW_DARK = 2500;   // measured reading when fully covered, at ADC_0db
const int  BACKLIGHT_DEFAULT_DUTY = 200;  // used while no light sensor is available
const unsigned long BRIGHTNESS_UPDATE_INTERVAL = 2000; // 2 sec

// === Heap watchdog tuning ===
const uint32_t LOW_HEAP_REBOOT_THRESHOLD = 60000;      // reboot below this (bytes free) — see checkHeapHealth()
const unsigned long HEAP_CHECK_INTERVAL = 10000;       // 10 sec

// === Multi-City Config ===
struct City {
  float lat;
  float lon;
  const char* name;
  const char* tz;  // POSIX timezone string for local time
};

City cities[] = {
  {38.9159,  -84.2432,  "California, KY",     "EST5EDT,M3.2.0,M11.1.0"},
  {48.8566,    2.3522,  "Paris, France",      "CET-1CEST,M3.5.0,M10.5.0/3"},
  {35.2271,  -80.8431,  "Charlotte, NC",     "EST5EDT,M3.2.0,M11.1.0"},
  {47.6062, -122.3321,  "Seattle, WA",       "PST8PDT,M3.2.0,M11.1.0"},
  {-3.3869,  36.6830,  "Arusha, Tanzania",   "EAT-3"}
};
const int NUM_CITIES = sizeof(cities) / sizeof(cities[0]);
int currentCityIndex = 0;

// === Time zones: one setenv(), fixed-width values ===
// The home zone, used by the header clock and the night-brightness ceiling.
const char *HOME_TZ = "EST5EDT,M3.2.0,M11.1.0";

// Every TZ string this firmware sets is right-padded with spaces to exactly this many
// characters before reaching setenv(). That is load-bearing, not cosmetic: it is the
// fix for a heap leak that drained ~18 bytes/second and forced a watchdog reboot every
// ~3.5 hours.
//
// Measured on this board (isolated probe, 1000 setenv calls per phase, heap diffed
// before/after, so the figure is bytes *per call* rather than per second):
//     same value repeatedly ............................  0.00 bytes/call
//     different value, SAME strlen (EST5EDT.. -> PST8PDT..)  0.00 bytes/call
//     different strlen (22 <-> 26) ..................... 24.00 bytes/call
//     different strlen, with tzset() removed ........... 24.00 bytes/call
// The last line is the important one: tzset() is innocent, the leak is entirely in
// setenv(). newlib compares strlen(new value) against strlen(old value) rather than
// against the size of the block it already allocated, so a longer value forces a fresh
// allocation that is never reclaimed; a shorter value is copied in place but leaves the
// recorded length short, so the next longer value allocates again. The per-second clock
// tick alternated home (22 chars) with the current city -- Paris is 26 and Arusha is 5 --
// which is why only those two cities leaked, and why the average came out near 19 B/s
// rather than 48. Holding strlen constant makes every write an in-place copy.
//
// Trailing-space padding is semantically neutral here: verified on hardware against the
// unpadded strings at six instants -- midwinter, midsummer, and both sides of the US and
// EU DST fallbacks -- with identical rendered time and identical tm_isdst in all 24
// comparisons. Do not "tidy" the padding away.
const size_t TZ_PADDED_LEN = 31;

// The ONLY setenv() in this firmware. Keep it that way: grep for setenv before adding
// any time-reading code and call this instead. The buffer is deliberately stack-local --
// setenv() copies the value into its own storage, but a shared static buffer would make
// that assumption load-bearing for no gain.
void setTimezone(const char *tz) {
  char padded[TZ_PADDED_LEN + 1];
  size_t n = strlen(tz);
  if (n > TZ_PADDED_LEN) n = TZ_PADDED_LEN; // reported at boot by checkTimezoneLengths()
  memset(padded, ' ', TZ_PADDED_LEN);
  memcpy(padded, tz, n);
  padded[TZ_PADDED_LEN] = '\0';
  setenv("TZ", padded, 1);
  tzset();
}

// A TZ string longer than TZ_PADDED_LEN gets silently truncated above, and a truncated
// POSIX rule still parses -- it just parses to a *different* rule, so the failure mode is
// a plausible-looking but wrong clock rather than a crash. Say so loudly at boot instead.
void checkTimezoneLengths() {
  size_t longest = strlen(HOME_TZ);
  if (longest > TZ_PADDED_LEN) {
    Serial.printf("*** TZ TOO LONG: home \"%s\" is %u chars > TZ_PADDED_LEN %u\n",
                  HOME_TZ, (unsigned)longest, (unsigned)TZ_PADDED_LEN);
  }
  for (int i = 0; i < NUM_CITIES; i++) {
    size_t n = strlen(cities[i].tz);
    if (n > longest) longest = n;
    if (n > TZ_PADDED_LEN) {
      Serial.printf("*** TZ TOO LONG: %s \"%s\" is %u chars > TZ_PADDED_LEN %u -- clock will be wrong\n",
                    cities[i].name, cities[i].tz, (unsigned)n, (unsigned)TZ_PADDED_LEN);
    }
  }
  Serial.printf("tz: longest string %u chars, padded to %u (headroom %d)\n",
                (unsigned)longest, (unsigned)TZ_PADDED_LEN,
                (int)TZ_PADDED_LEN - (int)longest);
}

// === Globals ===
TFT_eSPI tft = TFT_eSPI();
// A full 320x240x16bpp back buffer (150KB) was tried first but reliably failed to
// allocate on real hardware — WiFi/TLS leave the heap fragmented enough that no
// contiguous 150KB block exists, even right after boot (confirmed via serial log
// and heap_caps_get_largest_free_block(), see updateBacklight()'s neighbor below).
// Icons are the only element that isn't already a self-erasing opaque draw (they're
// built from many overlapping shapes of varying footprint), so only *that* zone gets
// a small dedicated sprite; everything else draws straight to the panel with an
// explicit opaque background so old content can't ghost through.
const int ICON_ZONE_X = 6, ICON_ZONE_Y = 39, ICON_ZONE_W = 113, ICON_ZONE_H = 130;
TFT_eSprite iconSprite = TFT_eSprite(&tft);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
// Persistent across fetches. With the previous HTTPClient::begin(url) form, HTTPClient
// allocated and destroyed its own WiFiClient on every call; holding one here lets
// HTTP/1.1 keep-alive actually reuse a single TCP connection. See fetchWeather().
WiFiClient netClient;

bool wifiConnected = false;
// Fixed-size buffers rather than Arduino String: these are rewritten on every fetch
// (every 10s, forever), and the esp32-cyd-engineering skill specifically warns against
// String churn in long-running paths. TFT_eSPI has const char* drawString() overloads,
// so nothing downstream needs String.
char weatherTemp[12] = "--";
int weatherCodeInt = 0;
char weatherDesc[24] = "--";
char weatherHumidity[8] = "--";
char weatherWind[12] = "--";
// True only while weatherTemp/Humidity/Wind/CodeInt and sunrise/sunset all describe
// cities[currentCityIndex]. Without this, a failed fetch left the PREVIOUS city's
// numbers on screen underneath the NEW city's name — e.g. a failed Paris fetch showed
// Charlotte's 68F labelled "Paris, France". Wrong data is worse than absent data.
bool weatherDataValid = false;
const int WEATHER_CODE_UNKNOWN = -1; // falls through drawWeatherIcon()'s "?" branch
int sunriseMinutes = 360;  // minutes-since-midnight fallback (6:00 AM) until first fetch succeeds
int sunsetMinutes = 1080;  // fallback (6:00 PM)
unsigned long lastWeatherUpdate = 0;
unsigned long lastCitySwitch = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastClockTick = 0;
unsigned long lastBrightnessUpdate = 0;
unsigned long lastHeapCheck = 0;
int currentBacklightDuty = 200; // smoothed toward target each update, see updateBacklight()
const unsigned long CITY_SWITCH_INTERVAL = 10000; // 10 sec
const unsigned long WIFI_CHECK_INTERVAL = 5000;   // 5 sec
const unsigned long CLOCK_TICK_INTERVAL = 1000;   // 1 sec
bool autoRotatePaused = false;
bool showingDiagnostics = false;

// Two full-screen layouts. They share no zones, so switching between them does a single
// full fillScreen -- the per-zone erase discipline used within each layout cannot clean
// up after the other one.
enum ViewMode : uint8_t { VIEW_WEATHER = 0, VIEW_CLOCK = 1 };
ViewMode currentView = VIEW_WEATHER;

// --- Touch gesture tuning (raw XPT2046 units, 0-4095 range) ---
const unsigned long LONG_PRESS_MS = 700;   // hold this long, without drifting, to toggle pause
// Both thresholds are in SCREEN PIXELS now that touch is calibrated -- they used to be
// raw ADC counts. Converted with the fitted scale (0.0894 px/count on x): the old 300
// and 500 raw correspond to ~25 px and ~45 px.
const int LONG_PRESS_MAX_DRIFT = 25;       // max |dx|+|dy| drift still counted as a long-press
const int SWIPE_MIN_DELTA = 45;            // min horizontal travel to count as a swipe, not a tap
const unsigned long DOUBLE_TAP_WINDOW_MS = 400; // 2nd tap within this window opens diagnostics

// === Weather code mapping (WMO codes) ===
// Returns a pointer to a string literal (static storage) — no allocation, unlike the
// previous String-returning version which built a fresh heap String on every fetch.
const char* wmoToString(int code) {
  if (code == 0) return "Clear sky";
  if (code == 1) return "Mainly clear";
  if (code == 2) return "Partly cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Foggy";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Rain showers";
  if (code >= 85 && code <= 86) return "Snow showers";
  if (code >= 95) return "Thunderstorm";
  return "Unknown";
}

// === Backlight: LDR ambient reading + home-local night ceiling, smoothed ===
int computeTargetBacklightDuty() {
  int duty = BACKLIGHT_DEFAULT_DUTY;
  if (LDR_PRESENT) {
    // Scale against the MEASURED dark reading, not the ADC's full 4095 span -- this
    // sensor never gets near 4095, so using it would waste most of the range.
    int raw = analogRead(LDR_PIN);
    float darkFrac = constrain(raw / (float)LDR_RAW_DARK, 0.0f, 1.0f);
    float lightFrac = LDR_HIGHER_MEANS_BRIGHTER ? darkFrac : 1.0f - darkFrac;
    duty = BACKLIGHT_MIN_DUTY + (int)(lightFrac * (BACKLIGHT_MAX_DUTY - BACKLIGHT_MIN_DUTY));
  }

  setTimezone(HOME_TZ);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 50)) {
    bool nightHours = (timeinfo.tm_hour >= 22 || timeinfo.tm_hour < 7);
    if (nightHours && duty > BACKLIGHT_NIGHT_CEILING) {
      duty = BACKLIGHT_NIGHT_CEILING;
    }
  }

  return constrain(duty, BACKLIGHT_MIN_DUTY, BACKLIGHT_MAX_DUTY);
}

void updateBacklight() {
  int target = computeTargetBacklightDuty();
  // Smooth toward target so a passing shadow over the LDR doesn't visibly flash the panel.
  currentBacklightDuty += (target - currentBacklightDuty) / 8;
  ledcWrite(BACKLIGHT_CHANNEL, currentBacklightDuty);
}

// === Boot diagnostics ===
// Logged once at startup so a field failure can be diagnosed from a serial capture
// alone: why it restarted, what silicon/flash it actually is, and the heap baseline
// every later "Heap:" line should be compared against. The chip/flash values are read
// from the SoC at runtime rather than assumed from the board definition — this is the
// evidence that backs the hardware claims in CLAUDE.md.
const char* resetReasonName(RESET_REASON r) {
  switch (r) {
    case POWERON_RESET:      return "POWERON";
    case SW_RESET:           return "SW";
    case OWDT_RESET:         return "OWDT";
    case DEEPSLEEP_RESET:    return "DEEPSLEEP";
    case SDIO_RESET:         return "SDIO";
    case TG0WDT_SYS_RESET:   return "TG0WDT_SYS";
    case TG1WDT_SYS_RESET:   return "TG1WDT_SYS";
    case RTCWDT_SYS_RESET:   return "RTCWDT_SYS";
    case INTRUSION_RESET:    return "INTRUSION";
    case TGWDT_CPU_RESET:    return "TGWDT_CPU";
    case SW_CPU_RESET:       return "SW_CPU";
    case RTCWDT_CPU_RESET:   return "RTCWDT_CPU";
    case EXT_CPU_RESET:      return "EXT_CPU";
    case RTCWDT_BROWN_OUT_RESET: return "BROWNOUT";
    case RTCWDT_RTC_RESET:   return "RTCWDT_RTC";
    default:                 return "UNKNOWN";
  }
}

void logBootDiagnostics() {
  Serial.println("--- boot diagnostics ---");
  Serial.printf("reset_reason: cpu0=%s cpu1=%s\n",
                resetReasonName(rtc_get_reset_reason(0)),
                resetReasonName(rtc_get_reset_reason(1)));
  Serial.printf("chip: %s rev=%d cores=%d cpu=%uMHz\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("flash_chip_size=%u sketch=%u free_sketch_space=%u\n",
                (unsigned)ESP.getFlashChipSize(),
                (unsigned)ESP.getSketchSize(),
                (unsigned)ESP.getFreeSketchSpace());
  // PSRAM is not declared for the esp32dev board target; log it rather than assume.
  Serial.printf("psram_size=%u (0 = none present/enabled)\n", (unsigned)ESP.getPsramSize());
  Serial.printf("heap: free=%u largest_block=%u min_free_ever=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)ESP.getMinFreeHeap());
  Serial.printf("mac: %s\n", WiFi.macAddress().c_str());
  Serial.println("------------------------");
}

// === Heap health watchdog ===
// Measured on real hardware: free heap drops by roughly 400 bytes per fetch+redraw
// cycle, and stays perfectly flat when auto-rotate is paused (no fetches happening)
// — so it's specifically tied to fetchWeather()'s HTTP/JSON work, not the sprite
// redraw, the clock tick, or the backlight update. Switching HTTPS to plain HTTP
// (see fetchWeather()) did NOT change the rate, which rules out TLS/mbedTLS as the
// cause. The true source (arduino-esp32 HTTPClient buffer handling? ArduinoJson?
// lwIP TCP TIME_WAIT-style resource accumulation that might plateau given enough
// real time?) needs a much longer observation window than was practical to test —
// see CLAUDE.md for the full writeup. Whatever it is, left unchecked it would
// exhaust heap within a few hours of continuous operation, so rather than wait for
// an unpredictable crash, reboot cleanly well before it gets critical — this resets
// WiFi state completely and the whole cycle recovers in setup() same as power-on.
void checkHeapHealth() {
  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("Heap: free=%u largest_free_block=%u\n",
                (unsigned)freeHeap,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (freeHeap < LOW_HEAP_REBOOT_THRESHOLD) {
    Serial.println("Free heap critically low -- rebooting to recover");
    Serial.flush();
    delay(100);
    ESP.restart();
  }
}

// === Icon rendering helpers ===

void drawShadedCircle(TFT_eSPI &d, int cx, int cy, int r, uint16_t baseColor, uint16_t highlightColor) {
  d.fillCircle(cx, cy, r, baseColor);
  d.fillCircle(cx - r/4, cy - r/4, r/2 - 1, highlightColor);
}

void drawCloud(TFT_eSPI &d, int cx, int cy, int scale, uint16_t color, uint16_t shadowColor) {
  int s = scale;
  d.fillCircle(cx - 8*s/10, cy + 2*s/10, 7*s/10, shadowColor);
  d.fillCircle(cx, cy + 2*s/10, 8*s/10, shadowColor);
  d.fillCircle(cx + 8*s/10, cy + 2*s/10, 7*s/10, shadowColor);
  d.fillCircle(cx + 4*s/10, cy + 3*s/10, 6*s/10, shadowColor);

  d.fillCircle(cx - 8*s/10, cy, 7*s/10, color);
  d.fillCircle(cx - 3*s/10, cy - 3*s/10, 8*s/10, color);
  d.fillCircle(cx + 3*s/10, cy - 2*s/10, 7*s/10, color);
  d.fillCircle(cx + 8*s/10, cy + 1*s/10, 6*s/10, color);
  d.fillCircle(cx, cy + 1*s/10, 9*s/10, color);

  uint16_t hl = d.color565(220, 220, 230);
  d.fillCircle(cx - 3*s/10, cy - 5*s/10, 3*s/10, hl);
  d.fillCircle(cx + 3*s/10, cy - 4*s/10, 2*s/10, hl);
}

void drawSun(TFT_eSPI &d, int cx, int cy, int scale) {
  uint16_t sunCore = TFT_YELLOW;
  uint16_t sunGlow = d.color565(255, 200, 60);
  uint16_t sunRay = d.color565(255, 180, 30);
  int s = scale;

  d.fillCircle(cx, cy, s + 6, sunGlow);

  for (int i = 0; i < 12; i++) {
    float angle = i * PI / 6;
    int rayLen = (i % 2 == 0) ? s + 14 : s + 8;
    float a1 = angle - 0.08;
    float a2 = angle + 0.08;
    int x1b = cx + cos(a1) * (s + 2);
    int y1b = cy + sin(a1) * (s + 2);
    int x2b = cx + cos(a2) * (s + 2);
    int y2b = cy + sin(a2) * (s + 2);
    int xt = cx + cos(angle) * rayLen;
    int yt = cy + sin(angle) * rayLen;
    d.fillTriangle(x1b, y1b, x2b, y2b, xt, yt, sunRay);
  }

  drawShadedCircle(d, cx, cy, s, sunCore, d.color565(255, 255, 180));
}

void drawMoon(TFT_eSPI &d, int cx, int cy, int scale) {
  // Crescent moon: light disc with a dark "bite" offset-drawn over it,
  // plus a soft glow ring and a couple of static stars for a night sky feel.
  uint16_t glow = d.color565(80, 90, 130);
  uint16_t moonColor = d.color565(230, 230, 210);
  uint16_t shadow = TFT_BLACK;
  int s = scale;

  d.fillCircle(cx, cy, s + 5, glow);
  d.fillCircle(cx, cy, s, moonColor);
  d.fillCircle(cx + s/2, cy - s/4, s, shadow);

  uint16_t starColor = d.color565(255, 255, 220);
  d.fillCircle(cx - s - 8, cy - s/2, 1, starColor);
  d.fillCircle(cx - s - 2, cy + s/2 + 4, 1, starColor);
  d.fillCircle(cx + s + 10, cy + s/3, 1, starColor);
}

void drawRaindrop(TFT_eSPI &d, int x, int y, int len, uint16_t color) {
  d.fillTriangle(x - 2, y, x + 2, y, x, y + len, color);
  d.drawLine(x, y - 2, x, y, color);
}

void drawSnowflake(TFT_eSPI &d, int cx, int cy, int r, uint16_t color) {
  for (int i = 0; i < 6; i++) {
    float angle = i * PI / 3;
    int x2 = cx + cos(angle) * r;
    int y2 = cy + sin(angle) * r;
    d.drawLine(cx, cy, x2, y2, color);
    int bx = cx + cos(angle) * r * 0.6;
    int by = cy + sin(angle) * r * 0.6;
    int bx1 = bx + cos(angle + PI/3) * r * 0.3;
    int by1 = by + sin(angle + PI/3) * r * 0.3;
    int bx2 = bx + cos(angle - PI/3) * r * 0.3;
    int by2 = by + sin(angle - PI/3) * r * 0.3;
    d.drawLine(bx, by, bx1, by1, color);
    d.drawLine(bx, by, bx2, by2, color);
  }
  d.fillCircle(cx, cy, 2, color);
}

void drawLightning(TFT_eSPI &d, int cx, int cy, int scale, uint16_t color) {
  int s = scale;
  d.fillTriangle(cx - 2, cy - s, cx + 6, cy - s/3, cx - 2, cy - s/4, color);
  d.fillTriangle(cx + 6, cy - s/3, cx - 4, cy + s, cx - 2, cy - s/4, color);
  d.fillTriangle(cx + 6, cy - s/3, cx + 3, cy + s/4, cx - 4, cy + s, color);
  uint16_t glow = d.color565(255, 255, 100);
  d.fillTriangle(cx - 1, cy - s + 2, cx + 3, cy - s/3, cx, cy - s/4, glow);
}

void drawWeatherIcon(TFT_eSPI &d, int cx, int cy, int scale, int code, bool isNight) {
  uint16_t cloudColor = d.color565(180, 180, 195);
  uint16_t cloudShadow = d.color565(120, 120, 135);
  uint16_t darkCloud = d.color565(100, 100, 115);
  uint16_t darkCloudShadow = d.color565(60, 60, 75);

  if (code == 0) {
    if (isNight) drawMoon(d, cx, cy, scale * 7 / 10);
    else drawSun(d, cx, cy, scale);
  } else if (code == 1 || code == 2) {
    if (isNight) drawMoon(d, cx - scale/3, cy - scale/3, scale / 2);
    else drawSun(d, cx - scale/3, cy - scale/3, scale * 7 / 10);
    drawCloud(d, cx + scale/4, cy + scale/5, scale * 6 / 10, cloudColor, cloudShadow);
  } else if (code == 3) {
    drawCloud(d, cx, cy, scale, cloudColor, cloudShadow);
  } else if (code == 45 || code == 48) {
    uint16_t fogColor = d.color565(160, 160, 175);
    uint16_t fogLight = d.color565(200, 200, 210);
    for (int i = 0; i < 6; i++) {
      int y = cy - scale/2 + i * (scale / 5);
      int offset = (i % 2 == 0) ? 0 : 5;
      d.drawFastHLine(cx - scale + offset, y, scale * 2 - 8, fogColor);
      d.drawFastHLine(cx - scale + offset + 3, y + 2, scale * 2 - 12, fogLight);
    }
  } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    drawCloud(d, cx, cy - scale/3, scale * 8 / 10, cloudColor, cloudShadow);
    uint16_t rainColor = d.color565(60, 120, 255);
    uint16_t rainLight = d.color565(120, 180, 255);
    for (int i = 0; i < 5; i++) {
      int rx = cx - scale * 7/10 + i * (scale * 14 / 40);
      int ry = cy + scale/4;
      int rl = scale / 3;
      drawRaindrop(d, rx, ry, rl, rainColor);
      d.drawLine(rx, ry, rx, ry + rl/2, rainLight);
    }
  } else if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
    drawCloud(d, cx, cy - scale/3, scale * 8 / 10, cloudColor, cloudShadow);
    uint16_t snowColor = TFT_WHITE;
    uint16_t iceBlue = d.color565(200, 230, 255);
    drawSnowflake(d, cx - scale/2, cy + scale/3, scale/4, iceBlue);
    drawSnowflake(d, cx, cy + scale/2, scale/5, snowColor);
    drawSnowflake(d, cx + scale/2, cy + scale/3, scale/4, iceBlue);
  } else if (code >= 95) {
    drawCloud(d, cx, cy - scale/4, scale * 8 / 10, darkCloud, darkCloudShadow);
    drawLightning(d, cx, cy + scale/3, scale/2, TFT_YELLOW);
  } else {
    d.drawCircle(cx, cy, scale, TFT_WHITE);
    d.setTextColor(TFT_WHITE);
    d.setTextDatum(MC_DATUM);
    d.drawChar('?', cx, cy, TFT_WHITE, TFT_BLACK, 4);
  }
}

// Parses "YYYY-MM-DDTHH:MM" (Open-Meteo's local-time format) to minutes-since-midnight.
// Returns -1 if the string doesn't look like the expected shape.
int isoTimeToMinutes(const char* iso) {
  if (iso == nullptr) return -1;
  const char* t = strchr(iso, 'T');
  if (t == nullptr || strlen(t) < 6) return -1;
  int hh = (t[1] - '0') * 10 + (t[2] - '0');
  int mm = (t[4] - '0') * 10 + (t[5] - '0');
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
  return hh * 60 + mm;
}

// Is it currently night at the city on screen, per that fetch's sunrise/sunset?
// Approximate: compares our hardcoded POSIX TZ clock against Open-Meteo's
// auto-resolved IANA timezone for the same coordinates — the two agree on local
// time except in rare DST-transition edge cases.
bool isCurrentCityNight() {
  // sunrise/sunsetMinutes belong to whichever city last fetched successfully, so they
  // are only meaningful while that data is still current — otherwise we'd tint the
  // panel using another city's sun times.
  if (!weatherDataValid) return false;
  setTimezone(cities[currentCityIndex].tz);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) return false; // default to the day icon if time is unavailable
  int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  return (nowMinutes < sunriseMinutes) || (nowMinutes >= sunsetMinutes);
}

// Drop any weather state belonging to a previously-displayed city. Called whenever a
// fetch is about to run or has failed, so the UI can render "--" instead of another
// city's readings. `reason` becomes the on-screen description line.
void invalidateWeatherData(const char* reason) {
  weatherDataValid = false;
  strlcpy(weatherTemp, "--", sizeof(weatherTemp));
  strlcpy(weatherHumidity, "--", sizeof(weatherHumidity));
  strlcpy(weatherWind, "--", sizeof(weatherWind));
  weatherCodeInt = WEATHER_CODE_UNKNOWN;
  strlcpy(weatherDesc, reason, sizeof(weatherDesc));
  // Deliberately NOT resetting sunrise/sunsetMinutes to a fixed default here: they are
  // only consulted via isCurrentCityNight(), which is itself gated on weatherDataValid.
}

// === Fetch weather for a specific city ===
void fetchWeather(int cityIdx) {
  // Anything still in the globals describes the *previous* city from this point on.
  invalidateWeatherData("Loading...");

  if (WiFi.status() != WL_CONNECTED) {
    invalidateWeatherData("No WiFi");
    Serial.println("Skipping fetch, WiFi not connected");
    return;
  }

  HTTPClient http;
  City c = cities[cityIdx];

  // NOTE: plain HTTP. This is public, unauthenticated, non-sensitive data and no
  // credentials are transmitted, so the exposure is limited to someone on-path being
  // able to alter a displayed temperature. It is nonetheless a deliberate deviation
  // from the esp32-cyd-engineering skill's "use TLS server verification" rule, taken
  // knowingly rather than by accident:
  //   - The previous https:// form was NOT actually verifying anything — HTTPClient's
  //     begin(url) path constructs a WiFiClientSecure and calls setInsecure(), so it
  //     was an unauthenticated TLS tunnel, which the same rule explicitly forbids.
  //   - Real verification is available here (WiFiClientSecure::setCACertBundle exists
  //     in this pinned core, and api.open-meteo.com chains to ISRG Root X1), so this
  //     is a revisitable choice, not a dead end. See CLAUDE.md "Transport security".
  // If OTA is ever added, verified TLS becomes mandatory — you'd be accepting code.
  // Built with snprintf into a stack buffer — the previous String concatenation chain
  // allocated ~8 heap temporaries per fetch.
  char url[320];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
           "&daily=sunrise,sunset&forecast_days=1"
           "&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto",
           c.lat, c.lon);

  Serial.printf("Fetching [%s]\n", c.name);
  // Pass the persistent netClient rather than letting HTTPClient create its own
  // WiFiClient per call, and leave HTTP/1.1 keep-alive enabled so the same TCP
  // connection is reused across fetches.
  http.begin(netClient, url);
  http.setReuse(true);
  http.setTimeout(8000); // avoid stalling loop() on a bad connection
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    // getString() (not getStream()) because keep-alive means HTTP/1.1, and Open-Meteo
    // replies with Transfer-Encoding: chunked — getStream() would hand back raw chunk
    // framing, which is not valid JSON (this failed with InvalidInput on hardware).
    // getString() performs the chunked de-framing for us.
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      float temp = doc["current"]["temperature_2m"];
      int humidity = doc["current"]["relative_humidity_2m"];
      int wmoCode = doc["current"]["weather_code"];
      float wind = doc["current"]["wind_speed_10m"];

      snprintf(weatherTemp, sizeof(weatherTemp), "%dF", (int)temp);
      snprintf(weatherHumidity, sizeof(weatherHumidity), "%d%%", humidity);
      snprintf(weatherWind, sizeof(weatherWind), "%dmph", (int)wind);
      weatherCodeInt = wmoCode;
      strlcpy(weatherDesc, wmoToString(wmoCode), sizeof(weatherDesc));

      // Sunrise/sunset come back as local ISO8601 ("2026-09-16T07:02") since
      // timezone=auto — pull just the HH:MM, no date/DST math needed.
      const char* sunriseIso = doc["daily"]["sunrise"][0];
      const char* sunsetIso = doc["daily"]["sunset"][0];
      int sr = isoTimeToMinutes(sunriseIso);
      int ss = isoTimeToMinutes(sunsetIso);
      if (sr >= 0) sunriseMinutes = sr;
      if (ss >= 0) sunsetMinutes = ss;

      // Everything above now describes cities[cityIdx] — safe to display.
      weatherDataValid = true;

      Serial.printf("OK: %s %s [%s]\n", weatherTemp, weatherDesc, c.name);
    } else {
      Serial.printf("JSON parse error: %s\n", error.c_str());
      invalidateWeatherData("Parse Error");
    }
  } else {
    Serial.printf("HTTP error: %d\n", httpCode);
    invalidateWeatherData("HTTP Error");
  }

  http.end();
}

// === WiFi status dot (lower right of info bar) — redrawable standalone ===
void drawWifiStatusDot(TFT_eSPI &d) {
  d.fillCircle(305, 187, 4, wifiConnected ? TFT_GREEN : TFT_RED);
}

// === Clock rendering, split out so it can be refreshed every second ===
// without redoing the full-screen sprite redraw (see loop()'s per-second tick).
void drawHomeClock(TFT_eSPI &d, bool eraseFirst) {
  struct tm timeinfo;
  char timeStr[32];
  setTimezone(HOME_TZ);
  if (getLocalTime(&timeinfo, 100)) {
    strftime(timeStr, sizeof(timeStr), "%I:%M %p  %b %d", &timeinfo);
  } else {
    strcpy(timeStr, "--:--");
  }
  if (eraseFirst) d.fillRect(HDR_CLOCK_X, HDR_Y + 1, HDR_CLOCK_W, HDR_H - 2, TFT_DARKCYAN);
  d.setTextColor(TFT_LIGHTGREY);
  d.setTextDatum(MR_DATUM);
  d.drawString(timeStr, HDR_CLOCK_R, HDR_Y + HDR_H / 2, 2);
}

void drawCityClock(TFT_eSPI &d, bool eraseFirst) {
  setTimezone(cities[currentCityIndex].tz);
  struct tm timeinfo;
  char cityTimeStr[24];
  if (getLocalTime(&timeinfo, 100)) {
    strftime(cityTimeStr, sizeof(cityTimeStr), "%I:%M %p", &timeinfo);
  } else {
    strcpy(cityTimeStr, "--:--");
  }
  if (eraseFirst) d.fillRect(COL_X, CTIME_Y, COL_W, CTIME_H, TFT_BLACK);
  d.setTextColor(TFT_DARKCYAN);
  d.setTextDatum(TC_DATUM);
  d.drawString(cityTimeStr, COL_CX, CTIME_Y, 2);
}

// === Draw the Weather Now screen (LANDSCAPE 320x240) ===
// Draws straight to the panel (no full-frame back buffer — see the note by
// iconSprite's declaration on why that didn't work out). Every zone either
// fully repaints its own opaque rectangle (bars) or sets an explicit text
// background so old content can't ghost through when new content is narrower.
// The one exception is the weather icon, which gets its own small sprite
// since it's built from many overlapping shapes rather than a solid rect.
// Measure what the renderer ACTUALLY produces, rather than trusting estimates. Run once
// at boot: if a new city name or a wordier condition string stops fitting its zone, this
// says so on the serial log instead of silently overlapping something.
void logLayoutMetrics() {
  Serial.println("--- layout metrics (measured, px) ---");

  tft.setFreeFont(&FreeSansBold12pt7b);
  int titleW = tft.textWidth(HEADER_TITLE);
  tft.setTextFont(2);   // GFXFF reset, see CLAUDE.md
  int titleEnd = HDR_TITLE_X + titleW;
  Serial.printf("  header \"%s\": %d wide, ends x=%d, clock box starts x=%d  %s\n",
                HEADER_TITLE, titleW, titleEnd, HDR_CLOCK_X,
                titleEnd < HDR_CLOCK_X ? "OK" : "*** COLLIDES ***");

  int cw = 0; const char *cn = "";
  for (int i = 0; i < NUM_CITIES; i++) {
    int w = tft.textWidth(cities[i].name, 2);
    if (w > cw) { cw = w; cn = cities[i].name; }
  }
  Serial.printf("  widest city \"%s\": %d wide, column is %d  %s\n",
                cn, cw, COL_W, cw <= COL_W ? "OK" : "*** OVERFLOWS ***");

  // The longest condition string the WMO ladder can produce.
  int dw = tft.textWidth("Thunderstorm", 4);
  Serial.printf("  longest condition \"Thunderstorm\": %d wide, column is %d  %s\n",
                dw, COL_W, dw <= COL_W ? "OK" : "*** OVERFLOWS ***");

  int hw = tft.textWidth("07:42 PM  Sep 18", 2);
  Serial.printf("  header clock sample: %d wide, box is %d  %s\n",
                hw, HDR_CLOCK_W, hw <= HDR_CLOCK_W ? "OK" : "*** OVERFLOWS ***");
  Serial.println("  bands: temp 44-102  desc 103-128  city 130-145  ctime 147-162");

  int bigW = tft.textWidth("07:42", 8);
  int dateW = tft.textWidth("Wednesday, Sep 18", 4);
  Serial.printf("  clock view: big time \"07:42\" %d wide (screen 320)  %s\n",
                bigW, bigW <= 300 ? "OK" : "*** TOO WIDE ***");
  Serial.printf("  clock view: longest date \"Wednesday, Sep 18\" %d wide  %s\n",
                dateW, dateW <= 316 ? "OK" : "*** TOO WIDE ***");
}

void drawWeatherScreen() {
  TFT_eSPI &d = tft;

  // --- Title bar --- (anti-aliased free font; see GFXFF note in CLAUDE.md)
  d.fillRoundRect(HDR_X, HDR_Y, HDR_W, HDR_H, 4, TFT_DARKCYAN);
  d.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  d.setTextDatum(ML_DATUM);
  d.setFreeFont(&FreeSansBold12pt7b);
  d.drawString(HEADER_TITLE, HDR_TITLE_X, HDR_Y + HDR_H / 2);
  d.setTextFont(2); // back to classic numbered fonts for the rest of the screen

  // --- Home time and date (right side of title bar) ---
  drawHomeClock(d, true);

  // --- Vertical divider ---
  d.drawFastVLine(120, 38, 125, TFT_DARKGREY);

  // --- Icon zone (left side, moved up) --- composited in its own small sprite
  bool nightNow = isCurrentCityNight();
  uint16_t skyColor = nightNow ? d.color565(8, 10, 28) : TFT_BLACK;
  bool useIconSprite = (iconSprite.createSprite(ICON_ZONE_W, ICON_ZONE_H) != nullptr);
  if (useIconSprite) {
    iconSprite.fillSprite(skyColor);
    // Icon coords are relative to the sprite's own origin, not the panel's.
    drawWeatherIcon((TFT_eSPI &)iconSprite, 62 - ICON_ZONE_X, 85 - ICON_ZONE_Y, 40, weatherCodeInt, nightNow);
    iconSprite.pushSprite(ICON_ZONE_X, ICON_ZONE_Y);
    iconSprite.deleteSprite();
  } else {
    Serial.println("Icon sprite alloc failed, drawing icon directly");
    d.fillRect(ICON_ZONE_X, ICON_ZONE_Y, ICON_ZONE_W, ICON_ZONE_H, skyColor);
    drawWeatherIcon(d, 62, 85, 40, weatherCodeInt, nightNow);
  }

  // --- Temperature zone (moved up) --- anti-aliased free font (see GFXFF note in CLAUDE.md)
  d.fillRect(COL_X, TEMP_Y, COL_W, TEMP_H, TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(TC_DATUM);
  d.setFreeFont(&FreeSansBold24pt7b);
  d.drawString(weatherTemp, COL_CX, TEMP_Y + 11);
  d.setTextFont(2); // back to classic numbered fonts for the rest of the screen

  // --- Description zone (moved up) --- opaque bg so a shorter string erases the old one.
  // Amber instead of cyan when the reading failed, so an error state is distinguishable
  // at a glance from a real forecast rather than reading like just another condition.
  d.fillRect(COL_X, DESC_Y, COL_W, DESC_H, TFT_BLACK);
  d.setTextColor(weatherDataValid ? TFT_CYAN : TFT_ORANGE, TFT_BLACK);
  d.drawString(weatherDesc, COL_CX, DESC_Y, 4);

  // --- City zone --- opaque bg for the same reason
  d.fillRect(COL_X, CITY_Y, COL_W, CITY_H, TFT_BLACK);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.drawString(cities[currentCityIndex].name, COL_CX, CITY_Y, 2);

  // --- City local time (below city name) ---
  drawCityClock(d, true);

  // --- Bottom info bar ---
  d.fillRoundRect(5, 170, 310, 35, 4, TFT_DARKGREY);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(ML_DATUM);
  d.drawString("Humidity:", 15, 187, 2);
  d.drawString(weatherHumidity, 75, 187, 2);
  d.drawString("Wind:", 130, 187, 2);
  d.drawString(weatherWind, 170, 187, 2);

  d.setTextColor(autoRotatePaused ? TFT_ORANGE : TFT_YELLOW);
  d.setTextDatum(MR_DATUM);
  d.drawString(autoRotatePaused ? "Paused" : "Auto 10s", 275, 187, 2);

  // --- WiFi status dot (lower right of info bar) ---
  drawWifiStatusDot(d);

  // --- Footer (tight to bar) --- static text, never changes width, but opaque anyway
  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.setTextDatum(TC_DATUM);
  d.drawString("Tap next | Swipe up: clock | Hold: pause", 160, 213, 1);
}

// === Alternate view: big clock over a condensed weather strip ===
// Roughly a 70/30 vertical split. The point of this view is glanceability from across a
// room, so the time gets Font 8 (75px) and everything else is deliberately secondary.
void drawBigClock(TFT_eSPI &d, bool eraseFirst) {
  setTimezone(HOME_TZ);
  struct tm ti;
  char hhmm[8], ampm[6], datestr[28];
  if (getLocalTime(&ti, 100)) {
    strftime(hhmm,    sizeof(hhmm),    "%I:%M", &ti);
    strftime(ampm,    sizeof(ampm),    "%p", &ti);
    strftime(datestr, sizeof(datestr), "%A, %b %d", &ti);
  } else {
    strlcpy(hhmm, "--:--", sizeof(hhmm));
    ampm[0] = '\0';
    strlcpy(datestr, "no time sync yet", sizeof(datestr));
  }

  if (eraseFirst) {
    d.fillRect(0, CLK_TIME_Y, 320, CLK_TIME_H, TFT_BLACK);
    d.fillRect(0, CLK_DATE_Y, 320, CLK_DATE_H, TFT_BLACK);
  }

  // Nudged left of centre so the AM/PM tag sits beside the digits without pushing the
  // whole block off-centre. Font 8 has no letters, hence the separate draw.
  int tw = d.textWidth(hhmm, 8);
  int cx = 160 - 14;
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(TC_DATUM);
  d.drawString(hhmm, cx, CLK_TIME_Y, 8);

  if (ampm[0]) {
    d.setTextColor(TFT_DARKCYAN, TFT_BLACK);
    d.setTextDatum(BL_DATUM);
    d.drawString(ampm, cx + tw / 2 + 10, CLK_TIME_Y + CLK_TIME_H - 12, 4);
  }

  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.setTextDatum(TC_DATUM);
  d.drawString(datestr, 160, CLK_DATE_Y, 4);
}

// The strip repaints its whole rounded rect every time, so unlike the main view there is
// no per-zone erase to get wrong and no way for a previous city's text to survive.
void drawClockWeatherStrip(TFT_eSPI &d) {
  const uint16_t bg = d.color565(18, 18, 26);
  d.fillRoundRect(STRIP_X, STRIP_Y, STRIP_W, STRIP_H, 5, bg);

  drawWeatherIcon(d, STRIP_ICON_CX, STRIP_ICON_CY, STRIP_ICON_SCALE,
                  weatherCodeInt, isCurrentCityNight());

  d.setTextDatum(ML_DATUM);
  d.setTextColor(TFT_WHITE, bg);
  d.setFreeFont(&FreeSansBold12pt7b);
  d.drawString(weatherTemp, STRIP_TEXT_X, STRIP_Y + 18);
  d.setTextFont(2);   // GFXFF reset, see CLAUDE.md

  d.setTextColor(weatherDataValid ? TFT_CYAN : TFT_ORANGE, bg);
  d.drawString(weatherDesc, STRIP_TEXT_X + 72, STRIP_Y + 18, 2);

  d.setTextColor(TFT_LIGHTGREY, bg);
  d.drawString(cities[currentCityIndex].name, STRIP_TEXT_X, STRIP_Y + 42, 2);

  d.setTextDatum(MR_DATUM);
  d.setTextColor(autoRotatePaused ? TFT_ORANGE : TFT_YELLOW, bg);
  d.drawString(autoRotatePaused ? "Paused" : "Auto 10s", STRIP_X + STRIP_W - 26, STRIP_Y + 42, 2);

  d.fillCircle(STRIP_X + STRIP_W - 13, STRIP_Y + 42, 4, wifiConnected ? TFT_GREEN : TFT_RED);
}

void drawClockScreen() {
  TFT_eSPI &d = tft;
  drawBigClock(d, true);
  d.drawFastHLine(24, CLK_SEP_Y, 272, TFT_DARKGREY);
  drawClockWeatherStrip(d);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setTextDatum(TC_DATUM);
  d.drawString("Swipe up/down for full weather", 160, 230, 1);
}

// Single dispatch point so gesture and timer code never has to know which view is up.
// Three redraw entry points, and picking the wrong one is how the clock view ended up
// with the full weather screen painted across it. Use:
//   drawCurrentView()      - the whole panel needs repainting (view switch, overlay
//                            dismissed, first draw)
//   redrawWeatherContent() - the weather DATA changed (new city, fresh fetch)
//   redrawWifiIndicator()  - only the connection dot changed
// Never call drawWeatherScreen() or drawWifiStatusDot() directly from loop() or the
// gesture handler: both draw at the weather view's coordinates unconditionally, so in
// the clock view they land on top of whatever is already there.
void drawCurrentView() {
  if (currentView == VIEW_CLOCK) drawClockScreen();
  else                           drawWeatherScreen();
}

// Only the strip carries weather in the clock view. Repainting the big clock here too
// would make the time visibly blink on every city change -- every 10s with auto-rotate.
void redrawWeatherContent() {
  if (currentView == VIEW_CLOCK) drawClockWeatherStrip(tft);
  else                           drawWeatherScreen();
}

// The status dot sits at different coordinates in each layout, so repaint whichever
// element actually owns it.
void redrawWifiIndicator() {
  if (currentView == VIEW_CLOCK) drawClockWeatherStrip(tft);
  else                           drawWifiStatusDot(tft);
}

// === Diagnostics overlay (double-tap to open, any tap to dismiss) ===
// Surfaces device health that's otherwise only visible over serial: heap
// headroom, WiFi signal, and flash usage against the huge_app.csv budget.
void drawDiagnosticsScreen() {
  // Drawn directly (no back-buffer sprite) — a full 320x240 buffer doesn't reliably
  // fit in one contiguous heap block on this hardware (see iconSprite's declaration),
  // and this screen only redraws on a deliberate double-tap/dismiss, not on a
  // recurring timer, so the one-time fillScreen flash here is an acceptable trade.
  TFT_eSPI &d = tft;

  d.fillScreen(TFT_BLACK);
  d.fillRoundRect(HDR_X, HDR_Y, HDR_W, HDR_H, 4, TFT_DARKCYAN);
  d.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  d.setTextDatum(ML_DATUM);
  d.setFreeFont(&FreeSansBold12pt7b);
  d.drawString("DIAGNOSTICS", 12, 20);
  d.setTextFont(2);

  d.setTextColor(TFT_WHITE);
  d.setTextDatum(ML_DATUM);

  unsigned long upSec = millis() / 1000;
  char buf[56];
  int y = 45;
  const int lineH = 22;

  snprintf(buf, sizeof(buf), "Uptime: %02lu:%02lu:%02lu",
           upSec / 3600, (upSec / 60) % 60, upSec % 60);
  d.drawString(buf, 15, y, 2); y += lineH;

  snprintf(buf, sizeof(buf), "Free heap: %lu KB (min seen: %lu KB)",
           (unsigned long)(ESP.getFreeHeap() / 1024),
           (unsigned long)(ESP.getMinFreeHeap() / 1024));
  d.drawString(buf, 15, y, 2); y += lineH;

  if (wifiConnected) {
    snprintf(buf, sizeof(buf), "WiFi: connected, %d dBm", WiFi.RSSI());
  } else {
    snprintf(buf, sizeof(buf), "WiFi: disconnected");
  }
  d.drawString(buf, 15, y, 2); y += lineH;

  snprintf(buf, sizeof(buf), "IP: %s",
           wifiConnected ? WiFi.localIP().toString().c_str() : "--");
  d.drawString(buf, 15, y, 2); y += lineH;

  snprintf(buf, sizeof(buf), "Flash used: %lu / %lu KB",
           (unsigned long)(ESP.getSketchSize() / 1024),
           (unsigned long)((ESP.getSketchSize() + ESP.getFreeSketchSpace()) / 1024));
  d.drawString(buf, 15, y, 2); y += lineH;

  snprintf(buf, sizeof(buf), "CPU: %u MHz", ESP.getCpuFreqMHz());
  d.drawString(buf, 15, y, 2); y += lineH;

  snprintf(buf, sizeof(buf), "Backlight duty: %d / 255", currentBacklightDuty);
  d.drawString(buf, 15, y, 2); y += lineH;

  d.setTextColor(TFT_GREEN);
  d.setTextDatum(TC_DATUM);
  d.drawString("Tap to return", 160, 220, 2);
}

// === Draw boot/connecting screen ===
void drawConnectingScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("CYD Weather Station", 160, 80, 4);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Connecting to WiFi...", 160, 120, 2);
  tft.setTextColor(TFT_YELLOW);
  tft.drawString(WIFI_SSID, 160, 145, 2);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== CYD Weather Station (Multi-City) ===");
  logBootDiagnostics();
  checkTimezoneLengths();
  
  tft.init();
  tft.setRotation(1);
  logLayoutMetrics();
  tft.fillScreen(TFT_BLACK);

  // Take over the backlight pin with PWM (tft.init() already set it digitally HIGH via
  // TFT_BACKLIGHT_ON) so brightness can be modulated instead of only on/off.
  ledcSetup(BACKLIGHT_CHANNEL, BACKLIGHT_FREQ, BACKLIGHT_RES_BITS);
  // 0 dB (~1.1V full scale) rather than the 11 dB default -- see the LDR notes above.
  // Without this the sensor's entire range collapses into a handful of ADC counts.
  analogSetPinAttenuation(LDR_PIN, ADC_0db);

  ledcAttachPin(TFT_BL, BACKLIGHT_CHANNEL);
  ledcWrite(BACKLIGHT_CHANNEL, currentBacklightDuty);

  // XPT2046_Touchscreen::begin() calls a bare SPI.begin(), which would claim VSPI's
  // DEFAULT pins -- where the touch chip is not wired. ESP32's SPIClass::begin() returns
  // early when the bus is already initialised, so re-pinning the global SPI object to
  // the touch controller's real pins FIRST makes the library adopt them.
  SPI.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin();
  ts.setRotation(1);

  drawConnectingScreen();
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    dots++;
    if (dots % 4 == 0) Serial.println();
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    String dotStr = "";
    for (int i = 0; i < (dots % 4); i++) dotStr += ". ";
    tft.drawString(dotStr, 160, 175, 4);
    if (dots > 60) {
      tft.setTextColor(TFT_RED);
      tft.drawString("WiFi FAILED!", 160, 210, 2);
      break;
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
    
    // Configure NTP for local time
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    // Set timezone to America/New_York (covers Kentucky area)
    setTimezone(HOME_TZ);
    
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString("WiFi Connected!", 160, 210, 2);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(WiFi.localIP().toString(), 160, 225, 2);
  }
  
  delay(2000);

  // One-time clean slate: drawWeatherScreen() no longer blanks the whole panel on
  // every redraw (each zone self-erases instead, see its comment), so the boot
  // screen's leftover text needs clearing exactly once before the first draw.
  tft.fillScreen(TFT_BLACK);

  // Fetch first city and draw
  fetchWeather(currentCityIndex);
  lastWeatherUpdate = millis();
  lastCitySwitch = millis();
  lastClockTick = millis();
  drawCurrentView();
}

void loop() {
  // --- Touch: tap = next city, swipe = previous city, long-press = pause/resume ---
  // Tracks touch-down position/time separately from the continuously-updated "still
  // held" position, so hold duration and swipe distance can both be measured accurately
  // (the previous version re-stamped its timestamp every tick, which never actually
  // measured how long a touch was held).
  static unsigned long touchStartTime = 0;
  static bool wasTouched = false;
  static bool longPressFired = false;
  static int touchStartX = 0, touchStartY = 0;
  static int lastTouchX = 0, lastTouchY = 0;
  bool touchNow = false;
  int curX = 0, curY = 0;

  if (ts.tirqTouched()) {
    if (ts.touched()) {
      TS_Point p = ts.getPoint();
      // Gate on pressure and a loose raw sanity range, then convert to screen pixels
      // so every gesture threshold below is expressed in the same units as the UI.
      if (p.z > 200 && p.x > 100 && p.y > 100 && p.x < 4000 && p.y < 4000) {
        touchNow = true;
        curX = constrain((int)lroundf(TOUCH_AX * p.x + TOUCH_BX), 0, 319);
        curY = constrain((int)lroundf(TOUCH_AY * p.y + TOUCH_BY), 0, 239);
      }
    }
  }

  if (touchNow && !wasTouched) {
    // Touch-down
    wasTouched = true;
    longPressFired = false;
    touchStartTime = millis();
    touchStartX = curX;
    touchStartY = curY;
    lastTouchX = curX;
    lastTouchY = curY;
  } else if (touchNow && wasTouched) {
    // Still held — fire a long-press once if held steady long enough
    lastTouchX = curX;
    lastTouchY = curY;
    int drift = abs(curX - touchStartX) + abs(curY - touchStartY);
    if (!longPressFired && drift < LONG_PRESS_MAX_DRIFT && millis() - touchStartTime > LONG_PRESS_MS) {
      longPressFired = true;
      if (showingDiagnostics) {
        showingDiagnostics = false;
        lastClockTick = millis();
        drawCurrentView();
        Serial.println("Long-press -> dismissed diagnostics overlay");
      } else {
        autoRotatePaused = !autoRotatePaused;
        lastCitySwitch = millis(); // don't let a stale window instantly resume-then-switch
        // Only the Paused/Auto label changed, and that lives in the weather content of
        // both layouts -- no need to repaint the big clock and make it blink.
        redrawWeatherContent();
        Serial.println(autoRotatePaused ? "Long-press -> auto-rotate paused" : "Long-press -> auto-rotate resumed");
      }
    }
  } else if (!touchNow && wasTouched) {
    // Release
    wasTouched = false;
    static unsigned long lastTapTime = 0;
    unsigned long releaseTime = millis();
    unsigned long heldFor = releaseTime - touchStartTime;
    int dx = lastTouchX - touchStartX;
    int dy = lastTouchY - touchStartY;

    // Touch is the flakiest subsystem on this board, and a gesture that fails to
    // classify is otherwise completely silent. One line per release is cheap and makes
    // "nothing happened" diagnosable: it shows whether the press was seen at all, how
    // long it was held, and how far it travelled in each axis.
    Serial.printf("touch release: held=%lums dx=%d dy=%d (swipe needs |d|>%d and 2x the other axis; tap window %lu-%lums)\n",
                  heldFor, dx, dy, SWIPE_MIN_DELTA, 50UL, LONG_PRESS_MS);

    if (!longPressFired && heldFor > 50 && heldFor < LONG_PRESS_MS) {
      if (showingDiagnostics) {
        // Any tap/swipe dismisses the overlay back to the weather screen.
        showingDiagnostics = false;
        lastClockTick = releaseTime;
        drawCurrentView();
        Serial.println("Dismissed diagnostics overlay");
      } else if (abs(dx) > SWIPE_MIN_DELTA && abs(dx) > abs(dy) * 2) {
        // Horizontal swipe = previous city. Sign follows the same raw-coordinate
        // convention as the existing tap detection; if it goes the wrong way on
        // your physical board, flip the sign here (see CLAUDE.md touch notes).
        int step = (dx > 0) ? -1 : 1;
        currentCityIndex = (currentCityIndex + step + NUM_CITIES) % NUM_CITIES;
        fetchWeather(currentCityIndex);
        lastWeatherUpdate = millis();
        lastCitySwitch = millis();
        lastClockTick = millis();
        redrawWeatherContent();
        Serial.printf("Swipe -> switched to: %s\n", cities[currentCityIndex].name);
      } else if (abs(dy) > SWIPE_MIN_DELTA && abs(dy) > abs(dx) * 2) {
        // Vertical swipe = toggle view. Mutually exclusive with the horizontal test
        // above: each requires its axis to dominate the other by 2x, so a sloppy
        // diagonal simply falls through and is treated as a tap.
        currentView = (currentView == VIEW_WEATHER) ? VIEW_CLOCK : VIEW_WEATHER;
        tft.fillScreen(TFT_BLACK);   // the layouts share no zones; wipe once on switch
        lastClockTick = millis();
        drawCurrentView();
        Serial.printf("Swipe %s -> %s view\n", dy > 0 ? "down" : "up",
                      currentView == VIEW_CLOCK ? "CLOCK" : "WEATHER");
      } else if (releaseTime - lastTapTime < DOUBLE_TAP_WINDOW_MS) {
        // Double-tap = diagnostics overlay (the first tap already advanced
        // the city as a normal single tap; this just replaces the 2nd advance).
        lastTapTime = 0;
        showingDiagnostics = true;
        drawDiagnosticsScreen();
        Serial.println("Double-tap -> diagnostics overlay");
      } else {
        // Plain tap = next city
        lastTapTime = releaseTime;
        currentCityIndex = (currentCityIndex + 1) % NUM_CITIES;
        fetchWeather(currentCityIndex);
        lastWeatherUpdate = millis();
        lastCitySwitch = millis();
        lastClockTick = millis();
        redrawWeatherContent();
        Serial.printf("Tap -> switched to: %s\n", cities[currentCityIndex].name);
      }
    }
  }


  // --- WiFi connectivity watchdog (non-blocking) ---
  if (millis() - lastWifiCheck > WIFI_CHECK_INTERVAL) {
    lastWifiCheck = millis();
    bool nowConnected = (WiFi.status() == WL_CONNECTED);
    if (nowConnected != wifiConnected) {
      wifiConnected = nowConnected;
      if (!showingDiagnostics) redrawWifiIndicator();
      Serial.println(wifiConnected ? "WiFi reconnected" : "WiFi dropped");
    }
    if (!nowConnected) {
      WiFi.reconnect();
    }
  }

  // --- Ambient/night-aware backlight (non-blocking) ---
  if (millis() - lastBrightnessUpdate > BRIGHTNESS_UPDATE_INTERVAL) {
    lastBrightnessUpdate = millis();
    updateBacklight();
  }

  // --- Heap health watchdog (logs + reboots if critically low, see checkHeapHealth()) ---
  if (millis() - lastHeapCheck > HEAP_CHECK_INTERVAL) {
    lastHeapCheck = millis();
    checkHeapHealth();
  }

  // --- Per-second clock tick (small direct redraw, no full-screen sprite) ---
  if (!showingDiagnostics && millis() - lastClockTick > CLOCK_TICK_INTERVAL) {
    lastClockTick = millis();
    if (currentView == VIEW_CLOCK) {
      drawBigClock(tft, true);
    } else {
      drawHomeClock(tft, true);
      drawCityClock(tft, true);
    }
  }

  // --- Auto-switch city every 10 seconds (unless paused or viewing diagnostics) ---
  if (!autoRotatePaused && !showingDiagnostics && millis() - lastCitySwitch > CITY_SWITCH_INTERVAL) {
    currentCityIndex = (currentCityIndex + 1) % NUM_CITIES;
    fetchWeather(currentCityIndex);
    lastWeatherUpdate = millis();
    lastCitySwitch = millis();
    lastClockTick = millis();
    redrawWeatherContent();
    Serial.printf("Auto-switch -> %s\n", cities[currentCityIndex].name);
  }
  
  delay(100);
}
