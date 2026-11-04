# iTime 2.1 — The Dual-Screen Clock That Learned the Weather (Against All Odds)

**Field-tested stable snapshot — 4 November 2026.**  
**Hardware:** ESP32-S3-N16R8 (16 MB Flash, 8 MB PSRAM), **two 1.8-inch 128×160 SPI ST7735 TFT screens**.  
[🇷🇺 Полное руководство на русском](README.md) · **English** · [Original iTime 1.0](../../firmware/itime/)

> We asked for a clock. The ESP32 demanded two displays, a soldering iron, an HTTPS trust store, a FreeRTOS task, and a formal inquiry into the emotional well-being of jumper wires. At least it knows the weather now.

## Verified features

| Component | What actually works |
|---|---|
| Screen 1 | NTP-synchronised **fixed UTC+4** date, clock, seconds and Wi-Fi status |
| Screen 2 | Live weather for **Samara, Russia** and a short network/server status view |
| Display schedule | **Weather for 300 seconds, server for 5 seconds**, repeat |
| Weather refresh | OpenWeather over HTTPS, cached for 15 minutes after a successful response |
| Pressure | **mmHg**, derived from API hectopascals |
| Rendering | Partial updates: no full-screen repaint every second |
| Embedded HTTP server | Browser dashboard, **GET /**, **GET /api/status**, **GET /api/weather** |
| Networking | Wi-Fi reconnect, NTP, TLS certificate checking |
| Planned, not shipped | News from Ubuntu RSS server, Cyrillic headlines, VS1053 audio, OTA |

A genuine prototype has been photographed with both displays running. This is not a stock render pretending to be a passing integration test. Long-term 24/7 reliability testing has **not** been claimed.

## Hardware wiring

TFT pins 1–8 are labeled on the back of the module. Both displays share data and clock, but have their own chip-select and reset pins.

| TFT pin | Screen 1: clock | Screen 2: weather |
|---|---|---|
| 1 RST | GPIO5 | GPIO4 |
| 2 CS | GPIO7 | GPIO8 |
| 3 D/C | GPIO6 | GPIO6 |
| 4 DIN / MOSI | GPIO17 | GPIO17 |
| 5 CLK / SCK | GPIO18 | GPIO18 |
| 6 VCC | 3V3 | 3V3 |
| 7 BL | 3V3 | 3V3 |
| 8 GND | GND | GND |

The code uses **software SPI**. The displays are rotated to landscape orientation (**160×128**).

**Very important:** The particular TFT PCB says **J1 OPEN = 5 V VCC**, **J1 SHORT = 3.3 V VCC**. The verified build uses **shorted J1 with 3.3 V**, not 5 V. With J1 shorted, do not feed 5 V into VCC. Use a meter; a color-coded expansion shield does not have supernatural voltage-detection powers. Remove USB power before moving any jumper wire.

### Engineering drama, condensed

First the screens displayed the same thing. Assigning separate CS and RST lines fixed that; two identical modules don't spontaneously develop individual personalities.

Then one screen was bright and the other was dim. A different J1 position explained part of the problem. The remaining culprit was a **poor electrical contact**, confirmed with a multimeter. An entire debugging saga defeated by a wobbly connector. The future of embedded computing is clearly in excellent hands.

Adding weather initially produced **HTTPClient error -1**. The fix involved DNS/TLS diagnostics, appropriate trust anchors and sensible timeouts. We **did not use setInsecure()** to pretend encrypted networking was optional.

The first implementation also redrew whole screens too often. Now the clock replaces only changed text. Believe it or not, a digital clock does not require erasing the entire known universe sixty times per minute.

## Libraries and architecture

- **Arduino ESP32 core** for ESP32-S3, Wi-Fi, NTP and FreeRTOS.
- **Adafruit GFX**, **Adafruit ST7735/ST7789**, **Adafruit BusIO** for drawing over SPI.
- **ArduinoJson 7** for OpenWeather JSON.
- **WiFiClientSecure + HTTPClient** for authenticated HTTPS.
- **WebServer** for the local dashboard and JSON API.

~~~text
esp32-s3
├─ loop(): clock ~1 s, Wi-Fi maintenance, HTTP server
├─ info TFT: weather 300 s → server 5 s
└─ FreeRTOS weatherTask():
   ├─ wait for Wi-Fi and valid NTP time
   ├─ resolve api.openweathermap.org
   ├─ HTTPS + CA verification + parse JSON
   ├─ lock/update cached weather data
   └─ sleep 15 min on success / retry after failure
~~~

**Display duration and API refresh are independent.** Calling OpenWeather every time the status screen changes would be a fine way to build an expensive, extremely tiny load generator.

### Weather pressure

OpenWeather supplies hPa. We show the more familiar mmHg:

**mmHg = hPa × 0.750061683**

The TFT rounds to the nearest whole millimetre; the web dashboard shows one decimal place. No actual mercury is poured into the breadboard. That really should not need saying, but here we are.

## Flashing on Windows / PowerShell

The verified test setup used **COM4**, **Arduino CLI** and the Downloads working directory. Your serial port may differ.

~~~powershell
cd c:\users\zhoric\downloads
git clone https://github.com/iXimik/iTime.git
cd .\iTime\firmware\itime_2_1
copy-item .\secrets.example.h .\secrets.h
notepad .\secrets.h
~~~

Set the following privately in **secrets.h**:

~~~cpp
#define WIFI_SSID "your_2_4ghz_wifi"
#define WIFI_PASSWORD "your_wifi_password"
#define OPENWEATHER_API_KEY "your_new_openweather_key"
~~~

**Do not commit secrets.h**. If an API key has appeared in screenshots, rotate it before proceeding. There is no achievement badge for donating API credits to strangers.

~~~powershell
arduino-cli core list
arduino-cli lib list
arduino-cli compile --fqbn esp32:esp32:esp32s3 .
arduino-cli board list
arduino-cli upload -p com4 --fqbn esp32:esp32:esp32s3 .
arduino-cli monitor -p com4 --config baudrate=115200
~~~

The exact sketch and directory names must match, so the new source is named **itime_2_1.ino** inside **firmware/itime_2_1/**. The tested original was called **itime_2_0.ino** inside a legacy local folder, and the source contents were preserved unchanged on import.

Arduino Library Manager once returned 403; Adafruit libraries were installed from their official GitHub repositories instead. ArduinoJson 7 is also needed. Do not cargo-cult the 40 MHz Flash workaround from the older ESP-WROOM-32 board onto this ESP32-S3.

## HTTP endpoints and security

When connected, the ESP32 displays its LAN address; the prototype previously used **192.168.10.45**, but DHCP can change it.

**GET /** is a simple page with time, Wi-Fi, IP, RSSI, uptime and weather.

**GET /api/status** returns network connection details and local date/time.

**GET /api/weather** returns cached OpenWeather information, including both **pressure_hpa** and **pressure_mmhg**, temperature, humidity, wind, condition and last successful update.

The browser refreshes status and weather without reloading the whole document. The server listens on port **80 without authentication**, so it belongs on a trusted LAN only. Please do not forward that port to the public internet unless you particularly enjoy unsolicited visitors.

## Troubleshooting

| Symptom | Check |
|---|---|
| Dark or very dim screen | 3.3 V power, **BL**, **J1**, common ground, flaky jumpers |
| Blank screen with power | CS, reset, shared SPI wiring, init sequence |
| Weather error -1 | DNS/TCP/TLS diagnostics over serial at 115200 |
| Weather API 401 | Invalid, expired or not-yet-active OpenWeather key |
| Clock updates but weather waits | Wi-Fi, NTP time, HTTPS and OpenWeather availability |
| Screens flicker | Avoid full-screen clearing on regular data updates |

## Next: iTime 2.2 News

The existing [news_informer_project](https://github.com/iXimik/news_informer_project) already parses RSS feeds on Ubuntu. The next step is a LAN JSON news endpoint and a Cyrillic-capable headline renderer on the second TFT. **This is planned, not part of the field-tested 2.1 snapshot.**

The project uses the repository's [MIT license](../../LICENSE). External Arduino libraries retain their own licenses.

---

**iTime 2.1 — Now you can find out what time it is, what the weather is, and whether your Wi-Fi is having another little existential crisis.**