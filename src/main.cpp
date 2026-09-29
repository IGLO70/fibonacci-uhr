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
} settings;

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

  // Sekundenpunkte auf der aeusseren Ellipse
  drawSecondDots(now.second());

  img.pushSprite(0, 0);
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
  select, input[type="color"] { width: 100%; box-sizing: border-box; padding: 10px; margin-bottom: 12px; border-radius: 6px; border: 1px solid #444; background: #222; color: #fff; }
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

  DateTime now = getNow();
  char tb[40];
  snprintf(tb, sizeof(tb), "%02d.%02d.%04d  %02d:%02d:%02d", now.day(), now.month(), now.year(),
           now.hour(), now.minute(), now.second());
  p += "<div class=\"section\" style=\"margin-top:18px\"><h3>Uhrzeit (RTC)</h3><label>Aktuell: ";
  p += tb;
  p += rtcOk ? "" : " (Zeitquelle: Software-Uhr, geht nach Neustart verloren)";
  p += R"rawliteral(</label>
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
    server.send(204, "text/plain", "");
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

  // --- WLAN Access Point + Webserver ---
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("WLAN Access Point: %s\n", apOk ? "OK" : "FEHLER");
  String ip = WiFi.softAPIP().toString();
  Serial.print("Einstellungen: http://");
  Serial.println(ip);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/settime", HTTP_POST, handleSetTime);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Webserver gestartet.");

  // --- kurzer Startbildschirm ---
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Fibonacci Spiralen-Uhr", 20, 20, 4);
  tft.drawString(String("WLAN: ") + AP_SSID, 20, 80, 2);
  tft.drawString(String("Passwort: ") + AP_PASS, 20, 105, 2);
  tft.drawString(String("Einstellungen: http://") + ip, 20, 130, 2);
  tft.drawString(rtcOk ? "RTC: OK" : "RTC: NICHT GEFUNDEN (SDA=IO18, SCL=IO19 pruefen)", 20, 165, 2);
  delay(3500);
}

void loop() {
  server.handleClient();

  static uint32_t lastFrame = 0;
  if (millis() - lastFrame >= 30) {
    lastFrame = millis();
    renderFrame();
  }
  delay(1);
}
