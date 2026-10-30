// iTime v1 — ESP-WROOM-32 + WH1602A 16x2 parallel LCD
// Confirmed hardware configuration: SPI Flash 40 MHz / DIO.
// Build with:
// arduino-cli compile --clean --fqbn "esp32:esp32:esp32:FlashFreq=40,FlashMode=dio" .\firmware\itime
// Never commit secrets.h.

#include <WiFi.h>
#include <LiquidCrystal.h>
#include <time.h>
#include "secrets.h"

// lcd: rs, enable, d4, d5, d6, d7
LiquidCrystal lcd(16, 17, 18, 19, 21, 22);

constexpr long UTC_OFFSET_SECONDS = 4L * 3600L;
constexpr unsigned long DISPLAY_PERIOD_MS = 250;
constexpr unsigned long WIFI_RETRY_MS = 15000;
constexpr unsigned long LOG_PERIOD_MS = 10000;

bool ntpStarted = false;
unsigned long displayStamp = 0;
unsigned long reconnectStamp = 0;
unsigned long logStamp = 0;

void printRow(uint8_t row, const char* text) {
  char padded[17];
  snprintf(padded, sizeof(padded), "%-16.16s", text);
  lcd.setCursor(0, row);
  lcd.print(padded);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  lcd.begin(16, 2);
  printRow(0, "wifi connecting");
  printRow(1, "please wait");

  Serial.println("itime boot ok");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("wifi begin called");
}

void loop() {
  const unsigned long nowMs = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!ntpStarted) {
      configTime(
        UTC_OFFSET_SECONDS,
        0,
        "pool.ntp.org",
        "time.google.com"
      );
      ntpStarted = true;
      Serial.print("ntp requested, ip: ");
      Serial.println(WiFi.localIP());
    }
  } else if (nowMs - reconnectStamp >= WIFI_RETRY_MS) {
    reconnectStamp = nowMs;
    WiFi.reconnect();
    Serial.println("wifi reconnect requested");
  }

  if (nowMs - logStamp >= LOG_PERIOD_MS) {
    logStamp = nowMs;
    Serial.print("uptime_s=");
    Serial.print(nowMs / 1000);
    Serial.print(" wifi_status=");
    Serial.print(WiFi.status());
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print(" rssi=");
      Serial.print(WiFi.RSSI());
    }
    Serial.println();
  }

  if (nowMs - displayStamp >= DISPLAY_PERIOD_MS) {
    displayStamp = nowMs;
    struct tm t;
    if (getLocalTime(&t, 25)) {
      char dateText[17];
      char timeText[17];
      strftime(dateText, sizeof(dateText), "%d.%m.%Y", &t);
      strftime(timeText, sizeof(timeText), "time  %H:%M:%S", &t);
      printRow(0, dateText);
      printRow(1, timeText);
    } else {
      printRow(0, WiFi.status() == WL_CONNECTED
        ? "ntp syncing" : "wifi connecting");
      printRow(1, "please wait");
    }
  }

  delay(20);
}
