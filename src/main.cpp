/*
  Fibonacci-Spiralen-Uhr - gemeinsamer Programmstand fuer WT32-SC01 und Sunton ESP32-8048S070

  Ablesen der Uhr:
    - Zwei Goldene Spiralen (r = a * e^(b*theta)) drehen sich um die Bildschirmmitte.
    - STUNDEN (Gold):  senkrechte Achse. Obere Haelfte 12-6 Uhr, untere Haelfte 6-12 Uhr.
    - MINUTEN (Blau):  waagrechte Achse. Rechte Haelfte 0-30 min, linke Haelfte 30-60 min.
    - Die Skalen stehen fest. Der Schnittpunkt der Spirale mit der Achse (Markierungspunkt)
      wandert beim Drehen der Spirale von innen nach aussen und zeigt die Zeit.

  Hardware:
    - Zeitquelle: WT32 optional RTC; Sunton NTP/Software-Uhr
    - Einstellungen: WLAN "Fibonacci-Clock-Setup" -> Browser -> http://192.168.4.1
    - Arduino IDE: diese Datei als FibonacciUhr.ino speichern (Inhalt unveraendert)
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "BoardConfig.h"
#include <RTClib.h>
#include <time.h>      // ESP32-Systemzeit / NTP

// ============================================================
//  Konfiguration
// ============================================================

static const char* AP_SSID = "Fibonacci-Clock-Setup";
static const char* AP_PASS = "retro-fibonacci";   // mind. 8 Zeichen

// Referenzdesign 480x320; Geometrie wird proportional auf das reale Display skaliert.
static constexpr float DESIGN_WIDTH = 480.0f;
static constexpr float DESIGN_HEIGHT = 320.0f;
int screenWidth=480, screenHeight=320, centerX=240, centerY=160;
static const float GB = 0.3063487f;

struct DisplayGeometry {
  float scale=1.0f, hourR0=56.0f, minR0=86.0f, spiralMaxRadius=290.0f, spiralSegmentPx=6.0f;
  int axisHalfHeight=152, axisSideMargin=4;
  int tickMajor=6, tickMinor=3, tickHalfThickness=1;
  int hourTextOffset=20, minuteTextOffset=17, boldOffset=1;
  int markerOuter=7, markerInner=5;
  int secondGlow=5, secondActive=3, secondTrail=2;
  int edgeMargin=10, dateBottomMargin=6;
  int particleMinRadius=20, particleMaxRadius=230, particleLargeSize=2;

  // Zentriertes Referenz-Designfeld. Bei abweichendem Seitenverhaeltnis
  // bleibt die Uhr unverzerrt und erhaelt ggf. freie Raender.
  int designPixelWidth=480, designPixelHeight=320;
  int designOffsetX=0, designOffsetY=0;

  // TFT_eSPI-Bitmapfonts koennen nicht stufenlos skaliert werden.
  // Deshalb werden sinnvolle Fontstufen gewaehlt.
  uint8_t fontSmall=2, fontLarge=4;
} geo;

int S(float v) { return max(1, (int)lroundf(v * geo.scale)); }
float SF(float v) { return v * geo.scale; }



float aHour, aMin;

// ============================================================
//  Objekte & Einstellungen
// ============================================================
DisplayDevice display;
DisplaySprite img(&display);       // Zeichen-Buffer (flimmerfrei)
DisplaySprite bgSprite(&display);  // Hintergrund-Cache

void initDisplayGeometry() {
  screenWidth=display.width();
  screenHeight=display.height();
  centerX=screenWidth/2; 
  centerY=screenHeight/2;
  geo.scale=min(screenWidth/DESIGN_WIDTH, screenHeight/DESIGN_HEIGHT);
  geo.hourR0=SF(56); 
  geo.minR0=SF(86); 
  geo.spiralMaxRadius=SF(290); 
  geo.spiralSegmentPx=SF(6);
  geo.axisHalfHeight=S(152); 
  geo.axisSideMargin=S(4);
  geo.tickMajor=S(6); 
  geo.tickMinor=S(3);

  // Hauptstriche bleiben optisch 3 Pixel dick.
  // Nur ihre Laenge wird mit der Displaygeometrie skaliert.
  geo.tickHalfThickness=1;
  geo.hourTextOffset=S(20); 
  geo.minuteTextOffset=S(17);

  // Optischer Fett-Effekt: bewusst NICHT skalieren.
  // Zwei Pixel wirken auf grossen Fonts bereits wie ein Schatten.
  geo.boldOffset=1;
  geo.markerOuter=7; 
  geo.markerInner=5;
  geo.secondGlow=S(5); 
  geo.secondActive=S(3); 
  geo.secondTrail=S(2);
  geo.edgeMargin=S(10); 
  geo.dateBottomMargin=S(6);
  geo.particleMinRadius=S(20); 
  geo.particleMaxRadius=S(230); 
  geo.particleLargeSize=S(2);

  geo.designPixelWidth  = (int)lroundf(DESIGN_WIDTH * geo.scale);
  geo.designPixelHeight = (int)lroundf(DESIGN_HEIGHT * geo.scale);
  geo.designOffsetX = (screenWidth  - geo.designPixelWidth) / 2;
  geo.designOffsetY = (screenHeight - geo.designPixelHeight) / 2;

  // Schriftgroessen stufenweise an groessere Displays anpassen.
  if (geo.scale >= 1.55f) {
    geo.fontSmall = 4;
    geo.fontLarge = 6;
  } else {
    geo.fontSmall = 2;
    geo.fontLarge = 4;
  }

  Serial.printf("Display-Geometrie: %dx%d, Skalierung %.3f, Mittelpunkt %d/%d\n",
                screenWidth, screenHeight, geo.scale, centerX, centerY);
  Serial.printf("Designfeld: %dx%d, Offset %d/%d, Fonts %u/%u\n",
                geo.designPixelWidth, geo.designPixelHeight,
                geo.designOffsetX, geo.designOffsetY,
                geo.fontSmall, geo.fontLarge);
}
   // Hintergrund-Cache (Metall-Textur)
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

// Touch wird beim Sunton direkt von LovyanGFX / GT911 verwaltet.

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
  return display.color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
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

// ------------------------------------------------------------
// RGB565 Alpha-Blending, unabhaengig von TFT_eSPI/LovyanGFX.
//
// alpha = 255 -> Vordergrund voll sichtbar
// alpha =   0 -> Hintergrund voll sichtbar
// ------------------------------------------------------------
uint16_t alphaBlend565(uint8_t alpha, uint16_t fg, uint16_t bg)
{
    uint32_t inv = 255U - alpha;

    uint32_t fr = (fg >> 11) & 0x1F;
    uint32_t fg6 = (fg >> 5) & 0x3F;
    uint32_t fb = fg & 0x1F;

    uint32_t br = (bg >> 11) & 0x1F;
    uint32_t bg6 = (bg >> 5) & 0x3F;
    uint32_t bb = bg & 0x1F;

    uint32_t r = (fr * alpha + br * inv + 127U) / 255U;
    uint32_t g = (fg6 * alpha + bg6 * inv + 127U) / 255U;
    uint32_t b = (fb * alpha + bb * inv + 127U) / 255U;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

inline uint16_t dim(uint16_t c) { return applyBrightness(c, settings.brightness); }

// Feature 1: Farben abhaengig von der Tageszeit
uint16_t getCircadianColor(int hour, bool isHourSpiral) {
  if (hour >= 6 && hour < 18) {   // Tag: kuehle Farben
    return isHourSpiral ? display.color565(230, 240, 255) : display.color565(0, 191, 255);
  }
  // Abend/Nacht: warme, augenschonende Toene
  return isHourSpiral ? display.color565(255, 140, 0) : display.color565(180, 0, 0);
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
        ui.axis = display.color565(35, 35, 40);
        ui.tick = display.color565(20, 20, 24);
        ui.text = display.color565(5, 5, 8);
    }
    else
    {
        // Schwarzer Hintergrund:
        // helle Skalen und Beschriftungen
        ui.axis = display.color565(100, 100, 105);
        ui.tick = display.color565(190, 190, 195);
        ui.text = display.color565(235, 235, 240);
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
      bgSprite.drawFastHLine(x, y, len, display.color565(v, v, vb));
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
    particles[i].radius     = random(geo.particleMinRadius, geo.particleMaxRadius);
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
      uint16_t c = dim(display.color565(b, b, b));
      if (b > 170) img.fillRect(x, y, geo.particleLargeSize, geo.particleLargeSize, c);
      else         img.drawPixel(x, y, c);
    }
  }
}

// ============================================================
//  Zifferblatt: feste Achsen und Skalen
//  Skalenposition = Radius, an dem die Spirale die Achse kreuzt, wenn die Uhr genau diese Zeit zeigt.
// ============================================================
void drawClockFace(int currentHour) {
  // Die Spirale bleibt eine 12-Stunden-Spirale.
  // Nur die Beschriftung wechselt automatisch zwischen
  // 00-12 Uhr und 12-24 Uhr.
  const bool afternoon = currentHour >= 12;
  // Achsen
  img.drawFastVLine(centerX, centerY - geo.axisHalfHeight, 2 * geo.axisHalfHeight + 1, ui.axis);
  img.drawFastHLine(geo.axisSideMargin, centerY, screenWidth - 2 * geo.axisSideMargin, ui.axis);

  img.setTextDatum(MC_DATUM);
  img.setTextColor(ui.text);

  // --- Stundenskala (senkrecht): 36 Teilstriche pro Haelfte = alle 10 min, Hauptstrich jede Stunde ---
  for (int i = 0; i <= 36; i++) {
    float r = geo.hourR0 * expf(GB * i * (PI / 36.0f));
    int yUp = centerY - (int)(r + 0.5f);
    int yDn = centerY + (int)(r + 0.5f);
    bool major = (i % 6 == 0);
    int len = major ? geo.tickMajor : geo.tickMinor;
    // Hauptstriche jeder vollen Stunde deutlich kraeftiger zeichnen
        if (major) {
            for (int dy = -geo.tickHalfThickness; dy <= geo.tickHalfThickness; dy++) {
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
        // Vormittag:  0..6 oben,  6..12 unten
        // Nachmittag: 12..18 oben, 18..24 unten
        // 24 ist die Endmarke; nach 23:59:59 beginnt wieder 0.
        int hourUp = afternoon ? (n + 12) : n;
        int hourDn = afternoon ? (n + 18) : (n + 6);

        boardDrawNumber(img, hourUp, centerX - geo.hourTextOffset, yUp, geo.fontSmall);
        boardDrawNumber(img, hourUp, centerX - geo.hourTextOffset + geo.boldOffset, yUp, geo.fontSmall);

        boardDrawNumber(img, hourDn, centerX - geo.hourTextOffset, yDn, geo.fontSmall);
        boardDrawNumber(img, hourDn, centerX - geo.hourTextOffset + geo.boldOffset, yDn, geo.fontSmall);
            }
        }

  // --- Minutenskala (waagrecht): jede Minute ein Strich, alle 5 min Zahl ---
  for (int i = 0; i <= 30; i++) {
    float r = geo.minR0 * expf(GB * i * (2.0f * PI / 60.0f));
    int xR = centerX + (int)(r + 0.5f);
    int xL = centerX - (int)(r + 0.5f);
    bool major = (i % 5 == 0);
    int len = major ? geo.tickMajor : geo.tickMinor;
    // Alle 5 Minuten wird der Skalenstrich 3 Pixel breit.
        // Die einzelnen Minuten bleiben 1 Pixel breit.
        if (major) {
            for (int dx = -geo.tickHalfThickness; dx <= geo.tickHalfThickness; dx++) {
                img.drawFastVLine(xR + dx, centerY - len, 2 * len + 1, ui.tick);
                img.drawFastVLine(xL + dx, centerY - len, 2 * len + 1, ui.tick);
            }
        } else {
            img.drawFastVLine(xR, centerY - len, 2 * len + 1, ui.tick);
            img.drawFastVLine(xL, centerY - len, 2 * len + 1, ui.tick);
        }
    if (major) {
      // Minutenbeschriftung leicht kraeftiger darstellen.
        boardDrawNumber(img, i,      xR, centerY + geo.minuteTextOffset, geo.fontSmall);
        boardDrawNumber(img, i,      xR + geo.boldOffset, centerY + geo.minuteTextOffset, geo.fontSmall);

        boardDrawNumber(img, 30 + i, xL, centerY + geo.minuteTextOffset, geo.fontSmall);
        boardDrawNumber(img, 30 + i, xL + geo.boldOffset, centerY + geo.minuteTextOffset, geo.fontSmall);
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
    theta += constrain(geo.spiralSegmentPx / r, 0.03f, 0.35f);      // ca. 6 px Segmentlaenge
    r = a * expf(GB * theta);
    if (r > maxR) break;

    float phi = rot - theta;
    float x1 = centerX + r * cosf(phi);
    float y1 = centerY + r * sinf(phi);

    uint16_t c = color;
    if (gradient) {   // Farbverlauf entlang der Spirale
      uint8_t alpha = (uint8_t)constrain(255 - (int)(theta * 14.0f), 40, 255);
      c = alphaBlend565(alpha, color, color2);
    }
    img.drawWideLine(x0, y0, x1, y1, width, c);
    x0 = x1; y0 = y1;
  }
}

void drawSpiral(float rot, float a, uint16_t rawColor, float maxR) {
  const uint16_t base = dim(rawColor);
  // Linienstaerke ist ein optischer Wert und wird NICHT mit der
  // Displaygroesse skaliert. Die Web-Einstellung 1..5 bleibt dadurch
  // auf kleinen und grossen Displays vergleichbar.
  const float t = (float)settings.thickness;

  if (settings.effectType == 1) {
#if defined(BOARD_WT32_SC01)
    // Bewaehrter WT32-Neon-Effekt.
    strokeSpiral(rot, a, maxR, t + 9.0f, alphaBlend565(35,  base, TFT_BLACK), false, 0);
    strokeSpiral(rot, a, maxR, t + 5.0f, alphaBlend565(85,  base, TFT_BLACK), false, 0);
    strokeSpiral(rot, a, maxR, t + 2.0f, alphaBlend565(160, base, TFT_BLACK), false, 0);
    strokeSpiral(rot, a, maxR, t, base, false, 0);
#else
    // Sunton: schmale Neonroehre ohne dunkle Aussenkontur.
    uint16_t neonCore = alphaBlend565(180, TFT_WHITE, base);
    strokeSpiral(rot, a, maxR, t + 2.0f, base, false, 0);
    strokeSpiral(rot, a, maxR, t, neonCore, false, 0);
#endif
  } else if (settings.effectType == 2) {             // Farbverlauf
    strokeSpiral(rot, a, maxR, t, base, true, dim(TFT_MAGENTA));
  } else {                                           // Klassisch
    strokeSpiral(rot, a, maxR, t, base, false, 0);
  }
}

// Markierungspunkt = Schnittpunkt Spirale / Achse (analytisch berechnet)
void drawMarker(int x, int y, uint16_t rawColor) {
  img.fillCircle(x, y, geo.markerOuter, ui.text);
  img.fillCircle(x, y, geo.markerInner, dim(rawColor));
}

void drawMarkers(float hourRot, float minRot, uint16_t hCol, uint16_t mCol) {
  // Stunde: erste Haelfte oben (12->6), zweite Haelfte unten (6->12)
  bool hDown = hourRot >= PI;
  float rh = geo.hourR0 * expf(GB * (hourRot - (hDown ? PI : 0.0f)));
  drawMarker(centerX, centerY + (hDown ? 1 : -1) * (int)rh, hCol);

  // Minute: erste Haelfte rechts (0->30), zweite Haelfte links (30->60)
  bool mLeft = minRot >= PI;
  float rm = geo.minR0 * expf(GB * (minRot - (mLeft ? PI : 0.0f)));
  drawMarker(centerX + (mLeft ? -1 : 1) * (int)rm, centerY, mCol);
}

// Feature 2: Datum
void drawDigitalDate(const DateTime& now) {
  static const char* days[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
  char buf[32];
  snprintf(buf, sizeof(buf), "%s, %02d.%02d.%04d", days[now.dayOfTheWeek()], now.day(), now.month(), now.year());
  img.setTextDatum(BL_DATUM);
  img.setTextColor(ui.text);
  boardDrawString(img, buf, geo.edgeMargin, screenHeight - geo.dateBottomMargin, geo.fontSmall);
}

void drawDigitalTime(const DateTime& now) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d",  now.hour(), now.minute(), now.second());
  img.setTextDatum(TR_DATUM);
  img.setTextColor(ui.text);
  boardDrawString(img, buf, screenWidth - geo.edgeMargin, 0, geo.fontSmall);
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
    const float radiusX = geo.minR0 * expf(GB * PI);
    const float radiusY = geo.hourR0 * expf(GB * PI);

    // Start bei 12 Uhr.
    const float startAngle = -PI / 2.0f;

    // Grundfarbe der inaktiven Sekundenpunkte.
    uint16_t inactiveColor = display.color565(55, 55, 60);

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
                alphaBlend565(
                    90,
                    activeColor,
                    TFT_BLACK
                );

            img.fillCircle(x, y, geo.secondGlow, glow);
            img.fillCircle(x, y, geo.secondActive, activeColor);
            img.drawPixel(x, y, TFT_WHITE);
        }

        // ----------------------------------------------------
        // Eine Sekunde davor
        // ----------------------------------------------------
        else if (s == (second + 59) % 60) {

            uint16_t trail =
                alphaBlend565(
                    150,
                    activeColor,
                    TFT_BLACK
                );

            img.fillCircle(x, y, geo.secondTrail, trail);
        }

        // ----------------------------------------------------
        // Zwei Sekunden davor
        // ----------------------------------------------------
        else if (s == (second + 58) % 60) {

            uint16_t trail =
                alphaBlend565(
                    90,
                    activeColor,
                    TFT_BLACK
                );

            img.fillCircle(x, y, geo.secondTrail, trail);
        }

        // ----------------------------------------------------
        // Drei Sekunden davor
        // ----------------------------------------------------
        else if (s == (second + 57) % 60) {

            uint16_t trail =
                alphaBlend565(
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
                    geo.secondTrail,
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

  if (settings.backgroundType == 1) {
#if defined(BOARD_WT32_SC01)
    bgSprite.pushToSprite(&img, 0, 0);
#else
    img.pushImage(0, 0, screenWidth, screenHeight,
                  (uint16_t*)bgSprite.getBuffer());
#endif
  } else {
    img.fillSprite(TFT_BLACK);
  }

  if (settings.particleEffect) updateAndDrawParticles();

  drawClockFace(now.hour());

  

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

  drawSpiral(hourRot, aHour, hCol, geo.spiralMaxRadius);
  drawSpiral(minRot,  aMin,  mCol, geo.spiralMaxRadius);
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
#if defined(BOARD_WT32_SC01)
  // WT32: FT6x36-kompatibler Touchcontroller direkt ueber I2C.
  Wire.beginTransmission(BOARD_TOUCH_ADDR);
  Wire.write(BOARD_TOUCH_STATUS_REG);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)BOARD_TOUCH_ADDR, (uint8_t)1) != 1) return false;
  uint8_t touches = Wire.read() & 0x0F;
  return touches > 0 && touches <= 2;
#else
  // Sunton: GT911 wird von LovyanGFX verwaltet.
  uint16_t x = 0, y = 0;
  return display.getTouch(&x, &y);
#endif
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

  int y = geo.designOffsetY + S(12);
  img.setTextColor(TFT_CYAN, TFT_BLACK);
  boardDrawString(img, "Fibonacci Spiralen-Uhr", geo.designOffsetX + S(14), y, geo.fontLarge);
  y += S(44);

  img.setTextColor(TFT_YELLOW, TFT_BLACK);
  boardDrawString(img, "Setup-WLAN", geo.designOffsetX + S(14), y, geo.fontSmall);
  y += S(21);

  img.setTextColor(TFT_WHITE, TFT_BLACK);
  boardDrawString(img, String("SSID: ") + AP_SSID, geo.designOffsetX + S(14), y, geo.fontSmall);
  y += S(19);
  boardDrawString(img, String("IP:   ") + WiFi.softAPIP().toString(), geo.designOffsetX + S(14), y, geo.fontSmall);
  y += S(27);

  img.setTextColor(TFT_YELLOW, TFT_BLACK);
  boardDrawString(img, "Haus-WLAN", geo.designOffsetX + S(14), y, geo.fontSmall);
  y += S(21);

  img.setTextColor(TFT_WHITE, TFT_BLACK);
  if (!settings.wifiEnabled) {
    boardDrawString(img, "Deaktiviert", geo.designOffsetX + S(14), y, geo.fontSmall);
    y += S(19);
  } else {
    boardDrawString(img, String("SSID: ") + settings.wifiSSID, geo.designOffsetX + S(14), y, geo.fontSmall);
    y += S(19);

    if (WiFi.status() == WL_CONNECTED) {
      img.setTextColor(TFT_GREEN, TFT_BLACK);
      boardDrawString(img, "Status: Verbunden", geo.designOffsetX + S(14), y, geo.fontSmall);
      y += S(19);
      img.setTextColor(TFT_WHITE, TFT_BLACK);
      boardDrawString(img, String("IP:     ") + WiFi.localIP().toString(), geo.designOffsetX + S(14), y, geo.fontSmall);
      y += S(19);
    } else if (wifiConnectPending) {
      img.setTextColor(TFT_YELLOW, TFT_BLACK);
      boardDrawString(img, "Status: Verbindung wird hergestellt ...", geo.designOffsetX + S(14), y, geo.fontSmall);
      y += S(19);
    } else {
      img.setTextColor(TFT_RED, TFT_BLACK);
      boardDrawString(img, "Status: Nicht verbunden", geo.designOffsetX + S(14), y, geo.fontSmall);
      y += S(19);
    }
  }

  y += S(8);
  img.setTextColor(TFT_YELLOW, TFT_BLACK);
  boardDrawString(img, "Zeitquelle", geo.designOffsetX + S(14), y, geo.fontSmall);
  y += S(21);

  img.setTextColor(TFT_WHITE, TFT_BLACK);
  boardDrawString(img, timeSourceText(), geo.designOffsetX + S(14), y, geo.fontSmall);
  y += S(19);

  img.setTextColor(TFT_DARKGREY, TFT_BLACK);
  boardDrawString(img, String("Zone: ") + TIMEZONES[settings.timezoneIndex].name, geo.designOffsetX + S(14), y, geo.fontSmall);
  y += S(19);

  if (ntpTimeValid) {
    img.setTextColor(TFT_DARKGREY, TFT_BLACK);
    boardDrawString(img, String("NTP-Abgleich: ") + lastNtpSyncText(), geo.designOffsetX + S(14), y, geo.fontSmall);
  } else if (ntpSyncRequested) {
    img.setTextColor(TFT_YELLOW, TFT_BLACK);
    boardDrawString(img, "NTP: Synchronisierung ...", geo.designOffsetX + S(14), y, geo.fontSmall);
  }

  // Hinweis am unteren Rand.
  img.setTextDatum(BC_DATUM);
  img.setTextColor(TFT_DARKGREY, TFT_BLACK);
  boardDrawString(img, "Display antippen: Info fuer 10 Sekunden",
                 screenWidth / 2, screenHeight - S(7), geo.fontSmall);

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
  display.fillScreen(TFT_BLACK);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_RED, TFT_BLACK);
  boardDrawString(display, msg, 10, 10, 2);
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

  // Spiralgeometrie wird nach der Displayinitialisierung berechnet.

  // --- Zeitbasis und Display: boardspezifisch ---
#if defined(BOARD_WT32_SC01)
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  rtcOk = rtc.begin(&Wire);
  Serial.printf("RTC DS3231: %s\n", rtcOk ? "OK" : "NICHT GEFUNDEN");
  softBase = DateTime(F(__DATE__), F(__TIME__));
  softBaseMs = millis();
  if (rtcOk && rtc.lostPower()) {
    Serial.println("RTC hatte keinen Strom - setze Kompilierzeit");
    rtc.adjust(softBase);
  }
  display.init();
  display.setRotation(1);
  display.fillScreen(TFT_BLACK);
  Serial.println("Display initialisiert: WT32-SC01 / ST7796 SPI / Rotation 1");
#else
  rtcOk = false;
  softBase = DateTime(F(__DATE__), F(__TIME__));
  softBaseMs = millis();
  Serial.println("RTC: nicht verwendet - Zeitquelle Software-Uhr / NTP");
  pinMode(BOARD_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BOARD_BACKLIGHT_PIN, HIGH);
  display.init();
  display.setRotation(0);
  display.fillScreen(TFT_BLACK);
  Serial.println("Display initialisiert: Sunton RGB 800x480 / LovyanGFX");
#endif
  Serial.printf("Hardwareprofil: %s\n", BOARD_NAME);

  initDisplayGeometry();
  aHour = geo.hourR0 / expf(GB * (2.5f * PI));
  aMin  = geo.minR0  / expf(GB * (2.0f * PI));

  Serial.printf("PSRAM gefunden: %s\n", psramFound() ? "JA" : "NEIN");
  printMemory("Vor Sprite-Erzeugung");
  if (!psramFound()) fatal("Kein PSRAM gefunden! -BOARD_HAS_PSRAM setzen / PSRAM aktivieren");

  // Speicherbedarf der zwei 16-Bit-Vollbild-Sprites vorab berechnen.
  const size_t spriteBytes = (size_t)screenWidth * (size_t)screenHeight * 2U;
  const size_t twoSpriteBytes = spriteBytes * 2U;
  const size_t psramFreeBeforeSprites = ESP.getFreePsram();
  const size_t safetyReserve = 256U * 1024U;

  Serial.printf("Sprite-Speicher pro Bild : %u Bytes\n", (unsigned)spriteBytes);
  Serial.printf("Sprite-Speicher fuer 2   : %u Bytes\n", (unsigned)twoSpriteBytes);
  Serial.printf("PSRAM Sicherheitsreserve : %u Bytes\n", (unsigned)safetyReserve);

  if (psramFreeBeforeSprites < twoSpriteBytes + safetyReserve) {
    fatal("Zu wenig PSRAM fuer zwei Vollbild-Sprites");
  }

  img.setColorDepth(16);
  bgSprite.setColorDepth(16);

#if defined(BOARD_SUNTON_8048S070)
  // LovyanGFX: Vollbild-Sprites bevorzugt im 8-MB-PSRAM anlegen.
  img.setPsram(true);
  bgSprite.setPsram(true);
#endif
  if (!img.createSprite(screenWidth, screenHeight))      fatal("Sprite (img) konnte nicht erzeugt werden");
  Serial.printf("Sprite IMG %dx%dx16: OK\n", screenWidth, screenHeight);
  printMemory("Nach Sprite IMG");
  if (!bgSprite.createSprite(screenWidth, screenHeight)) fatal("Sprite (bg) konnte nicht erzeugt werden");
  Serial.printf("Sprite BG %dx%dx16: OK\n", screenWidth, screenHeight);
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
    if (millis() - lastFrame >= DISPLAY_FRAME_INTERVAL_MS) {
      lastFrame = millis();
      renderFrame();
    }
  }

  delay(1);
}
