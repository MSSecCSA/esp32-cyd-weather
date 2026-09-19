// CYD hardware validation rig -- NOT production firmware.
//
// Exercises every board feature the weather station currently leaves UNVERIFIED or
// unused, so the guesses in CLAUDE.md can be replaced with measurements:
//   * RGB LED on GPIO 4/16/17 (claimed active-LOW)
//   * BOOT button on GPIO 0 (claimed the only software-readable button)
//   * LDR polarity on GPIO 34 (LDR_HIGHER_MEANS_BRIGHTER is currently a guess)
//   * I2C bus on SDA=27 / SCL=22 (the CN1 connector claim)
//   * microSD on GPIO 5 + VSPI
//   * speaker/amp on GPIO 26
//   * touch calibration (never performed; production uses raw values)
//
// Serial-command driven at 115200 so each test runs on demand rather than on a timer.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

// The XPT2046 has its OWN SPI pins on this board -- it is NOT on the VSPI defaults
// (18/19/23) that the global SPI object uses. Talking to it there reads a floating
// MISO and returns all-ones (x=8191/y=8191/z=4095).
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_SCK  25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
#define LDR_PIN   34
#define BOOT_BTN   0
#define LED_R      4
#define LED_G     16
#define LED_B     17
#define SPK_PIN   26
#define SD_CS      5
#define I2C_SDA   27
#define I2C_SCL   22

TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

static void banner(const char *t) {
  Serial.printf("\n===== %s =====\n", t);
}

// ---------------------------------------------------------------- RGB LED
// Claimed active-LOW: driving the pin LOW should light that channel.
void testLed() {
  banner("RGB LED  (GPIO 4=R 16=G 17=B, claimed active-LOW)");
  const int pins[3] = {LED_R, LED_G, LED_B};
  const char *names[3] = {"RED", "GREEN", "BLUE"};
  for (int i = 0; i < 3; i++) { pinMode(pins[i], OUTPUT); digitalWrite(pins[i], HIGH); }
  Serial.println("all channels OFF (driven HIGH). Watch the LED now.");
  delay(800);
  for (int i = 0; i < 3; i++) {
    Serial.printf("  -> %s ON  (GPIO %d driven LOW)\n", names[i], pins[i]);
    digitalWrite(pins[i], LOW);
    delay(1400);
    digitalWrite(pins[i], HIGH);
    delay(300);
  }
  Serial.println("  -> WHITE (all three LOW)");
  for (int i = 0; i < 3; i++) digitalWrite(pins[i], LOW);
  delay(1600);
  for (int i = 0; i < 3; i++) digitalWrite(pins[i], HIGH);
  Serial.println("all OFF. Report which colours you actually saw, and in what order.");
}

// ---------------------------------------------------------------- BOOT button
void testButton() {
  banner("BOOT BUTTON  (GPIO 0, internal pull-up)");
  pinMode(BOOT_BTN, INPUT_PULLUP);
  Serial.println("idle level should read 1; pressing should read 0.");
  Serial.printf("idle reading now: %d\n", digitalRead(BOOT_BTN));
  Serial.println("PRESS THE BOOT BUTTON A FEW TIMES over the next 15 seconds...");
  unsigned long t0 = millis();
  int last = digitalRead(BOOT_BTN), presses = 0;
  while (millis() - t0 < 15000) {
    int now = digitalRead(BOOT_BTN);
    if (now != last) {
      if (now == LOW) { presses++; Serial.printf("  [%5lums] PRESS  #%d\n", millis()-t0, presses); }
      else            { Serial.printf("  [%5lums] release\n", millis()-t0); }
      last = now;
      delay(30); // debounce
    }
    delay(5);
  }
  Serial.printf("RESULT: %d press(es) detected. %s\n", presses,
                presses ? "GPIO 0 is usable as a runtime input."
                        : "No presses seen -- button not readable, or not pressed.");
}

// ---------------------------------------------------------------- LDR
// R21 (GT36516) is wired GPIO34 -> GND with a pull-up divider to 3V3 (R15/R19).
// Two confounders ruined the first attempt at this test: the backlight was ON at duty
// 220, and spill from the panel edge lands directly on R21; and analogRead() floors to 0
// below the ESP32 ADC's bottom dead zone. So: backlight OFF, and report millivolts
// alongside raw, since the calibrated mV path still showed signal (142 mV) when raw read 0.
void ldrTrace(int backlightDuty, const char *what);

void testLdr()   { ldrTrace(0,   "BACKLIGHT OFF"); }
void testLdrBl() { ldrTrace(220, "BACKLIGHT ON (duty 220) -- the real operating condition"); }

void ldrTrace(int backlightDuty, const char *what) {
  char hdr[96]; snprintf(hdr, sizeof(hdr), "LDR / R21 -- %s", what);
  banner(hdr);
  Serial.printf("backlight duty set to %d\n", backlightDuty);
  Serial.println("COVER R21 firmly (seconds 5-12), UNCOVER (12-18), COVER again (18-24).");
  Serial.println("R21 is the small clear/amber component near the display edge.");
  Serial.println("  t(ms)   raw    mV");

  ledcWrite(0, backlightDuty);
  analogSetPinAttenuation(LDR_PIN, ADC_11db);
  delay(300);

  int rlo = 4095, rhi = 0, mlo = 99999, mhi = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 24000) {
    int raw = analogRead(LDR_PIN);
    int mv  = analogReadMilliVolts(LDR_PIN);
    if (raw < rlo) rlo = raw;
    if (raw > rhi) rhi = raw;
    if (mv  < mlo) mlo = mv;
    if (mv  > mhi) mhi = mv;
    Serial.printf("  %5lu  %4d  %4d\n", millis() - t0, raw, mv);
    delay(400);
  }

  Serial.printf("RESULT raw: min=%d max=%d span=%d\n", rlo, rhi, rhi - rlo);
  Serial.printf("RESULT  mV: min=%d max=%d span=%d\n", mlo, mhi, mhi - mlo);
  if (rhi > 40 || (mhi - mlo) > 60)
    Serial.println("VERDICT: R21 RESPONDS to light. The sensor works; the divider just\n"
                   "         compresses it. A resistor mod would open the range up.");
  else
    Serial.println("VERDICT: no usable response even with the backlight off -- the divider\n"
                   "         pins it below the ADC floor in all conditions.");

  // attenuation sweep while (hopefully) still covered
  Serial.println("\nKEEP IT COVERED for 6 more seconds -- attenuation sweep:");
  const adc_attenuation_t atts[4] = {ADC_0db, ADC_2_5db, ADC_6db, ADC_11db};
  const char *an[4] = {"0dB", "2.5dB", "6dB", "11dB"};
  for (int i = 0; i < 4; i++) {
    analogSetPinAttenuation(LDR_PIN, atts[i]);
    delay(400);
    long acc = 0; int lo = 4095, hi = 0;
    for (int k = 0; k < 15; k++) { int r = analogRead(LDR_PIN); acc += r; if(r<lo)lo=r; if(r>hi)hi=r; delay(60); }
    Serial.printf("  %-6s covered: avg=%4ld min=%4d max=%4d  mV=%d\n",
                  an[i], acc/15, lo, hi, analogReadMilliVolts(LDR_PIN));
  }

  ledcWrite(0, 220);                    // backlight back on
  Serial.println("backlight restored.");
}

// ---------------------------------------------------------------- I2C
void testI2c() {
  banner("I2C SCAN  (SDA=27, SCL=22 -- the CN1 connector claim)");
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device found at 0x%02X\n", addr);
      found++;
    }
  }
  Serial.printf("RESULT: %d device(s).\n", found);
  if (!found)
    Serial.println("Expected with nothing plugged in. The scan completing without hanging\n"
                   "still proves the pins are free and not held by other hardware.");
}

// ---------------------------------------------------------------- microSD
void testSd() {
  banner("microSD  (CS=GPIO 5, shares VSPI with touch)");
  Serial.println("NOTE: this re-inits the shared SPI bus; re-run touch tests after.");
  if (!SD.begin(SD_CS)) {
    Serial.println("RESULT: SD.begin() failed -- no card inserted, or slot not on GPIO 5.");
    return;
  }
  uint8_t type = SD.cardType();
  const char *tn = type == CARD_MMC ? "MMC" : type == CARD_SD ? "SDSC"
                 : type == CARD_SDHC ? "SDHC" : "UNKNOWN";
  Serial.printf("RESULT: card mounted. type=%s size=%llu MB\n", tn, SD.cardSize() / (1024ULL*1024ULL));
  File root = SD.open("/");
  int n = 0;
  for (File f = root.openNextFile(); f && n < 10; f = root.openNextFile(), n++)
    Serial.printf("   %s  %u bytes\n", f.name(), (unsigned)f.size());
  if (!n) Serial.println("   (card is empty)");
  SD.end();
}

// ---------------------------------------------------------------- speaker
void testSpeaker() {
  banner("SPEAKER  (GPIO 26 -> on-board class-D amp -> P4)");
  Serial.println("Playing a short rising tone sequence. LISTEN for any sound.");
  Serial.println("(with no speaker attached to P4 you may hear nothing, or a faint tick)");
  const int ch = 4;
  ledcSetup(ch, 1000, 10);
  ledcAttachPin(SPK_PIN, ch);
  const int notes[5] = {440, 554, 659, 880, 1109};
  for (int i = 0; i < 5; i++) {
    ledcWriteTone(ch, notes[i]);
    ledcWrite(ch, 300);              // modest duty: no current limiting on this output
    Serial.printf("  %d Hz\n", notes[i]);
    delay(320);
  }
  ledcWrite(ch, 0);
  ledcDetachPin(SPK_PIN);
  pinMode(SPK_PIN, INPUT);
  Serial.println("done. Report whether you heard anything.");
}

// ---------------------------------------------------------------- raw touch
void testTouchRaw() {
  banner("RAW TOUCH STREAM (10s)");
  Serial.println("Drag your finger around the screen. Raw x/y/z as production sees them.");
  unsigned long t0 = millis();
  while (millis() - t0 < 10000) {
    if (ts.tirqTouched() && ts.touched()) {
      TS_Point p = ts.getPoint();
      Serial.printf("  x=%4d y=%4d z=%4d\n", p.x, p.y, p.z);
      delay(80);
    }
    delay(10);
  }
  Serial.println("stream ended.");
}

// ---------------------------------------------------------------- calibration
struct CalPoint { int sx, sy; long rx, ry; };

static bool captureTap(int sx, int sy, long &rx, long &ry) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Tap the crosshair", 160, 120, 2);
  tft.drawLine(sx - 12, sy, sx + 12, sy, TFT_YELLOW);
  tft.drawLine(sx, sy - 12, sx, sy + 12, TFT_YELLOW);
  tft.drawCircle(sx, sy, 7, TFT_YELLOW);

  // wait for a press (30s timeout)
  unsigned long t0 = millis();
  while (!(ts.tirqTouched() && ts.touched())) {
    if (millis() - t0 > 30000) return false;
    delay(10);
  }
  delay(60); // let the press settle
  long sx_acc = 0, sy_acc = 0; int n = 0;
  while (ts.touched() && n < 40) {
    TS_Point p = ts.getPoint();
    if (p.z > 200) { sx_acc += p.x; sy_acc += p.y; n++; }
    delay(8);
  }
  if (n < 5) return false;
  rx = sx_acc / n; ry = sy_acc / n;
  tft.fillCircle(sx, sy, 7, TFT_GREEN);
  while (ts.touched()) delay(10);   // wait for release
  delay(250);
  return true;
}

// least-squares fit: screen = a*raw + b
static void fit(const long *raw, const int *scr, int n, float &a, float &b) {
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (int i = 0; i < n; i++) { sx += raw[i]; sy += scr[i]; sxx += (double)raw[i]*raw[i]; sxy += (double)raw[i]*scr[i]; }
  double d = n * sxx - sx * sx;
  if (fabs(d) < 1e-9) { a = 0; b = 0; return; }
  a = (float)((n * sxy - sx * sy) / d);
  b = (float)((sy - a * sx) / n);
}

static double corr(const long *x, const int *y, int n) {
  double mx = 0, my = 0;
  for (int i = 0; i < n; i++) { mx += x[i]; my += y[i]; }
  mx /= n; my /= n;
  double num = 0, dx = 0, dy = 0;
  for (int i = 0; i < n; i++) {
    double a = x[i] - mx, b = y[i] - my;
    num += a * b; dx += a * a; dy += b * b;
  }
  if (dx <= 0 || dy <= 0) return 0;
  return num / sqrt(dx * dy);
}

void testCalibrate() {
  banner("TOUCH CALIBRATION  (5 points, rotation 1, 320x240)");
  Serial.println("Tap each crosshair as accurately as you can. Use a fingernail or stylus --");
  Serial.println("this is a RESISTIVE panel, so it wants a firm small press, not a fingertip pad.");

  CalPoint pts[5] = {
    { 30,  30, 0, 0}, {290,  30, 0, 0},
    { 30, 210, 0, 0}, {290, 210, 0, 0},
    {160, 120, 0, 0},
  };
  const char *label[5] = {"top-left","top-right","bottom-left","bottom-right","centre"};

  for (int i = 0; i < 5; i++) {
    Serial.printf("  point %d/5: %s (%d,%d) -- tap it\n", i+1, label[i], pts[i].sx, pts[i].sy);
    if (!captureTap(pts[i].sx, pts[i].sy, pts[i].rx, pts[i].ry)) {
      Serial.println("  TIMED OUT or press too light. Aborting calibration.");
      tft.fillScreen(TFT_BLACK);
      tft.drawString("calibration aborted", 160, 120, 2);
      return;
    }
    Serial.printf("     raw x=%ld y=%ld\n", pts[i].rx, pts[i].ry);
  }

  long rx[5], ry[5]; int sx[5], sy[5];
  for (int i = 0; i < 5; i++) { rx[i]=pts[i].rx; ry[i]=pts[i].ry; sx[i]=pts[i].sx; sy[i]=pts[i].sy; }

  double cxx = fabs(corr(rx, sx, 5)), cxy = fabs(corr(rx, sy, 5));
  bool swap = cxy > cxx;
  Serial.printf("\n  |corr(rawX,screenX)|=%.3f  |corr(rawX,screenY)|=%.3f  -> axes %s\n",
                cxx, cxy, swap ? "SWAPPED" : "aligned");

  float ax, bx, ay, by;
  if (!swap) { fit(rx, sx, 5, ax, bx); fit(ry, sy, 5, ay, by); }
  else       { fit(ry, sx, 5, ax, bx); fit(rx, sy, 5, ay, by); }

  Serial.println("\n----- CALIBRATION RESULT (paste-ready) -----");
  Serial.printf("const bool  TOUCH_SWAP_XY = %s;\n", swap ? "true" : "false");
  Serial.printf("const float TOUCH_AX = %.6f, TOUCH_BX = %.3f;  // screenX = AX*raw + BX\n", ax, bx);
  Serial.printf("const float TOUCH_AY = %.6f, TOUCH_BY = %.3f;  // screenY = AY*raw + BY\n", ay, by);
  Serial.printf("// X axis %s, Y axis %s\n", ax < 0 ? "INVERTED" : "normal", ay < 0 ? "INVERTED" : "normal");

  Serial.println("\n----- residuals (how good the fit is) -----");
  float worst = 0;
  for (int i = 0; i < 5; i++) {
    float px = ax * (swap ? ry[i] : rx[i]) + bx;
    float py = ay * (swap ? rx[i] : ry[i]) + by;
    float ex = px - sx[i], ey = py - sy[i];
    float e = sqrt(ex*ex + ey*ey);
    if (e > worst) worst = e;
    Serial.printf("  %-13s target(%3d,%3d) predicted(%6.1f,%6.1f) error %.1f px\n",
                  label[i], sx[i], sy[i], px, py, e);
  }
  Serial.printf("worst error: %.1f px  -- %s\n", worst,
                worst < 12 ? "GOOD, well within a fingertip" :
                worst < 25 ? "USABLE, but taps near edges may drift" :
                             "POOR -- re-run and tap more precisely");

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("calibration done", 160, 108, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  char buf[40];
  snprintf(buf, sizeof(buf), "worst error %.1f px", worst);
  tft.drawString(buf, 160, 132, 2);
}

// ---------------------------------------------------------------- verify
void testVerify() {
  banner("VERIFY CALIBRATION -- draw test (20s)");
  Serial.println("Touch the screen; a dot should appear UNDER YOUR FINGER.");
  Serial.println("Run this only after 't'. Values are the freshly fitted ones.");
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("draw here - dot should track your finger", 160, 4, 1);
  Serial.println("(no persisted constants in this build -- see the paste-ready block above)");
  unsigned long t0 = millis();
  while (millis() - t0 < 20000) {
    if (ts.tirqTouched() && ts.touched()) {
      TS_Point p = ts.getPoint();
      if (p.z > 200) tft.fillCircle(map(p.x, 200, 3900, 0, 320), map(p.y, 200, 3900, 0, 240), 3, TFT_CYAN);
    }
    delay(12);
  }
  Serial.println("draw test ended (this used a crude default map, not your fit).");
}

// ---------------------------------------------------------------- ADC probe
void testAdc() {
  banner("ADC PROBE  (why does GPIO 34 read a flat 0?)");
  const int pins[2] = {34, 35};
  const adc_attenuation_t atts[4] = {ADC_0db, ADC_2_5db, ADC_6db, ADC_11db};
  const char *an[4] = {"0dB", "2.5dB", "6dB", "11dB"};
  for (int p = 0; p < 2; p++) {
    Serial.printf("  --- GPIO %d ---\n", pins[p]);
    for (int a = 0; a < 4; a++) {
      analogSetPinAttenuation(pins[p], atts[a]);
      delay(20);
      long acc = 0; int lo = 4095, hi = 0;
      for (int i = 0; i < 20; i++) { int r = analogRead(pins[p]); acc += r; if(r<lo)lo=r; if(r>hi)hi=r; delay(8); }
      Serial.printf("    %-6s avg=%4ld min=%4d max=%4d  mV=%d\n",
                    an[a], acc/20, lo, hi, analogReadMilliVolts(pins[p]));
    }
  }
  Serial.println("  GPIO 35 is a known-floating control: it should show NOISE.");
  Serial.println("  If 35 is noisy but 34 is pinned at 0, GPIO 34 is held low -> no LDR there.");
  Serial.println("  If BOTH are 0, the ADC itself is misconfigured.");
}

void help() {
  Serial.println("\n--------- CYD hardware test rig ---------");
  Serial.println("  l  RGB LED cycle            b  BOOT button (15s)");
  Serial.println("  d  LDR polarity (20s)       i  I2C scan on 27/22");
  Serial.println("  s  microSD mount            p  speaker tone");
  Serial.println("  r  raw touch stream (10s)   t  TOUCH CALIBRATION (5 taps)");
  Serial.println("  v  draw test (20s)          a  ADC probe (GPIO 34/35)");
  Serial.println("  L  LDR with backlight ON     ?  this help");
  Serial.println("----------------------------------------");
}

void setup() {
  Serial.begin(115200);
  delay(400);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(21, 0);
  ledcWrite(0, 220);                 // backlight on, matching production's PWM approach
  // The pinned XPT2046_Touchscreen calls a bare SPI.begin(), which would claim VSPI's
  // DEFAULT pins (18/19/23) -- where the touch chip is not. ESP32's SPIClass::begin()
  // returns early if the bus is already initialised, so re-pinning the global SPI to
  // the touch controller's actual pins FIRST makes the library adopt them.
  SPI.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin();
  ts.setRotation(1);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("HW TEST RIG", 160, 110, 4);
  tft.drawString("drive it from serial", 160, 140, 2);

  Serial.println("\n\n########  CYD HARDWARE TEST RIG  ########");
  Serial.println("This is NOT the weather firmware. Production will be restored after.");
  help();
  Serial.println("READY");
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  char c = Serial.read();
  switch (c) {
    case 'l': testLed();       break;
    case 'b': testButton();    break;
    case 'd': testLdr();       break;
    case 'i': testI2c();       break;
    case 's': testSd();        break;
    case 'p': testSpeaker();   break;
    case 'r': testTouchRaw();  break;
    case 't': testCalibrate(); break;
    case 'v': testVerify();    break;
    case 'a': testAdc();       break;
    case 'L': testLdrBl();     break;
    case '?': help();          break;
    default: return;
  }
  Serial.println("DONE");
}
