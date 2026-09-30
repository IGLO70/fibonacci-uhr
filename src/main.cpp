/*
  Fibonacci-Spiralen-Uhr fuer WT32-SC01 (ESP32-WROVER-B, 3.5" 480x320, ST7796 ueber SPI)

  Ablesen der Uhr:
    - Zwei Goldene Spiralen (r = a * e^(b*theta)) drehen sich um die Bildschirmmitte.
    - STUNDEN (Gold):  senkrechte Achse. Obere Haelfte 12-6 Uhr, untere Haelfte 6-12 Uhr.
    - MINUTEN (Blau):  waagrechte Achse. Rechte Haelfte 0-30 min, linke Haelfte 30-60 min.
    - Die Skalen stehen fest. Der Schnittpunkt der Spirale mit der Achse (Markierungspunkt)
      wandert beim Drehen der Spirale von innen nach aussen und zeigt die Zeit.

  Hardware:
    - Uhrzeit: DS3231-RTC am I2C-Bus (SDA = IO18, SCL = IO19)
    - Einstellungen: WLAN "Fibonacci-Clock-Setup" -> Browser -> http://192.168.4.1
    - Arduino IDE: diese Datei als FibonacciUhr.ino speichern (Inhalt unveraendert)
*/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>       // Explizit fuer TFT_eSPI / Adafruit BusIO
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <RTClib.h>
#include <time.h>      // ESP32-Systemzeit / NTP

// ============================================================
//  Konfiguration
// ============================================================
static const int PIN_SDA = 18;   // I2C des WT32-SC01 (teilt sich den Bus mit dem Touch-Controller)
static const int PIN_SCL = 19;

static const char* AP_SSID = "Fibonacci-Clock-Setup";
static const char* AP_PASS = "retro-fibonacci";   // mind. 8 Zeichen

static const int screenWidth  = 480;   // nach setRotation(1) = Querformat
static const int screenHeight = 320;
static const int centerX = screenWidth / 2;
static const int centerY = screenHeight / 2;

// Goldener Schnitt: b = ln(phi) / (PI/2)  -> Radius waechst pro Vierteldrehung um Faktor 1,618
static const float GB = 0.3063487f;

// Radius auf der Achse bei "Skalenanfang" (12:00 oben / 6:00 unten bzw. 0 min rechts / 30 min links)
// Ueber eine halbe Drehung waechst der Radius um phi^2 = 2,618
static const float HOUR_R0 = 56.0f;    // -> Skala 56 ... 146 px (senkrecht)
static const float MIN_R0  = 86.0f;    // -> Skala 86 ... 225 px (waagrecht)

float aHour, aMin;   // Startradien der beiden Spiralen (werden in setup() berechnet)

// ============================================================
//  Objekte & Einstellungen
// ============================================================
TFT_eSPI    tft      = TFT_eSPI();
TFT_eSprite img      = TFT_eSprite(&tft);   // Zeichen-Buffer (flimmerfrei)
TFT_eSprite bgSprite = TFT_eSprite(&tft);   // Hintergrund-Cache (Metall-Textur)
RTC_DS3231  rtc;
WebServer   server(80);
Preferences prefs;

bool     rtcOk = false;
DateTime softBase;              // Ersatzzeit, falls keine RTC gefunden wird
uint32_t softBaseMs = 0;

// NTP-Status
bool ntpTimeValid = false;
bool ntpSyncRequested = false;
uint32_t ntpRequestStartedMs = 0;
uint32_t lastNtpAttemptMs = 0;
time_t lastNtpSyncEpoch = 0;

// Auswahl wichtiger Zeitzonen.
// Die POSIX-Regeln enthalten, wo erforderlich, automatische Sommer-/Winterzeit.
struct TimezoneEntry {
  const char* name;
  const char* posix;
};

const TimezoneEntry TIMEZONES[] = {
  { "Mitteleuropa - Wien, Berlin, Zuerich, Paris", "CET-1CEST,M3.5.0/2,M10.5.0/3" },
  { "Grossbritannien / Irland - London, Dublin",   "GMT0BST,M3.5.0/1,M10.5.0" },
  { "Osteuropa - Helsinki, Bukarest",              "EET-2EEST,M3.5.0/3,M10.5.0/4" },
  { "UTC - Weltzeit",                              "UTC0" },
  { "USA Eastern - New York",                      "EST5EDT,M3.2.0/2,M11.1.0/2" },
  { "USA Central - Chicago",                       "CST6CDT,M3.2.0/2,M11.1.0/2" },
  { "USA Mountain - Denver",                       "MST7MDT,M3.2.0/2,M11.1.0/2" },
  { "USA Pacific - Los Angeles",                   "PST8PDT,M3.2.0/2,M11.1.0/2" },
  { "Japan - Tokio",                               "JST-9" },
  { "Australien Eastern - Sydney",                 "AEST-10AEDT,M10.1.0/2,M4.1.0/3" }
};

static const int TIMEZONE_COUNT = sizeof(TIMEZONES) / sizeof(TIMEZONES[0]);

struct ClockSettings {
  int      backgroundType = 1;          // 0 = Schwarz, 1 = Gebuerstetes Metall
  int      effectType     = 1;          // 0 = Klassisch, 1 = Neon-Glow, 2 = Farbverlauf
  uint32_t hourRGB        = 0xFFD700;   // Gold
  uint32_t minRGB         = 0x00BFFF;   // Himmelblau
  uint32_t secondRGB      = 0xFFDC46;   // Sonnenorange
  int      brightness     = 100;        // 10..100 %
  int      thickness      = 2;          // 1..5 px
  bool     circadianMode  = false;      // Feature 1: Tageszeit-Farben
  bool     showDate       = false;      // Feature 2: Datum
  bool     particleEffect = false;      // Feature 3: Sternenstaub

  // Optionales Haus-WLAN; der Setup-Access-Point bleibt immer aktiv.
  bool     wifiEnabled    = false;
  String   wifiSSID       = "";
  String   wifiPassword   = "";

  // Index in TIMEZONES; Standard = Mitteleuropa.
  int      timezoneIndex  = 0;
} settings;

// Laufzeitstatus der optionalen Haus-WLAN-Verbindung.
bool wifiConnectPending = false;
uint32_t wifiConnectStartedMs = 0;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// Echte WLAN-Aenderungen werden erst nach der HTTP-Antwort angewendet.
bool wifiRestartPending = false;
uint32_t wifiRestartAtMs = 0;

// Touchcontroller des WT32-SC01 (FT6336U/FT6x36-kompatibel).
static const uint8_t TOUCH_I2C_ADDR = 0x38;
static const uint8_t TOUCH_REG_TD_STATUS = 0x02;

// Infoseite: beim Start und nach Antippen 10 Sekunden sichtbar.
bool infoScreenActive = true;
uint32_t infoScreenUntilMs = 0;
uint32_t lastInfoRedrawMs = 0;
bool touchWasDown = false;
static const uint32_t INFO_SCREEN_DURATION_MS = 10000;

struct UiColors { uint16_t axis, tick, text; } ui;

// Partikel (Feature 3)
const int MAX_PARTICLES = 45;
struct Particle { float angle; float radius; float speed; uint8_t brightness; } particles[MAX_PARTICLES];

// ============================================================
//  Hilfsfunktionen: Farben
// ============================================================
uint16_t rgb888to565(uint32_t c) {
  return tft.color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// Helligkeit einer RGB565-Farbe in Prozent anpassen
uint16_t applyBrightness(uint16_t color, int percent) {
  if (percent >= 100) return color;
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;
  r = (r * percent) / 100;
  g = (g * percent) / 100;
  b = (b * percent) / 100;
  return (r << 11) | (g << 5) | b;
}

inline uint16_t dim(uint16_t c) { return applyBrightness(c, settings.brightness); }

// Feature 1: Farben abhaengig von der Tageszeit
uint16_t getCircadianColor(int hour, bool isHourSpiral) {
  if (hour >= 6 && hour < 18) {   // Tag: kuehle Farben
    return isHourSpiral ? tft.color565(230, 240, 255) : tft.color565(0, 191, 255);
  }
  // Abend/Nacht: warme, augenschonende Toene
  return isHourSpiral ? tft.color565(255, 140, 0) : tft.color565(180, 0, 0);
}

// ------------------------------------------------------------
// Farben fuer Achsen, Skalen und Beschriftung.
//
// WICHTIG:
// Die Skalenfarben werden NICHT mit der globalen Helligkeit
// gedimmt. Dadurch bleiben Uhrzeit-Skalen und Zahlen auch bei
// reduzierter Displayhelligkeit deutlich lesbar.
// ------------------------------------------------------------
void updateUiColors()
{
    if (settings.backgroundType == 1)
    {
        // Gebuerstetes Metall:
        // dunkle, aber deutlich sichtbare Beschriftung
        ui.axis = tft.color565(35, 35, 40);
        ui.tick = tft.color565(20, 20, 24);
        ui.text = tft.color565(5, 5, 8);
    }
    else
    {
        // Schwarzer Hintergrund:
        // helle Skalen und Beschriftungen
        ui.axis = tft.color565(100, 100, 105);
        ui.tick = tft.color565(190, 190, 195);
        ui.text = tft.color565(235, 235, 240);
    }
}

// ============================================================
//  Diagnose: Speicherstatus
// ============================================================
void printMemory(const char* title) {
  Serial.println();
  Serial.println("----------------------------------------");
  Serial.println(title);
  Serial.printf("Heap gesamt : %u Bytes\n", ESP.getHeapSize());
  Serial.printf("Heap frei   : %u Bytes\n", ESP.getFreeHeap());
  Serial.printf("PSRAM gesamt: %u Bytes\n", ESP.getPsramSize());
  Serial.printf("PSRAM frei  : %u Bytes\n", ESP.getFreePsram());
  Serial.println("----------------------------------------");
}

// ============================================================
//  Zeit
// ============================================================
DateTime getNow() {
  if (rtcOk) return rtc.now();

  // Nach erfolgreichem NTP-Abgleich die lokale ESP32-Systemzeit verwenden.
  if (ntpTimeValid) {
    time_t nowEpoch = time(nullptr);
    struct tm localTm;
    if (localtime_r(&nowEpoch, &localTm)) {
      return DateTime(localTm.tm_year + 1900, localTm.tm_mon + 1,
                      localTm.tm_mday, localTm.tm_hour,
                      localTm.tm_min, localTm.tm_sec);
    }
  }

  // Rueckfall: bisherige Software-Uhr.
  return softBase + TimeSpan((millis() - softBaseMs) / 1000);
}

// ============================================================
//  Einstellungen speichern / laden (bleiben nach Neustart erhalten)
// ============================================================
void loadSettings() {
  prefs.begin("fibclock", false);
  settings.backgroundType = prefs.getInt("bg", settings.backgroundType);
  settings.effectType     = prefs.getInt("fx", settings.effectType);
  settings.hourRGB        = prefs.getUInt("hcol", settings.hourRGB);
  settings.minRGB         = prefs.getUInt("mcol", settings.minRGB);
  settings.secondRGB      = prefs.getUInt("scol", settings.secondRGB);
  settings.brightness     = prefs.getInt("bright", settings.brightness);
  settings.thickness      = prefs.getInt("thick", settings.thickness);
  settings.circadianMode  = prefs.getBool("circ", settings.circadianMode);
  settings.showDate       = prefs.getBool("date", settings.showDate);
  settings.particleEffect = prefs.getBool("part", settings.particleEffect);
  settings.wifiEnabled    = prefs.getBool("wifi_on", settings.wifiEnabled);
  settings.wifiSSID       = prefs.getString("wifi_ssid", settings.wifiSSID);
  settings.wifiPassword   = prefs.getString("wifi_pass", settings.wifiPassword);
  settings.timezoneIndex  = prefs.getInt("timezone", settings.timezoneIndex);
  if (settings.timezoneIndex < 0 || settings.timezoneIndex >= TIMEZONE_COUNT) {
    settings.timezoneIndex = 0;
  }
  prefs.end();
}

void saveSettings() {
  prefs.begin("fibclock", false);
  prefs.putInt("bg", settings.backgroundType);
  prefs.putInt("fx", settings.effectType);
  prefs.putUInt("hcol", settings.hourRGB);
  prefs.putUInt("mcol", settings.minRGB);
  prefs.putUInt("scol", settings.secondRGB);
  prefs.putInt("bright", settings.brightness);
  prefs.putInt("thick", settings.thickness);
  prefs.putBool("circ", settings.circadianMode);
  prefs.putBool("date", settings.showDate);
  prefs.putBool("part", settings.particleEffect);
  prefs.end();
}

// ============================================================
//  Hintergrund: gebuerstetes Metall (einmalig in Cache-Sprite gerendert)
// ============================================================
void initBackground() {
  bgSprite.fillSprite(TFT_BLACK);
  if (settings.backgroundType != 1) return;

  randomSeed(1234);   // feste Textur, damit sie bei jedem Neuaufbau gleich aussieht
  const float maxD = sqrtf((float)centerX * centerX + (float)centerY * centerY);

  for (int y = 0; y < screenHeight; y++) {
    int base = 104 + random(-16, 17);            // Helligkeit dieser "Buerstenriefe"
    for (int x = 0; x < screenWidth;) {
      int len = random(10, 70);                  // kurze Segmente -> feines Korn
      int v = base + random(-9, 10);
      float dx = x + len / 2.0f - centerX;
      float dy = y - centerY;
      float d = sqrtf(dx * dx + dy * dy) / maxD;
      v = (int)(v * (1.0f - 0.45f * d * d));     // Vignette: Ecken dunkler
      v = constrain(v * settings.brightness / 100, 0, 255);
      int vb = constrain(v + 4, 0, 255);         // leichter Blaustich wie Stahl
      bgSprite.drawFastHLine(x, y, len, tft.color565(v, v, vb));
      x += len;
    }
  }
  randomSeed(micros());
}

// ============================================================
//  Partikel (Feature 3)
// ============================================================
void initParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    particles[i].angle      = random(0, 360) * (PI / 180.0f);
    particles[i].radius     = random(20, 230);
    particles[i].speed      = random(5, 20) / 1000.0f;
    particles[i].brightness = random(80, 230);
  }
}

void updateAndDrawParticles() {
  for (int i = 0; i < MAX_PARTICLES; i++) {
    particles[i].angle += particles[i].speed;
    if (particles[i].angle >= 2.0f * PI) particles[i].angle -= 2.0f * PI;

    int x = centerX + (int)(particles[i].radius * cosf(particles[i].angle));
    int y = centerY + (int)(particles[i].radius * sinf(particles[i].angle));

    if (x > 0 && x < screenWidth - 1 && y > 0 && y < screenHeight - 1) {
      uint8_t b = particles[i].brightness;
      uint16_t c = dim(tft.color565(b, b, b));
      if (b > 170) img.fillRect(x, y, 2, 2, c);
      else         img.drawPixel(x, y, c);
    }
  }
}

// ============================================================
//  Zifferblatt: feste Achsen und Skalen
//  Skalenposition = Radius, an dem die Spirale die Achse kreuzt, wenn die Uhr genau diese Zeit zeigt.
// ============================================================
void drawClockFace() {
  // Achsen
  img.drawFastVLine(centerX, centerY - 152, 305, ui.axis);
  img.drawFastHLine(4, centerY, screenWidth - 8, ui.axis);

  img.setTextDatum(MC_DATUM);
  img.setTextColor(ui.text);

  // --- Stundenskala (senkrecht): 36 Teilstriche pro Haelfte = alle 10 min, Hauptstrich jede Stunde ---
  for (int i = 0; i <= 36; i++) {
    float r = HOUR_R0 * expf(GB * i * (PI / 36.0f));
    int yUp = centerY - (int)(r + 0.5f);
    int yDn = centerY + (int)(r + 0.5f);
    bool major = (i % 6 == 0);
    int len = major ? 6 : 3;
    // Hauptstriche jeder vollen Stunde deutlich kraeftiger zeichnen
        if (major) {
            for (int dy = -1; dy <= 1; dy++) {
                img.drawFastHLine(centerX - len, yUp + dy, 2 * len + 1, ui.tick);
                img.drawFastHLine(centerX - len, yDn + dy, 2 * len + 1, ui.tick);
            }
        } else {
            img.drawFastHLine(centerX - len, yUp, 2 * len + 1, ui.tick);
            img.drawFastHLine(centerX - len, yDn, 2 * len + 1, ui.tick);
        }
    if (major) {
      int n = i / 6;                          // 0..6
      // Stundenbeschriftung leicht "fett" zeichnen.
        // Zweiter Aufruf um 1 Pixel versetzt.
        int hourUp = (n == 0 ? 12 : n);
        int hourDn = n + 6;

        img.drawNumber(hourUp, centerX - 20,     yUp, 2);
        img.drawNumber(hourUp, centerX - 20 + 1, yUp, 2);

        img.drawNumber(hourDn, centerX - 20,     yDn, 2);
        img.drawNumber(hourDn, centerX - 20 + 1, yDn, 2);
            }
        }

  // --- Minutenskala (waagrecht): jede Minute ein Strich, alle 5 min Zahl ---
  for (int i = 0; i <= 30; i++) {
    float r = MIN_R0 * expf(GB * i * (2.0f * PI / 60.0f));
    int xR = centerX + (int)(r + 0.5f);
    int xL = centerX - (int)(r + 0.5f);
    bool major = (i % 5 == 0);
    int len = major ? 6 : 3;
    // Alle 5 Minuten wird der Skalenstrich 3 Pixel breit.
        // Die einzelnen Minuten bleiben 1 Pixel breit.
        if (major) {
            for (int dx = -1; dx <= 1; dx++) {
                img.drawFastVLine(xR + dx, centerY - len, 2 * len + 1, ui.tick);
                img.drawFastVLine(xL + dx, centerY - len, 2 * len + 1, ui.tick);
            }
        } else {
            img.drawFastVLine(xR, centerY - len, 2 * len + 1, ui.tick);
            img.drawFastVLine(xL, centerY - len, 2 * len + 1, ui.tick);
        }
    if (major) {
      // Minutenbeschriftung leicht kraeftiger darstellen.
        img.drawNumber(i,      xR,     centerY + 17, 2);
        img.drawNumber(i,      xR + 1, centerY + 17, 2);

        img.drawNumber(30 + i, xL,     centerY + 17, 2);
        img.drawNumber(30 + i, xL + 1, centerY + 17, 2);
            }
  }
}

// ============================================================
//  Spiralen zeichnen
//  Punkt der Spirale:  Radius r = a * e^(b*theta),  Bildschirmwinkel phi = rot - theta
//  -> dreht sich mit "rot" im Uhrzeigersinn, windet sich nach aussen gegen den Uhrzeigersinn.
// ============================================================
static void strokeSpiral(float rot, float a, float maxR, float width,
                         uint16_t color, bool gradient, uint16_t color2) {
  float theta = 0.0f;
  float x0 = centerX + a * cosf(rot);
  float y0 = centerY + a * sinf(rot);

  while (true) {
    float r = a * expf(GB * theta);
    theta += constrain(6.0f / r, 0.03f, 0.35f);      // ca. 6 px Segmentlaenge
    r = a * expf(GB * theta);
    if (r > maxR) break;

    float phi = rot - theta;
    float x1 = centerX + r * cosf(phi);
    float y1 = centerY + r * sinf(phi);

    uint16_t c = color;
    if (gradient) {   // Farbverlauf entlang der Spirale
      uint8_t alpha = (uint8_t)constrain(255 - (int)(theta * 14.0f), 40, 255);
      c = tft.alphaBlend(alpha, color, color2);
    }
    img.drawWideLine(x0, y0, x1, y1, width, c);
    x0 = x1; y0 = y1;
  }
}

void drawSpiral(float rot, float a, uint16_t rawColor, float maxR) {
  const uint16_t base = dim(rawColor);
  const float t = (float)settings.thickness;

  if (settings.effectType == 1) {                    // Neon-Glow: mehrere Schichten
    strokeSpiral(rot, a, maxR, t + 9.0f, tft.alphaBlend(35,  base, TFT_BLACK), false, 0);
    strokeSpiral(rot, a, maxR, t + 5.0f, tft.alphaBlend(85,  base, TFT_BLACK), false, 0);
    strokeSpiral(rot, a, maxR, t + 2.0f, tft.alphaBlend(160, base, TFT_BLACK), false, 0);
    strokeSpiral(rot, a, maxR, t,        base, false, 0);
  } else if (settings.effectType == 2) {             // Farbverlauf
    strokeSpiral(rot, a, maxR, t, base, true, dim(TFT_MAGENTA));
  } else {                                           // Klassisch
    strokeSpiral(rot, a, maxR, t, base, false, 0);
  }
}

// Markierungspunkt = Schnittpunkt Spirale / Achse (analytisch berechnet)
void drawMarker(int x, int y, uint16_t rawColor) {
  img.fillCircle(x, y, 7, ui.text);
  img.fillCircle(x, y, 5, dim(rawColor));
}

void drawMarkers(float hourRot, float minRot, uint16_t hCol, uint16_t mCol) {
  // Stunde: erste Haelfte oben (12->6), zweite Haelfte unten (6->12)
  bool hDown = hourRot >= PI;
  float rh = HOUR_R0 * expf(GB * (hourRot - (hDown ? PI : 0.0f)));
  drawMarker(centerX, centerY + (hDown ? 1 : -1) * (int)rh, hCol);

  // Minute: erste Haelfte rechts (0->30), zweite Haelfte links (30->60)
  bool mLeft = minRot >= PI;
  float rm = MIN_R0 * expf(GB * (minRot - (mLeft ? PI : 0.0f)));
  drawMarker(centerX + (mLeft ? -1 : 1) * (int)rm, centerY, mCol);
}

// Feature 2: Datum
void drawDigitalDate(const DateTime& now) {
  static const char* days[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
  char buf[32];
  snprintf(buf, sizeof(buf), "%s, %02d.%02d.%04d", days[now.dayOfTheWeek()], now.day(), now.month(), now.year());
  img.setTextDatum(BL_DATUM);
  img.setTextColor(ui.text);
  img.drawString(buf, 10, screenHeight - 6, 2);
}

void drawDigitalTime(const DateTime& now) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d",  now.hour(), now.minute(), now.second());
  img.setTextDatum(TR_DATUM);
  img.setTextColor(ui.text);
  img.drawString(buf, screenWidth - 10 , 0, 2);
}
// ============================================================
// Sekundenanzeige auf einer gedachten Ellipse
//
// 60 Positionen liegen rund um das Zifferblatt.
// Der aktuelle Sekundenpunkt leuchtet hell.
// Die drei vorhergehenden Punkte bilden ein schwaches Nachleuchten.
// ============================================================
void drawSecondDots(int second)
{
    // Ellipse knapp innerhalb des Displayrandes.
    // Bei Bedarf koennen wir diese Werte spaeter optisch anpassen.
    // Ellipsenradien direkt aus den vorhandenen Skalen berechnen.
    // Damit folgt der Sekundenring automatisch der Geometrie
    // der Stunden- und Minutenskala.
    const float radiusX = MIN_R0  * expf(GB * PI);
    const float radiusY = HOUR_R0 * expf(GB * PI);

    // Start bei 12 Uhr.
    const float startAngle = -PI / 2.0f;

    // Grundfarbe der inaktiven Sekundenpunkte.
    uint16_t inactiveColor = tft.color565(55, 55, 60);

    // Leuchtfarbe des aktuellen Sekundenpunktes.
    uint16_t activeColor = rgb888to565(settings.secondRGB);

    for (int s = 0; s < 60; s++) {

        float angle =
            startAngle +
            s * (2.0f * PI / 60.0f);

        int x = centerX +
                (int)(radiusX * cosf(angle));

        int y = centerY +
                (int)(radiusY * sinf(angle));

        // ----------------------------------------------------
        // Aktuelle Sekunde
        // ----------------------------------------------------
        if (s == second) {

            // Schwacher Glow
            uint16_t glow =
                tft.alphaBlend(
                    90,
                    activeColor,
                    TFT_BLACK
                );

            img.fillCircle(x, y, 5, glow);
            img.fillCircle(x, y, 3, activeColor);
            img.drawPixel(x, y, TFT_WHITE);
        }

        // ----------------------------------------------------
        // Eine Sekunde davor
        // ----------------------------------------------------
        else if (s == (second + 59) % 60) {

            uint16_t trail =
                tft.alphaBlend(
                    150,
                    activeColor,
                    TFT_BLACK
                );

            img.fillCircle(x, y, 2, trail);
        }

        // ----------------------------------------------------
        // Zwei Sekunden davor
        // ----------------------------------------------------
        else if (s == (second + 58) % 60) {

            uint16_t trail =
                tft.alphaBlend(
                    90,
                    activeColor,
                    TFT_BLACK
                );

            img.fillCircle(x, y, 2, trail);
        }

        // ----------------------------------------------------
        // Drei Sekunden davor
        // ----------------------------------------------------
        else if (s == (second + 57) % 60) {

            uint16_t trail =
                tft.alphaBlend(
                    50,
                    activeColor,
                    TFT_BLACK
                );

            img.drawPixel(x, y, trail);
        }

        // ----------------------------------------------------
        // Alle uebrigen Sekundenpositionen
        // ----------------------------------------------------
        else {

            // Alle 5 Sekunden etwas deutlicher markieren.
            if ((s % 5) == 0) {
                img.fillCircle(
                    x,
                    y,
                    2,
                    inactiveColor
                );
            } else {
                img.drawPixel(
                    x,
                    y,
                    inactiveColor
                );
            }
        }
    }
}
// ============================================================
//  Ein Bild rendern
// ============================================================
void renderFrame() {
  DateTime now = getNow();

  // Hintergrund neu aufbauen, wenn Stil oder Helligkeit geaendert wurde
  static int lastBg = -1, lastBright = -1;
  if (settings.backgroundType != lastBg || settings.brightness != lastBright) {
    initBackground();
    lastBg = settings.backgroundType;
    lastBright = settings.brightness;
  }
  updateUiColors();

  if (settings.backgroundType == 1) bgSprite.pushToSprite(&img, 0, 0);
  else                              img.fillSprite(TFT_BLACK);

  if (settings.particleEffect) updateAndDrawParticles();

  drawClockFace();

  

  // Drehwinkel: Stunden 12 h = 360 Grad, Minuten 60 min = 360 Grad
  float hourRot = ((now.hour() % 12) + now.minute() / 60.0f + now.second() / 3600.0f) * (2.0f * PI / 12.0f);
  float minRot  = (now.minute() + now.second() / 60.0f) * (2.0f * PI / 60.0f);

  // Farben: manuell (Webseite) oder Tageszeit-Modus
  uint16_t hCol = rgb888to565(settings.hourRGB);
  uint16_t mCol = rgb888to565(settings.minRGB);
  if (settings.circadianMode) {
    hCol = getCircadianColor(now.hour(), true);
    mCol = getCircadianColor(now.hour(), false);
  }

  drawSpiral(hourRot, aHour, hCol, 290.0f);
  drawSpiral(minRot,  aMin,  mCol, 290.0f);
  drawMarkers(hourRot, minRot, hCol, mCol);

  if (settings.showDate) drawDigitalDate(now);
  if (settings.showDate) drawDigitalTime(now);

  // Sekundenpunkte auf der aeusseren Ellipse
  drawSecondDots(now.second());

  img.pushSprite(0, 0);
}

// ============================================================
//  NTP-Zeitsynchronisierung
// ============================================================
void startNtpSync() {
  if (rtcOk || WiFi.status() != WL_CONNECTED) return;

  Serial.printf("NTP: Synchronisierung gestartet - Zeitzone: %s\n",
                TIMEZONES[settings.timezoneIndex].name);
  configTzTime(TIMEZONES[settings.timezoneIndex].posix,
               "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  ntpSyncRequested = true;
  ntpRequestStartedMs = millis();
  lastNtpAttemptMs = millis();
}

void serviceNtp() {
  if (rtcOk || WiFi.status() != WL_CONNECTED) return;

  // Erster Abgleich sofort; danach etwa alle 12 Stunden erneut anfordern.
  if (!ntpSyncRequested &&
      (!ntpTimeValid || millis() - lastNtpAttemptMs >= 12UL * 60UL * 60UL * 1000UL)) {
    startNtpSync();
  }

  if (!ntpSyncRequested) return;

  time_t nowEpoch = time(nullptr);
  struct tm localTm;

  if (localtime_r(&nowEpoch, &localTm) && localTm.tm_year + 1900 >= 2024) {
    ntpSyncRequested = false;
    ntpTimeValid = true;
    lastNtpSyncEpoch = nowEpoch;

    Serial.printf("NTP: OK - %02d.%02d.%04d %02d:%02d:%02d\n",
                  localTm.tm_mday, localTm.tm_mon + 1, localTm.tm_year + 1900,
                  localTm.tm_hour, localTm.tm_min, localTm.tm_sec);
    return;
  }

  if (millis() - ntpRequestStartedMs >= 15000) {
    ntpSyncRequested = false;
    Serial.println("NTP: innerhalb von 15 s keine gueltige Zeit erhalten.");
  }
}

String timeSourceText() {
  if (rtcOk) return "Hardware-RTC";
  if (ntpTimeValid) return "NTP";
  return "Software-Uhr";
}

String lastNtpSyncText() {
  if (!ntpTimeValid || lastNtpSyncEpoch == 0) return "noch keine";

  struct tm t;
  if (!localtime_r(&lastNtpSyncEpoch, &t)) return "unbekannt";

  char b[32];
  snprintf(b, sizeof(b), "%02d.%02d.%04d %02d:%02d:%02d",
           t.tm_mday, t.tm_mon + 1, t.tm_year + 1900,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(b);
}

// ============================================================
//  Touch + lokale Infoseite
// ============================================================

// Nur die Anzahl der Beruehrungspunkte lesen.
// Fuer "irgendwo antippen" brauchen wir keine kalibrierten X/Y-Koordinaten.
bool isDisplayTouched() {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(TOUCH_REG_TD_STATUS);
  if (Wire.endTransmission(false) != 0) return false;

  if (Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)1) != 1) return false;

  uint8_t touches = Wire.read() & 0x0F;
  return touches > 0 && touches <= 2;
}

// Infoseite aktivieren bzw. die 10-Sekunden-Frist neu starten.
void showInfoScreen() {
  infoScreenActive = true;
  infoScreenUntilMs = millis() + INFO_SCREEN_DURATION_MS;
  lastInfoRedrawMs = 0;
}

// Infoseite direkt auf dem TFT zeichnen.
// Sie wird waehrend der Anzeige regelmaessig aktualisiert, damit z.B.
// eine spaeter zustande gekommene Haus-WLAN-IP sichtbar wird.
void drawInfoScreen() {
  // Die komplette Infoseite zuerst unsichtbar im vorhandenen PSRAM-Sprite
  // aufbauen und erst danach in einem Schritt auf das TFT uebertragen.
  // Dadurch gibt es kein sichtbares Loeschen/Neu-Zeichnen mehr.
  img.fillSprite(TFT_BLACK);
  img.setTextDatum(TL_DATUM);

  int y = 12;
  img.setTextColor(TFT_CYAN, TFT_BLACK);
  img.drawString("Fibonacci Spiralen-Uhr", 14, y, 4);
  y += 44;

  img.setTextColor(TFT_YELLOW, TFT_BLACK);
  img.drawString("Setup-WLAN", 14, y, 2);
  y += 21;

  img.setTextColor(TFT_WHITE, TFT_BLACK);
  img.drawString(String("SSID: ") + AP_SSID, 14, y, 2);
  y += 19;
  img.drawString(String("IP:   ") + WiFi.softAPIP().toString(), 14, y, 2);
  y += 27;

  img.setTextColor(TFT_YELLOW, TFT_BLACK);
  img.drawString("Haus-WLAN", 14, y, 2);
  y += 21;

  img.setTextColor(TFT_WHITE, TFT_BLACK);
  if (!settings.wifiEnabled) {
    img.drawString("Deaktiviert", 14, y, 2);
    y += 19;
  } else {
    img.drawString(String("SSID: ") + settings.wifiSSID, 14, y, 2);
    y += 19;

    if (WiFi.status() == WL_CONNECTED) {
      img.setTextColor(TFT_GREEN, TFT_BLACK);
      img.drawString("Status: Verbunden", 14, y, 2);
      y += 19;
      img.setTextColor(TFT_WHITE, TFT_BLACK);
      img.drawString(String("IP:     ") + WiFi.localIP().toString(), 14, y, 2);
      y += 19;
    } else if (wifiConnectPending) {
      img.setTextColor(TFT_YELLOW, TFT_BLACK);
      img.drawString("Status: Verbindung wird hergestellt ...", 14, y, 2);
      y += 19;
    } else {
      img.setTextColor(TFT_RED, TFT_BLACK);
      img.drawString("Status: Nicht verbunden", 14, y, 2);
      y += 19;
    }
  }

  y += 8;
  img.setTextColor(TFT_YELLOW, TFT_BLACK);
  img.drawString("Zeitquelle", 14, y, 2);
  y += 21;

  img.setTextColor(TFT_WHITE, TFT_BLACK);
  img.drawString(timeSourceText(), 14, y, 2);
  y += 19;

  img.setTextColor(TFT_DARKGREY, TFT_BLACK);
  img.drawString(String("Zone: ") + TIMEZONES[settings.timezoneIndex].name, 14, y, 2);
  y += 19;

  if (ntpTimeValid) {
    img.setTextColor(TFT_DARKGREY, TFT_BLACK);
    img.drawString(String("NTP-Abgleich: ") + lastNtpSyncText(), 14, y, 2);
  } else if (ntpSyncRequested) {
    img.setTextColor(TFT_YELLOW, TFT_BLACK);
    img.drawString("NTP: Synchronisierung ...", 14, y, 2);
  }

  // Hinweis am unteren Rand.
  img.setTextDatum(BC_DATUM);
  img.setTextColor(TFT_DARKGREY, TFT_BLACK);
  img.drawString("Display antippen: Info fuer 10 Sekunden",
                 screenWidth / 2, screenHeight - 7, 2);

  // Erst jetzt das komplett fertige Bild sichtbar machen.
  img.pushSprite(0, 0);

  // Datum fuer spaetere Uhrendarstellung wieder auf Standard setzen.
  img.setTextDatum(TL_DATUM);
}

// Touch auf steigende Flanke auswerten.
// Ein gehaltenes Display verlaengert die Anzeige dadurch nicht permanent.
void serviceTouchAndInfo() {
  bool touched = isDisplayTouched();

  if (touched && !touchWasDown) {
    Serial.println("Touch erkannt -> Infoseite fuer 10 Sekunden");
    showInfoScreen();
  }
  touchWasDown = touched;

  if (!infoScreenActive) return;

  // Einmal pro Sekunde neu zeichnen, damit WLAN-Status/IP aktuell bleiben.
  // Das komplette Bild wird dabei flimmerfrei aus dem Sprite uebertragen.
  if (lastInfoRedrawMs == 0 || millis() - lastInfoRedrawMs >= 1000) {
    lastInfoRedrawMs = millis();
    drawInfoScreen();
  }

  // Ueberlaufssicher pruefen, ob die 10 Sekunden abgelaufen sind.
  if ((int32_t)(millis() - infoScreenUntilMs) >= 0) {
    infoScreenActive = false;
    Serial.println("Infoseite beendet -> Uhr");
  }
}

// ============================================================
//  Webserver: Einstellungsseite
// ============================================================
uint32_t parseColor(const String& s, uint32_t fallback) {
  if (s.length() != 7 || s[0] != '#') return fallback;
  return (uint32_t)strtoul(s.c_str() + 1, NULL, 16);
}

String hexColor(uint32_t c) {
  char b[8];
  snprintf(b, sizeof(b), "#%06X", (unsigned)(c & 0xFFFFFF));
  return String(b);
}

String optionTag(int value, const char* text, int current) {
  String o = "<option value=\"";
  o += value;
  o += "\"";
  if (value == current) o += " selected";
  o += ">";
  o += text;
  o += "</option>";
  return o;
}

String checkTag(const char* name, const char* id, const char* text, bool on) {
  String c = "<div class=\"sw\"><label for=\"";
  c += id;
  c += "\">";
  c += text;
  c += "</label><input type=\"checkbox\" id=\"";
  c += id;
  c += "\" name=\"";
  c += name;
  c += "\" value=\"1\"";
  if (on) c += " checked";
  c += "></div>";
  return c;
}

// HTML-Sonderzeichen fuer die Anzeige von SSIDs maskieren.
String htmlEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

// Verbindung zum gespeicherten Haus-WLAN starten.
// Die Funktion wartet nicht auf die Verbindung.
void startStationWifi() {
  wifiConnectPending = false;

  if (!settings.wifiEnabled || settings.wifiSSID.length() == 0) {
    WiFi.disconnect(false);
    Serial.println("Haus-WLAN: deaktiviert");
    return;
  }

  Serial.printf("Haus-WLAN: Verbindungsversuch mit \"%s\" gestartet ...\n",
                settings.wifiSSID.c_str());

  WiFi.begin(settings.wifiSSID.c_str(), settings.wifiPassword.c_str());
  wifiConnectStartedMs = millis();
  wifiConnectPending = true;
}

// WLAN-Aenderungen erst nach Abschluss der HTTP-Antwort anwenden.
void servicePendingWifiRestart() {
  if (!wifiRestartPending) return;
  if ((int32_t)(millis() - wifiRestartAtMs) < 0) return;

  wifiRestartPending = false;
  wifiConnectPending = false;

  if (!settings.wifiEnabled) {
    Serial.println("Haus-WLAN: wird deaktiviert.");
    WiFi.disconnect(false);
    return;
  }

  Serial.println("Haus-WLAN: Konfiguration geaendert -> Neuverbindung.");
  WiFi.disconnect(false);
  delay(20);
  startStationWifi();
}

// Verbindung im Hintergrund ueberwachen, ohne Anzeige/Webserver zu blockieren.
void serviceStationWifi() {
  if (!settings.wifiEnabled || !wifiConnectPending) return;

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectPending = false;
    Serial.printf("Haus-WLAN: VERBUNDEN, IP = %s\n",
                  WiFi.localIP().toString().c_str());
    return;
  }

  if (millis() - wifiConnectStartedMs >= WIFI_CONNECT_TIMEOUT_MS) {
    wifiConnectPending = false;
    Serial.println("Haus-WLAN: innerhalb von 15 s nicht verbunden.");
    Serial.println("Setup-Access-Point bleibt weiterhin erreichbar.");
  }
}

String buildPage() {
  String p;
  p.reserve(7000);

  p += R"rawliteral(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fibonacci Uhr</title>
<style>
  body { font-family: 'Segoe UI', Arial, sans-serif; background: #151515; color: #eaeaea; text-align: center; margin: 0; padding: 20px; }
  .container { max-width: 450px; margin: 0 auto; background: #252525; padding: 25px; border-radius: 15px; box-shadow: 0 8px 24px rgba(0,0,0,0.6); }
  h2 { color: #00bfff; margin-bottom: 25px; font-weight: 400; }
  .section { background: #303030; padding: 15px; border-radius: 8px; margin-bottom: 18px; text-align: left; }
  .section h3 { margin-top: 0; color: #ffaa00; font-size: 16px; border-bottom: 1px solid #444; padding-bottom: 5px; }
  label { font-size: 14px; color: #ccc; display: block; margin-bottom: 8px; }
  select, input[type="color"], input[type="text"], input[type="password"] { width: 100%; box-sizing: border-box; padding: 10px; margin-bottom: 12px; border-radius: 6px; border: 1px solid #444; background: #222; color: #fff; }
  input[type="color"] { height: 50px; cursor: pointer; border: none; padding: 0; }
  .slider { display: flex; align-items: center; margin-bottom: 10px; }
  .slider input { flex-grow: 1; margin: 0 10px 0 0; accent-color: #00bfff; }
  .val { min-width: 42px; text-align: right; font-family: monospace; color: #00bfff; }
  .sw { display: flex; align-items: center; justify-content: space-between; margin-bottom: 12px; }
  .sw label { margin-bottom: 0; cursor: pointer; }
  input[type="checkbox"] { width: 20px; height: 20px; accent-color: #00bfff; cursor: pointer; }
  button { background: #00bfff; width: 100%; padding: 14px; border: none; border-radius: 8px; color: white; cursor: pointer; font-weight: bold; font-size: 16px; }
  button.alt { background: #555; margin-bottom: 0; }
</style></head>
<body><div class="container">
<h2>Fibonacci Konfigurator</h2>
<form action="/save" method="POST">
<div class="section"><h3>Basis-Stil</h3>
<label>Hintergrund:</label><select name="bg">)rawliteral";
  p += optionTag(0, "Tiefschwarz", settings.backgroundType);
  p += optionTag(1, "Gebuerstetes Metall", settings.backgroundType);
  p += "</select><label>Optischer Effekt:</label><select name=\"effect\">";
  p += optionTag(0, "Klassisch (scharfe Linie)", settings.effectType);
  p += optionTag(1, "Neon-Gluehen", settings.effectType);
  p += optionTag(2, "Mathematischer Farbverlauf", settings.effectType);
  p += "</select></div>";

  p += "<div class=\"section\"><h3>Farben (Farbkreis)</h3><label>Stunden-Spirale:</label>"
       "<input type=\"color\" name=\"h_col\" value=\"";
  p += hexColor(settings.hourRGB);
  p += "\"><label>Minuten-Spirale:</label><input type=\"color\" name=\"m_col\" value=\"";
  p += hexColor(settings.minRGB);
  p += "\"><label>Sekundenpunkte:</label><input type=\"color\" name=\"s_col\" value=\"";
  p += hexColor(settings.secondRGB);
  p += "\"></div>";

  p += "<div class=\"section\"><h3>Helligkeit &amp; Staerke</h3><label>Helligkeit (Dimmer):</label>"
       "<div class=\"slider\"><input type=\"range\" name=\"bright\" min=\"10\" max=\"100\" value=\"";
  p += settings.brightness;
  p += "\" oninput=\"this.nextElementSibling.innerText=this.value+'%'\"><span class=\"val\">";
  p += settings.brightness;
  p += "%</span></div><label>Linienstaerke:</label>"
       "<div class=\"slider\"><input type=\"range\" name=\"thickness\" min=\"1\" max=\"5\" value=\"";
  p += settings.thickness;
  p += "\" oninput=\"this.nextElementSibling.innerText=this.value+'px'\"><span class=\"val\">";
  p += settings.thickness;
  p += "px</span></div></div>";

  p += "<div class=\"section\"><h3>Zusatz-Features</h3>";
  p += checkTag("circ", "circ", "Tageszeit-Farbmodus", settings.circadianMode);
  p += checkTag("disp", "disp", "Digitales Datum", settings.showDate);
  p += checkTag("part", "part", "Kosmischer Partikeleffekt", settings.particleEffect);
  p += "</div><button type=\"submit\">Konfiguration anwenden</button></form>";

  // WLAN hat ein eigenes Formular, damit Anzeige-Checkboxen beim
  // Speichern der Netzwerkdaten nicht unbeabsichtigt veraendert werden.
  p += "<form action=\"/savewifi\" method=\"POST\">"
       "<div class=\"section\" style=\"margin-top:18px\"><h3>WLAN &amp; Netzwerk</h3>";
  p += checkTag("wifi_on", "wifi_on", "Mit vorhandenem WLAN verbinden", settings.wifiEnabled);
  p += "<label>WLAN-Name (SSID):</label><input type=\"text\" name=\"wifi_ssid\" value=\"";
  p += htmlEscape(settings.wifiSSID);
  p += "\" maxlength=\"32\" autocomplete=\"off\">";

  p += "<label>WLAN-Passwort:</label>"
       "<input type=\"password\" name=\"wifi_pass\" value=\"\" maxlength=\"64\" "
       "placeholder=\"Leer lassen = gespeichertes Passwort behalten\" autocomplete=\"new-password\">";

  p += "<label>Zeitzone:</label><select name=\"timezone\">";
  for (int i = 0; i < TIMEZONE_COUNT; i++) {
    p += "<option value=\"";
    p += i;
    p += "\"";
    if (i == settings.timezoneIndex) p += " selected";
    p += ">";
    p += TIMEZONES[i].name;
    p += "</option>";
  }
  p += "</select>";

  p += "<label>Status: ";
  if (!settings.wifiEnabled) {
    p += "deaktiviert";
  } else if (WiFi.status() == WL_CONNECTED) {
    p += "verbunden mit ";
    p += htmlEscape(WiFi.SSID());
    p += " &ndash; IP ";
    p += WiFi.localIP().toString();
  } else if (wifiConnectPending) {
    p += "Verbindung wird hergestellt ...";
  } else {
    p += "nicht verbunden";
  }
  p += "</label>";

  p += "<label>Setup-Zugang: ";
  p += AP_SSID;
  p += " &ndash; ";
  p += WiFi.softAPIP().toString();
  p += "</label>";

  p += "<button type=\"submit\">WLAN-Einstellungen speichern</button></div></form>";

  DateTime now = getNow();
  char tb[40];
  snprintf(tb, sizeof(tb), "%02d.%02d.%04d  %02d:%02d:%02d", now.day(), now.month(), now.year(),
           now.hour(), now.minute(), now.second());
  p += "<div class=\"section\" style=\"margin-top:18px\"><h3>Uhrzeit &amp; Zeitquelle</h3><label>Aktuell: ";
  p += tb;
  p += "</label><label>Zeitquelle: ";
  p += timeSourceText();
  p += "</label><label>Zeitzone: ";
  p += TIMEZONES[settings.timezoneIndex].name;
  p += "</label>";
  if (ntpTimeValid) {
    p += "<label>Letzter NTP-Abgleich: ";
    p += lastNtpSyncText();
    p += "</label>";
  } else if (ntpSyncRequested) {
    p += "<label>NTP: Synchronisierung laeuft ...</label>";
  }
  p += R"rawliteral(
<button type="button" class="alt" onclick="syncTime()">Uhrzeit vom Handy uebernehmen</button></div>
<script>
function syncTime(){
  var d=new Date();
  var p=new URLSearchParams({y:d.getFullYear(),mo:d.getMonth()+1,d:d.getDate(),h:d.getHours(),mi:d.getMinutes(),s:d.getSeconds()});
  fetch('/settime',{method:'POST',body:p}).then(function(r){return r.text();}).then(function(t){alert(t);location.reload();});
}
</script></div></body></html>)rawliteral";

  return p;
}

void handleRoot() { server.send(200, "text/html", buildPage()); }

void handleSave() {
  Serial.println();
  Serial.println("=== POST /save empfangen ===");
  for (int i = 0; i < server.args(); i++) {
    Serial.printf("  %s = %s\n", server.argName(i).c_str(), server.arg(i).c_str());
  }

  if (server.hasArg("bg"))        settings.backgroundType = constrain(server.arg("bg").toInt(), 0, 1);
  if (server.hasArg("effect"))    settings.effectType     = constrain(server.arg("effect").toInt(), 0, 2);
  if (server.hasArg("h_col"))     settings.hourRGB        = parseColor(server.arg("h_col"), settings.hourRGB);
  if (server.hasArg("m_col"))     settings.minRGB         = parseColor(server.arg("m_col"), settings.minRGB);
  if (server.hasArg("s_col"))     settings.secondRGB      = parseColor(server.arg("s_col"), settings.secondRGB);
  if (server.hasArg("bright"))    settings.brightness     = constrain(server.arg("bright").toInt(), 10, 100);
  if (server.hasArg("thickness")) settings.thickness      = constrain(server.arg("thickness").toInt(), 1, 5);

  // Checkboxen werden nur uebertragen, wenn sie aktiv sind
  settings.circadianMode  = server.hasArg("circ");
  settings.showDate       = server.hasArg("disp");
  settings.particleEffect = server.hasArg("part");

  saveSettings();

  Serial.println("Gespeicherte Einstellungen:");
  Serial.printf("  Hintergrund : %d\n", settings.backgroundType);
  Serial.printf("  Effekt      : %d\n", settings.effectType);
  Serial.printf("  Stundenfarbe: #%06X\n", (unsigned)(settings.hourRGB & 0xFFFFFF));
  Serial.printf("  Minutenfarbe: #%06X\n", (unsigned)(settings.minRGB & 0xFFFFFF));
  Serial.printf("  Helligkeit  : %d %%\n", settings.brightness);
  Serial.printf("  Linienstaerke: %d px\n", settings.thickness);
  Serial.printf("  Tageszeit   : %s\n", settings.circadianMode ? "AN" : "AUS");
  Serial.printf("  Datum       : %s\n", settings.showDate ? "AN" : "AUS");
  Serial.printf("  Partikel    : %s\n", settings.particleEffect ? "AN" : "AUS");
  Serial.println("=== Einstellungen gespeichert ===");

  // Nichtleere Antwort vermeidet die irrefuehrende WebServer-Warnung.
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "Einstellungen gespeichert");
}

void handleSaveWifi() {
  Serial.println();
  Serial.println("=== POST /savewifi empfangen ===");

  // Alten Zustand merken, damit nur echte WLAN-Aenderungen
  // eine Trennung/Neuverbindung ausloesen.
  bool oldWifiEnabled = settings.wifiEnabled;
  String oldSSID = settings.wifiSSID;
  int oldTimezoneIndex = settings.timezoneIndex;

  settings.wifiEnabled = server.hasArg("wifi_on");

  if (server.hasArg("timezone")) {
    settings.timezoneIndex =
        constrain(server.arg("timezone").toInt(), 0, TIMEZONE_COUNT - 1);
  }

  String newSSID = server.hasArg("wifi_ssid") ? server.arg("wifi_ssid") : "";
  newSSID.trim();
  settings.wifiSSID = newSSID;

  // Leeres Passwort bedeutet: vorhandenes Passwort behalten.
  String newPassword = server.hasArg("wifi_pass") ? server.arg("wifi_pass") : "";
  bool passwordChanged = newPassword.length() > 0;
  if (passwordChanged) settings.wifiPassword = newPassword;

  bool wifiConfigChanged =
      (settings.wifiEnabled != oldWifiEnabled) ||
      (settings.wifiSSID != oldSSID) ||
      passwordChanged;

  bool timezoneChanged = (settings.timezoneIndex != oldTimezoneIndex);

  prefs.begin("fibclock", false);
  prefs.putBool("wifi_on", settings.wifiEnabled);
  prefs.putString("wifi_ssid", settings.wifiSSID);
  prefs.putString("wifi_pass", settings.wifiPassword);
  prefs.putInt("timezone", settings.timezoneIndex);
  prefs.end();

  Serial.printf("  WLAN aktiviert: %s\n", settings.wifiEnabled ? "JA" : "NEIN");
  Serial.printf("  SSID          : %s\n", settings.wifiSSID.c_str());
  Serial.printf("  Passwort      : %s\n", passwordChanged ? "geaendert" : "unveraendert");
  Serial.printf("  Zeitzone      : %s\n", TIMEZONES[settings.timezoneIndex].name);
  Serial.printf("  WLAN-Konfig   : %s\n", wifiConfigChanged ? "GEAENDERT" : "UNVERAENDERT");
  Serial.printf("  Zeitzone      : %s\n", timezoneChanged ? "GEAENDERT" : "UNVERAENDERT");

  // Zeitzonenwechsel benoetigt neuen NTP-Abgleich, aber KEINE WLAN-Trennung.
  if (timezoneChanged) {
    ntpTimeValid = false;
    ntpSyncRequested = false;
    lastNtpSyncEpoch = 0;
    lastNtpAttemptMs = 0;
  }

  // Zuerst HTTP-Antwort abschliessen.
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "WLAN-Einstellungen gespeichert");

  // Nur echte WLAN-Aenderungen spaeter anwenden.
  if (wifiConfigChanged) {
    wifiRestartPending = true;
    wifiRestartAtMs = millis() + 750;
  }
}

void handleSetTime() {
  int y = server.arg("y").toInt(),  mo = server.arg("mo").toInt(), d = server.arg("d").toInt();
  int h = server.arg("h").toInt(),  mi = server.arg("mi").toInt(), s = server.arg("s").toInt();
  if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
    server.send(400, "text/plain", "Ungueltige Zeitangabe");
    return;
  }
  DateTime dt(y, mo, d, h, mi, s);
  if (rtcOk) rtc.adjust(dt);
  else { softBase = dt; softBaseMs = millis(); }
  server.send(200, "text/plain", rtcOk ? "Uhrzeit in die RTC geschrieben" : "Uhrzeit gesetzt (ohne RTC, geht nach Neustart verloren)");
}

void handleNotFound() {
  Serial.printf("HTTP nicht gefunden: %s  Methode=%s\n",
                server.uri().c_str(),
                server.method() == HTTP_GET ? "GET" : "POST/ANDERE");

  // Browser fragen oft automatisch nach favicon.ico. Das ist kein Fehler.
  if (server.uri() == "/favicon.ico") {
    // Nichtleere Antwort vermeidet "content length is zero".
    server.send(200, "text/plain", "no favicon");
    return;
  }

  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Weiterleitung zur Startseite");
}

// ============================================================
//  setup / loop
// ============================================================
void fatal(const char* msg) {
  Serial.println(msg);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.drawString(msg, 10, 10, 2);
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("\n========================================");
  Serial.println("Fibonacci-Uhr Start / Diagnose");
  Serial.println("========================================");
  loadSettings();
  Serial.println("Einstellungen aus Preferences geladen.");

  aHour = HOUR_R0 / expf(GB * (2.5f * PI));   // Spirale so skaliert, dass der Achsenschnitt bei HOUR_R0 beginnt
  aMin  = MIN_R0  / expf(GB * (2.0f * PI));

  // --- RTC (I2C-Pins explizit setzen: Standard-Pins 21/22 sind beim WT32-SC01 vom Display belegt!) ---
  Wire.begin(PIN_SDA, PIN_SCL);

  // ------------------------------------------------------------
// I2C-Bus durchsuchen.
// Damit sehen wir, welche I2C-Bausteine auf SDA=18 / SCL=19
// tatsaechlich antworten.
// ------------------------------------------------------------
Serial.println();
Serial.println("I2C-Scan auf SDA=18 / SCL=19:");

int found = 0;

for (uint8_t address = 1; address < 127; address++)
{
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();

    if (error == 0)
    {
        Serial.printf(
            "  I2C-Geraet gefunden: 0x%02X\n",
            address
        );

        found++;
    }
}

if (found == 0)
{
    Serial.println("  KEIN I2C-Geraet gefunden!");
}
else
{
    Serial.printf("  Insgesamt %d I2C-Geraet(e) gefunden.\n", found);
}

Serial.println();  

  rtcOk = rtc.begin(&Wire);
  Serial.printf("RTC DS3231: %s\n", rtcOk ? "OK" : "NICHT GEFUNDEN");
  softBase = DateTime(F(__DATE__), F(__TIME__));
  softBaseMs = millis();
  if (rtcOk && rtc.lostPower()) {
    Serial.println("RTC hatte keinen Strom - setze Kompilierzeit (bitte ueber die Webseite korrigieren)");
    rtc.adjust(softBase);
  }

  // --- Display ---
  tft.init();
  tft.setRotation(1);          // Querformat; falls das Bild auf dem Kopf steht: 3
  tft.fillScreen(TFT_BLACK);
  Serial.println("Display initialisiert: ST7796 ueber SPI, Rotation 1");

  Serial.printf("PSRAM gefunden: %s\n", psramFound() ? "JA" : "NEIN");
  printMemory("Vor Sprite-Erzeugung");
  if (!psramFound()) fatal("Kein PSRAM gefunden! -BOARD_HAS_PSRAM setzen / PSRAM aktivieren");
  img.setColorDepth(16);
  bgSprite.setColorDepth(16);
  if (!img.createSprite(screenWidth, screenHeight))      fatal("Sprite (img) konnte nicht erzeugt werden");
  Serial.println("Sprite IMG 480x320x16: OK");
  printMemory("Nach Sprite IMG");
  if (!bgSprite.createSprite(screenWidth, screenHeight)) fatal("Sprite (bg) konnte nicht erzeugt werden");
  Serial.println("Sprite BG 480x320x16: OK");
  printMemory("Nach Sprite BG");

  initParticles();

  // --- WLAN: Setup-AP bleibt immer aktiv; Haus-WLAN ist optional ---
  WiFi.mode(WIFI_AP_STA);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("WLAN Access Point: %s\n", apOk ? "OK" : "FEHLER");
  String ip = WiFi.softAPIP().toString();
  Serial.print("Einstellungen: http://");
  Serial.println(ip);

  // Gespeichertes Haus-WLAN im Hintergrund verbinden.
  startStationWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.on("/settime", HTTP_POST, handleSetTime);
  // Favicon-Anfrage des Browsers abfangen
  server.on("/favicon.ico", HTTP_GET, []() {
      server.send(200, "text/plain", "no favicon");
  });
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Webserver gestartet.");

  // --- Infoseite beim Start 10 Sekunden anzeigen ---
  // Kein delay(): WLAN, Webserver und Touch bleiben waehrenddessen aktiv.
  showInfoScreen();
  drawInfoScreen();
}

void loop() {
  server.handleClient();

  // Echte WLAN-Aenderungen erst nach abgeschlossener HTTP-Antwort anwenden.
  servicePendingWifiRestart();

  // Nicht blockierende Ueberwachung des optionalen Haus-WLANs.
  serviceStationWifi();

  // NTP nur bei bestehender Haus-WLAN-Verbindung betreiben.
  serviceNtp();

  // Touch auswerten und ggf. Infoseite anzeigen/aktualisieren.
  serviceTouchAndInfo();

  // Solange die Infoseite aktiv ist, darf die Uhr sie nicht ueberzeichnen.
  if (!infoScreenActive) {
    static uint32_t lastFrame = 0;
    if (millis() - lastFrame >= 30) {
      lastFrame = millis();
      renderFrame();
    }
  }

  delay(1);
}
