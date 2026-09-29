# Fibonacci-Spiralen-Uhr für WT32-SC01

Eine grafische Fibonacci-Uhr für das **WT32-SC01** mit ESP32-WROVER-B
und 3,5"-ST7796-Display.

Die Uhr stellt Stunden und Minuten mit zwei logarithmischen Spiralen
dar. Zusätzlich besitzt sie feste Stunden- und Minutenskalen sowie eine
Sekundenanzeige aus 60 Leuchtpunkten auf einer Ellipse. Darstellung,
Farben und Effekte können über eine Weboberfläche eingestellt werden.

## Hardware

-   WT32-SC01 mit ESP32-WROVER-B
-   3,5"-Display, 480 × 320 Pixel
-   ST7796 über SPI
-   PSRAM
-   Touchcontroller auf dem I²C-Bus
-   optionale WT32-SC01 Extension-Platine

### RTC-Hinweis

Bei der verwendeten Extension-Platine **Version 1.2** ist zwar ein
Batteriehalter vorhanden, der RTC-Bestückungsplatz **U10 ist jedoch
nicht bestückt**. Beim I²C-Scan wurde nur der Touchcontroller auf
Adresse `0x38` gefunden.

Das Programm kann deshalb auch ohne Hardware-RTC betrieben werden. Die
Uhrzeit kann über die Weboberfläche vom Smartphone übernommen werden.
Eine spätere WLAN-/NTP-Synchronisierung ist vorgesehen.

## Software

Das Projekt wird mit **PlatformIO** und dem Arduino-Framework erstellt.

Verwendete Bibliotheken:

-   TFT_eSPI 2.5.43
-   Adafruit RTClib 2.1.4

Wichtige PlatformIO-Einstellungen:

-   ST7796 über SPI
-   PSRAM aktiviert (`BOARD_HAS_PSRAM`)
-   `lib_ldf_mode = deep+`

`deep+` ist erforderlich, damit die Abhängigkeiten von RTClib / Adafruit
BusIO einschließlich SPI korrekt erkannt werden.

## Speicher

Für eine flimmerfreie Darstellung werden zwei vollständige
16-Bit-Sprites im PSRAM verwendet:

-   `img` -- aktuelles Bild
-   `bgSprite` -- Hintergrund-Cache

Ein Sprite benötigt `480 × 320 × 2 = 307.200 Byte`. Zwei Sprites
benötigen etwa 614 kB PSRAM. Beim Test blieben nach Erzeugung beider
Sprites rund 3,58 MB PSRAM frei.

## Anzeige der Uhr

### Stunden

Die **Stundenspirale** wird an der senkrechten Skala abgelesen: - obere
Hälfte: 12 bis 6 Uhr - untere Hälfte: 6 bis 12 Uhr - volle Stunden
besitzen verstärkte Skalenstriche

### Minuten

Die **Minutenspirale** wird an der waagrechten Skala abgelesen: - rechte
Hälfte: 0 bis 30 Minuten - linke Hälfte: 30 bis 60 Minuten - alle 5
Minuten wird ein verstärkter Skalenstrich dargestellt

Die Markierungspunkte der Spiralen mit den jeweiligen Skalen zeigen die
aktuelle Zeit.

### Sekunden

60 Punkte liegen auf einer Ellipse um das Zifferblatt. Die Ellipsengröße
wird aus den äußeren Endpunkten der Stunden- und Minutenskalen
abgeleitet.

Der aktuelle Sekundenpunkt leuchtet deutlich und besitzt einen kurzen
Nachleuchteffekt. Die Sekundenpunkte werden zuletzt gezeichnet und
bleiben dadurch auch an Kreuzungen mit den Spiralen sichtbar.

## Web-Konfiguration

Nach dem Start erzeugt die Uhr einen eigenen WLAN-Access-Point.

**WLAN:** `Fibonacci-Clock-Setup`

Die Einstellungsseite ist standardmäßig unter `http://192.168.4.1`
erreichbar.

Bedienung: 1. Smartphone, Tablet oder PC mit `Fibonacci-Clock-Setup`
verbinden. 2. Browser öffnen. 3. `http://192.168.4.1` aufrufen. 4.
Einstellungen ändern. 5. **Konfiguration anwenden** auswählen.

Die Einstellungen werden mit `Preferences` im nichtflüchtigen Speicher
des ESP32 gespeichert und bleiben nach einem Neustart erhalten.

## Einstellmöglichkeiten

### Hintergrund

-   Tiefschwarz
-   Gebürstetes Metall

### Darstellungseffekt

-   Klassisch
-   Neon-Glühen
-   mathematischer Farbverlauf

### Farben

Getrennte Farbauswahl für: - Stundenspirale - Minutenspirale -
Sekundenanzeige

### Helligkeit

Die globale Helligkeit kann über einen Schieberegler eingestellt werden.
Skalen und Beschriftungen sind davon entkoppelt, damit sie auch bei
reduzierter Helligkeit lesbar bleiben.

### Linienstärke

Die Stärke der Spiralen kann über einen Schieberegler verändert werden.

### Zusatzfunktionen

-   Tageszeit-Farbmodus
-   Datumsanzeige
-   kosmischer Partikeleffekt

## Uhrzeit einstellen

Da auf der verwendeten Extension-Platine keine RTC bestückt ist, kann
die Uhrzeit über die Einstellungsseite vom Smartphone übernommen werden:

1.  Mit `Fibonacci-Clock-Setup` verbinden.
2.  Einstellungsseite öffnen.
3.  **Uhrzeit vom Handy übernehmen** auswählen.

Ohne Hardware-RTC geht die Zeit nach einem vollständigen Neustart bzw.
Stromausfall verloren.

## Geplante WLAN-/NTP-Erweiterung

Die Weboberfläche soll später um die Verbindung mit einem vorhandenen
WLAN erweitert werden: - WLAN aktivieren/deaktivieren - SSID -
WLAN-Passwort - Verbindungsstatus - IP-Adresse -
NTP-Zeitsynchronisierung

Der Access Point `Fibonacci-Clock-Setup` soll als Konfigurations- und
Rückfallzugang erhalten bleiben.

Vorgesehene Priorität der Zeitquellen: 1. NTP bei verfügbarem
konfiguriertem WLAN 2. manuelle Zeitübernahme vom Smartphone 3.
Software-Uhr des ESP32

## Projektstruktur

``` text
Fibonacci-Uhr/
├── .gitignore
├── README.md
├── platformio.ini
├── src/
│   └── main.cpp
├── include/
└── lib/
```

Das PlatformIO-Verzeichnis `.pio/` wird nicht in Git gespeichert.

## Hinweise zur Display-Konfiguration

Das WT32-SC01 verwendet in diesem Projekt den **ST7796 über SPI**. Eine
zuvor getestete 8-Bit-Parallel-Konfiguration war für dieses Board nicht
korrekt und führte zu einem dauerhaft weißen Display.

## Diagnose

Beim Programmstart werden im seriellen Monitor unter anderem
ausgegeben: - geladene Einstellungen - I²C-/RTC-Status -
Displayinitialisierung - PSRAM-Status - freier Heap und freies PSRAM -
Sprite-Erzeugung - WLAN-Status und Adresse der Weboberfläche -
Webserver-Status

Beim Speichern über die Weboberfläche werden die empfangenen und
gespeicherten Werte ebenfalls ausgegeben.

## Aktueller Projektstand

Erfolgreich getestet: - ST7796-Display über SPI - Querformat 480 × 320 -
PSRAM - zwei vollständige 16-Bit-Sprites - flimmerfreie Darstellung -
Stunden- und Minutenspiralen - Stunden- und Minutenskalen - verstärkte
Hauptskalen - Sekundenellipse mit Leuchtpunkt - einstellbare Farben und
Effekte - Helligkeitsregelung - Web-Konfiguration - dauerhafte
Speicherung der Einstellungen - Zeitübernahme vom Smartphone

## Lizenz / Verwendung

Dieses Projekt befindet sich in privater Entwicklung. Eine konkrete
Lizenz wurde bisher nicht festgelegt.
