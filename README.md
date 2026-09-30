# Fibonacci-Spiralen-Uhr für WT32-SC01

Grafische Fibonacci-Uhr für das **WT32-SC01** mit ESP32-WROVER-B und
3,5"-ST7796-Display (480 × 320).

Die Uhr zeigt Stunden und Minuten mit logarithmischen Spiralen. Dazu
kommen feste Skalen, eine Sekundenellipse mit Leuchtpunkt sowie optional
eine digitale Datums- und Zeitanzeige. Darstellung, WLAN, NTP und
Zeitzone werden über eine Weboberfläche konfiguriert.

## Hardware

- WT32-SC01 mit ESP32-WROVER-B
- ST7796 über SPI
- PSRAM
- Touchcontroller auf I²C-Adresse `0x38`
- WT32-SC01 Extension-Platine Version 1.2

### RTC

Bei der verwendeten Extension-Platine V1.2 ist der RTC-Bestückungsplatz
**U10 nicht bestückt**. Beim I²C-Scan wurde nur der Touchcontroller auf
`0x38` gefunden. Die normale Zeitquelle ist daher NTP. Alternativ kann
die Uhrzeit vom Smartphone übernommen werden.

## Software und PlatformIO

Verwendet werden unter anderem TFT_eSPI 2.5.43, Adafruit RTClib 2.1.4,
ESP32 WiFi/WebServer und `time.h` für NTP.

Wichtige Einstellungen:

- ST7796 über SPI
- PSRAM mit `BOARD_HAS_PSRAM`
- `lib_ldf_mode = deep+`
- SPI-Takt 40 MHz
- MISO GPIO12, MOSI GPIO13, SCLK GPIO14
- CS GPIO15, DC GPIO21, RESET GPIO22, Backlight GPIO23

## Speicher und flimmerfreie Darstellung

Es werden zwei vollständige 16-Bit-Sprites im PSRAM verwendet: `img` für
das aktuelle Bild bzw. die Infoseite und `bgSprite` als
Hintergrund-Cache.

Ein 480×320-Sprite benötigt 307.200 Byte. Nach Erzeugung beider Sprites
blieben beim Test rund **3,58 MB PSRAM frei**.

Auch die Infoseite wird vollständig im Sprite aufgebaut und anschließend
in einem Schritt übertragen. Dadurch bleiben Uhr und Infoseite
flimmerfrei.

## Anzeige der Uhr

### Stunden

Die Stundenspirale wird an der senkrechten Skala abgelesen. Oben liegen
12 bis 6 Uhr, unten 6 bis 12 Uhr. Volle Stunden besitzen verstärkte
Skalenstriche.

### Minuten

Die Minutenspirale wird an der waagrechten Skala abgelesen. Rechts
liegen 0 bis 30 Minuten, links 30 bis 60 Minuten. Alle 5 Minuten wird
ein verstärkter Skalenstrich dargestellt.

Skalen und Beschriftungen sind von der globalen Helligkeitsregelung
entkoppelt und bleiben dadurch gut lesbar.

### Sekunden

60 Punkte liegen auf einer Ellipse, deren Radien aus den äußeren
Endpunkten der Stunden- und Minutenskalen berechnet werden. Der aktuelle
Sekundenpunkt leuchtet und besitzt einen kurzen Nachleuchteffekt. Seine
Farbe ist über die Weboberfläche einstellbar.

### Digitale Datums- und Zeitanzeige

Bei aktivierter Option **„Digitales Datum"** werden zusätzlich
angezeigt:

- Datum am unteren Rand
- digitale Uhrzeit oben rechts im Format `HH:MM:SS`

Digitale Anzeige und Fibonacci-Spiralen verwenden dieselbe Zeitquelle.

## Web-Konfiguration

Die Uhr arbeitet gleichzeitig als eigener Access Point und optional als
Teilnehmer im Haus-WLAN.

### Setup-WLAN

- SSID: `Fibonacci-Clock-Setup`
- Adresse: `http://192.168.4.1`

Dieser Zugang bleibt als Rückfallebene erhalten.

### Haus-WLAN

Nach erfolgreicher Verbindung erhält die Uhr vom Router eine lokale
IP-Adresse, z. B. `192.168.1.138`. Die Einstellungsseite ist dann auch
direkt über diese Adresse aus dem Hausnetz erreichbar.

Konfigurierbar sind WLAN Ein/Aus, SSID, Passwort und Zeitzone. Die Werte
werden in `Preferences` gespeichert. Ein leeres Passwortfeld behält das
gespeicherte Passwort.

Die WLAN-Verbindung wird nur bei echten Änderungen von WLAN Ein/Aus,
SSID oder Passwort neu aufgebaut. Ein Zeitzonenwechsel verursacht
**keinen WLAN-Neustart**.

## NTP und Zeitquellen

Nach Verbindung mit dem Haus-WLAN synchronisiert sich die Uhr
automatisch über NTP.

Verwendete Server:

- `pool.ntp.org`
- `time.google.com`
- `time.cloudflare.com`

Nach erfolgreichem Abgleich läuft die ESP32-Systemzeit weiter. Ein
erneuter Abgleich erfolgt ungefähr alle 12 Stunden.

Priorität der Zeitquellen:

1. Hardware-RTC, falls vorhanden
2. NTP
3. Software-Uhr / manuelle Übernahme vom Smartphone

## Zeitzonen

Die Zeitzone wird über ein Pulldown-Menü ausgewählt und gespeichert.

Verfügbar sind:

- Mitteleuropa -- Wien, Berlin, Zürich, Paris
- Großbritannien / Irland -- London, Dublin
- Osteuropa -- Helsinki, Bukarest
- UTC -- Weltzeit
- USA Eastern -- New York
- USA Central -- Chicago
- USA Mountain -- Denver
- USA Pacific -- Los Angeles
- Japan -- Tokio
- Australien Eastern -- Sydney

POSIX-Zeitzonenregeln sorgen bei den entsprechenden Regionen automatisch
für Sommer-/Winterzeit.

## Infoseite und Touch

Beim Einschalten erscheint für etwa **10 Sekunden** eine Infoseite. Sie
zeigt Setup-WLAN und IP, Haus-WLAN und lokale IP, Verbindungsstatus,
Zeitquelle, Zeitzone und letzten NTP-Abgleich.

Ein Antippen des Displays blendet die Infoseite erneut für 10 Sekunden
ein. Danach erfolgt automatisch die Rückkehr zur Uhr.

## Einstellmöglichkeiten der Anzeige

- Hintergrund: Tiefschwarz oder gebürstetes Metall
- Effekt: Klassisch, Neon-Glühen oder mathematischer Farbverlauf
- getrennte Farben für Stunden-, Minuten- und Sekundenanzeige
- globale Helligkeit
- Linienstärke
- Tageszeit-Farbmodus
- digitales Datum und digitale Uhrzeit
- kosmischer Partikeleffekt

## Manuelle Zeitübernahme

Die Schaltfläche **„Uhrzeit vom Handy übernehmen"** bleibt als
Rückfallebene erhalten. Ohne NTP oder Hardware-RTC geht eine rein
manuell gesetzte Software-Zeit nach einem vollständigen Stromausfall
verloren.

## Projektstruktur

``` text
Fibonacci-Uhr/
├── .gitignore
├── README.md
├── platformio.ini
├── src/
│ └── main.cpp
├── include/
└── lib/
```

Das PlatformIO-Verzeichnis `.pio/` wird nicht in Git gespeichert.

## Diagnose

Der serielle Monitor zeigt unter anderem I²C-/RTC-Status, Display- und
PSRAM-Initialisierung, freien Speicher, WLAN-Status, IP-Adressen,
NTP-Synchronisierung, Zeitzone und empfangene Web-Konfiguration.

## Aktueller getesteter Stand

Erfolgreich getestet:

- ST7796 über SPI und 480 × 320 Querformat
- PSRAM und zwei Vollbild-Sprites
- flimmerfreie Uhr und Infoseite
- Fibonacci-Stunden- und Minutenspiralen
- verstärkte Hauptskalen
- Sekundenellipse mit Nachleuchteffekt
- einstellbare Farben, Helligkeit und Linienstärke
- digitale Datumsanzeige
- digitale Uhrzeitanzeige `HH:MM:SS`
- Web-Konfiguration
- AP+STA gleichzeitig
- Zugriff über Setup-WLAN und Haus-WLAN
- Touch-Infoseite mit automatischer Rückkehr
- NTP-Synchronisierung
- Zeitzonenauswahl und automatische Sommer-/Winterzeit
- Zeitzonenwechsel ohne Netzwerkneustart
- Neustart mit automatischer WLAN- und NTP-Wiederherstellung

## Alternative Hardware -- ungetestet

Die folgenden Boards sind **nicht mit diesem Projekt getestet**. Die
Nennung bedeutet ausdrücklich **keine Funktionsgarantie und keine
zugesicherte Kompatibilität**. Je nach Board müssen `platformio.ini`,
Displaytreiber, GPIO-Belegung, Touchcontroller, Displayrotation,
Hintergrundbeleuchtung und PSRAM-Konfiguration angepasst werden.

Für die derzeitige Architektur ist **PSRAM dringend empfohlen**, da zwei
vollständige 480×320×16-Bit-Sprites verwendet werden.

### WT32-SC01 Plus

ESP32-S3-basierter möglicher Nachfolger. Displayinterface, Pinbelegung
und Controller-Konfiguration unterscheiden sich vom getesteten
WT32-SC01.

**Erwarteter Anpassungsaufwand:** hoch.

### Makerfabs MaTouch ESP32-S3 SPI TFT 3.5"

- ESP32-S3
- 3,5", 480 × 320
- ILI9488 über SPI
- kapazitiver FT6236-Touch
- PSRAM vorhanden

Durch gleiche Auflösung und SPI grundsätzlich interessant. Treiber,
Pins, Touch und PlatformIO-Konfiguration müssen angepasst werden.

**Erwarteter Anpassungsaufwand:** mittel.

### Makerfabs MaTouch ESP32-S3 Parallel TFT 3.5"

- ESP32-S3
- 3,5", 480 × 320
- ILI9488
- 16-Bit-Parallelinterface
- FT6236-Touch
- 8 MB PSRAM

Die Speichergröße passt gut zu den Vollbild-Sprites; die
Displayansteuerung unterscheidet sich jedoch grundlegend von der
getesteten SPI-Version.

**Erwarteter Anpassungsaufwand:** hoch.

### Makerfabs ESP32 3.5" TFT Touch mit Camera

- ESP32-WROVER
- 3,5", 480 × 320
- ILI9488 über SPI
- Varianten mit kapazitivem oder resistivem Touch

Durch ESP32-WROVER, SPI und gleiche Auflösung grundsätzlich interessant.
Display-, Touch- und Pin-Konfiguration müssen dennoch angepasst werden.

**Erwarteter Anpassungsaufwand:** mittel.

### Andere ESP32-/ESP32-S3-Boards

Eine Portierung ist grundsätzlich denkbar, wenn ausreichend PSRAM, WLAN
und ein unterstütztes TFT vorhanden sind. Bei anderer Auflösung müssen
zusätzlich Bildschirmgeometrie, Mittelpunkt, Skalenradien und
Sekundenellipse angepasst werden.

> **Wichtig:** Getestet und dokumentiert ist derzeit ausschließlich das
> WT32-SC01 mit ESP32-WROVER-B und ST7796 über SPI. Alle Alternativen
> sind mögliche Portierungsziele ohne Funktionsgarantie.

## Lizenz

Dieses Projekt wird unter der **PolyForm Noncommercial License 1.0.0**
veröffentlicht.

Die Software darf für nichtkommerzielle Zwecke verwendet, untersucht,
verändert und weitergegeben werden. Eine kommerzielle Nutzung ist nicht
gestattet.

Die vollständigen Bedingungen stehen in `LICENSE.md`.

**Hinweis:** Wegen des Ausschlusses kommerzieller Nutzung ist dies keine
klassische Open-Source-Lizenz im Sinne der Open Source Initiative.
