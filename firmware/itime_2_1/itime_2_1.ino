#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "openweather_ca.h"
#include <WebServer.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "secrets.h"

// Tested first display wiring from iTime-2.0:
// TFT 1: rst=5, cs=7, dc=6, din=17, clk=18.
// TFT 2: rst=4, cs=8, dc=6, din=17, clk=18.
// Both: VCC=3V3 (J1 short), BL=3V3, GND=GND.
// Shared software SPI clock/data; separate CS and RST.

constexpr int LCD_DC = 6;
constexpr int LCD_MOSI = 17;
constexpr int LCD_SCLK = 18;
constexpr int CLOCK_CS = 7;
constexpr int CLOCK_RST = 5;
constexpr int INFO_CS = 8;
constexpr int INFO_RST = 4;
constexpr long UTC_PLUS_FOUR_SECONDS = 4L * 3600L;

Adafruit_ST7735 clockTft(CLOCK_CS, LCD_DC, LCD_MOSI, LCD_SCLK, CLOCK_RST);
Adafruit_ST7735 infoTft(INFO_CS, LCD_DC, LCD_MOSI, LCD_SCLK, INFO_RST);
WebServer server(80);

bool ntpStarted = false;
bool wasConnected = false;
unsigned long lastClockDraw = 0;
unsigned long lastInfoDraw = 0;
unsigned long lastWifiTry = 0;
unsigned long lastSerialLog = 0;

// Samara city-centre coordinates; adjust to your district if needed.
constexpr float WEATHER_LAT = 53.1956f;
constexpr float WEATHER_LON = 50.1015f;
constexpr uint32_t WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;
// OpenWeather pressure is in hPa; display it in millimetres of mercury.
constexpr float HPA_TO_MMHG = 0.750061683f;
// Display #2: show weather for five minutes, then server status for five seconds.
// This is independent of the OpenWeather API refresh interval above.
constexpr uint32_t WEATHER_PAGE_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t SERVER_PAGE_MS = 5UL * 1000UL;

struct WeatherInfo {
  bool valid = false;
  float temp = 0;
  float feelsLike = 0;
  float wind = 0;
  int humidity = 0;
  int pressure = 0;
  int code = 0;
  String description;
  String error = "waiting for first update";
  time_t updated = 0;
};

WeatherInfo weather;
SemaphoreHandle_t weatherMutex = nullptr;
unsigned long lastScreenSwitch = 0;
unsigned long lastInfoPoll = 0;
bool showWeatherPage = true;  // weather first, server appears briefly every five minutes
bool screenBaseDirty = true;

WeatherInfo weatherSnapshot() {
  WeatherInfo result;
  if (weatherMutex != nullptr && xSemaphoreTake(weatherMutex, pdMS_TO_TICKS(40)) == pdTRUE) {
    result = weather;
    xSemaphoreGive(weatherMutex);
  }
  return result;
}

String conditionEnglish(int code) {
  if (code >= 200 && code < 300) return "thunderstorm";
  if (code >= 300 && code < 400) return "drizzle";
  if (code >= 500 && code < 600) return "rain";
  if (code >= 600 && code < 700) return "snow";
  if (code >= 700 && code < 800) return "mist / haze";
  if (code == 800) return "clear sky";
  if (code == 801) return "few clouds";
  if (code >= 802 && code <= 804) return "cloudy";
  return "unknown";
}

void setWeatherError(const String& message) {
  Serial.print("weather error: ");
  Serial.println(message); // no API key is printed
  if (weatherMutex != nullptr && xSemaphoreTake(weatherMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    weather.error = message;
    xSemaphoreGive(weatherMutex);
  }
}

// A separate FreeRTOS task performs HTTPS; loop() keeps redrawing seconds.
// HTTPS certificate validation is enforced (no setInsecure()).
bool fetchWeather() {
  if (strlen(OPENWEATHER_API_KEY) < 10) {
    setWeatherError("missing api key");
    return false;
  }

  // Check DNS separately: HTTPClient -1 can mean DNS, TCP or TLS failure.
  const char* host = "api.openweathermap.org";
  IPAddress resolved;
  if (WiFi.hostByName(host, resolved) != 1) {
    setWeatherError("dns failed");
    Serial.println("weather dns: failed to resolve api.openweathermap.org");
    return false;
  }
  Serial.print("weather dns: ");
  Serial.println(resolved);
  Serial.printf("weather tls: epoch=%lld, free_heap=%u, rssi=%d dBm\n",
                (long long)time(nullptr), (unsigned)ESP.getFreeHeap(), WiFi.RSSI());

  WiFiClientSecure tls;
  // Validate host + certificate chain; do not disable TLS verification.
  tls.setCACert(OPENWEATHER_ROOT_CA);
  tls.setTimeout(12000);
  HTTPClient https;
  https.setConnectTimeout(12000);
  https.setTimeout(12000);

  String url = "https://api.openweathermap.org/data/2.5/weather?lat=";
  url += String(WEATHER_LAT, 4);
  url += "&lon=";
  url += String(WEATHER_LON, 4);
  url += "&units=metric&lang=ru&appid=";
  url += OPENWEATHER_API_KEY;

  if (!https.begin(tls, url)) {
    setWeatherError("https begin failed");
    return false;
  }
  Serial.println("weather https: connecting to api.openweathermap.org:443");
  const int status = https.GET();
  if (status != HTTP_CODE_OK) {
    if (status < 0) {
      // HTTPClient hides the underlying TLS failure under negative codes.
      char tlsMessage[180] = {0};
      int tlsCode = tls.lastError(tlsMessage, sizeof(tlsMessage));
      Serial.printf("weather http transport error: %d (%s)\n",
                    status, https.errorToString(status).c_str());
      Serial.printf("weather tls error: %d, %s\n", tlsCode,
                    tlsMessage[0] ? tlsMessage : "no TLS detail available");
      setWeatherError(String("https error ") + status);
    } else {
      // A real HTTP status: e.g. 401 means an invalid or inactive API key.
      Serial.printf("weather openweather returned HTTP %d\n", status);
      setWeatherError(String("http status ") + status);
    }
    https.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, https.getString());
  if (err || doc["main"]["temp"].isNull() || doc["wind"]["speed"].isNull()) {
    setWeatherError("bad json response");
    https.end();
    return false;
  }

  WeatherInfo fresh;
  fresh.valid = true;
  fresh.temp = doc["main"]["temp"].as<float>();
  fresh.feelsLike = doc["main"]["feels_like"].as<float>();
  fresh.humidity = doc["main"]["humidity"].as<int>();
  fresh.pressure = doc["main"]["pressure"].as<int>();
  fresh.wind = doc["wind"]["speed"].as<float>();
  fresh.code = doc["weather"][0]["id"].as<int>();
  fresh.description = doc["weather"][0]["description"].as<String>();
  fresh.updated = time(nullptr);
  fresh.error = "";
  https.end();

  if (weatherMutex != nullptr && xSemaphoreTake(weatherMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    weather = fresh;
    xSemaphoreGive(weatherMutex);
  }
  Serial.printf("weather updated: %.1f C, humidity %d%%\n", fresh.temp, fresh.humidity);
  return true;
}

void weatherTask(void*) {
  for (;;) {
    // System time must be set before validating an HTTPS certificate.
    const bool ready = WiFi.status() == WL_CONNECTED && time(nullptr) > 1700000000;
    bool success = false;
    if (ready) success = fetchWeather();
    // Avoid hammering the service. A successful result is cached for 15 min.
    const uint32_t sleepMs = !ready ? 15000UL : success ? WEATHER_REFRESH_MS : 60000UL;
    vTaskDelay(pdMS_TO_TICKS(sleepMs));
  }
}

const char page[] PROGMEM = R"HTML(
<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>iTime-2.0</title>
<style>
body{background:#101723;color:#e9eff9;font:16px Arial,sans-serif;max-width:600px;margin:35px auto;padding:0 16px}
h1{color:#54daef}section{background:#1d293c;border-radius:14px;padding:18px;line-height:1.8}
strong{color:#90d8ff}small{color:#9db4ce}code{color:#d8e6fa}
</style></head><body><h1>iTime-2.0</h1>
<section><div>Время: <strong id="time">...</strong></div>
<div>Wi-Fi: <strong id="wifi">...</strong></div>
<div>IP: <strong id="ip">...</strong></div>
<div>Сигнал: <strong id="rssi">...</strong></div>
<div>Время работы: <strong id="uptime">...</strong></div>
<div>HTTP: <strong>порт 80</strong></div></section>
<h2>Погода · Самара</h2>
<section><div>Температура: <strong id="temp">ожидание...</strong></div>
<div>Ощущается как: <strong id="feels">—</strong></div>
<div>Влажность: <strong id="humidity">—</strong></div>
<div>Ветер: <strong id="wind">—</strong></div>
<div>Давление: <strong id="pressure">—</strong></div>
<div>Состояние: <strong id="condition">—</strong></div>
<div>Данные получены: <strong id="updated">—</strong></div></section>
<p><small>ESP32-S3 N16R8 · 2 × TFT ST7735 · локальный сервер</small></p>
<script>
async function refresh(){try{
const r=await fetch('/api/status',{cache:'no-store'});
if(!r.ok)throw Error(r.status);
const d=await r.json();
document.getElementById('time').textContent=d.time;
document.getElementById('wifi').textContent=d.connected?'подключен':'отключен';
document.getElementById('ip').textContent=d.ip;
document.getElementById('rssi').textContent=d.connected?d.rssi+' dBm':'—';
document.getElementById('uptime').textContent=d.uptime_s+' с';
}catch(e){document.getElementById('wifi').textContent='сервер недоступен';}}
async function refreshWeather(){try{
const r=await fetch('/api/weather',{cache:'no-store'});
if(!r.ok)throw Error(r.status);
const w=await r.json();
if(!w.valid){document.getElementById('temp').textContent=w.error||'ожидание';return;}
document.getElementById('temp').textContent=w.temp_c.toFixed(1)+' °C';
document.getElementById('feels').textContent=w.feels_like_c.toFixed(1)+' °C';
document.getElementById('humidity').textContent=w.humidity_percent+' %';
document.getElementById('wind').textContent=w.wind_ms.toFixed(1)+' м/с';
document.getElementById('pressure').textContent=w.pressure_mmhg.toFixed(1)+' мм рт. ст.';
document.getElementById('condition').textContent=w.description;
document.getElementById('updated').textContent=w.updated;
}catch(e){document.getElementById('temp').textContent='недоступно';}}
refresh();refreshWeather();setInterval(refresh,3000);setInterval(refreshWeather,10000);
</script></body></html>
)HTML";

String localTimeString() {
  struct tm t;
  if (!getLocalTime(&t, 15)) return "not synced";
  char buf[32];
  strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S", &t);
  return String(buf);
}

void configureServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", page);
  });
  server.on("/api/status", HTTP_GET, []() {
    bool online = WiFi.status() == WL_CONNECTED;
    String json;
    json.reserve(220);
    json += "{\"connected\":";
    json += online ? "true" : "false";
    json += ",\"ip\":\"";
    json += online ? WiFi.localIP().toString() : String("-");
    json += "\",\"rssi\":";
    json += online ? String(WiFi.RSSI()) : String(0);
    json += ",\"uptime_s\":";
    json += String(millis() / 1000UL);
    json += ",\"time\":\"";
    json += localTimeString();
    json += "\"}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", json);
  });
  server.on("/api/weather", HTTP_GET, []() {
    WeatherInfo w = weatherSnapshot();
    JsonDocument result;
    result["city"] = "Samara";
    result["valid"] = w.valid;
    result["error"] = w.error;
    if (w.valid) {
      result["temp_c"] = w.temp;
      result["feels_like_c"] = w.feelsLike;
      result["humidity_percent"] = w.humidity;
      result["pressure_hpa"] = w.pressure; // retained for existing API clients
      result["pressure_mmhg"] = w.pressure * HPA_TO_MMHG;
      result["wind_ms"] = w.wind;
      result["weather_code"] = w.code;
      result["description"] = w.description;
      struct tm t;
      char stamp[24];
      if (localtime_r(&w.updated, &t) != nullptr) {
        strftime(stamp, sizeof(stamp), "%d.%m.%Y %H:%M", &t);
        result["updated"] = stamp;
      }
    }
    String json;
    serializeJson(result, json);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", json);
  });
  server.onNotFound([]() {
    server.send(404, "text/plain; charset=utf-8", "not found");
  });
}

// Screen layouts are drawn once. Only changed values are refreshed.
void drawClockBase() {
  clockTft.fillScreen(ST77XX_BLACK);
  clockTft.drawRect(0, 0, clockTft.width(), clockTft.height(), ST77XX_CYAN);
  clockTft.setTextWrap(false);
  clockTft.setTextSize(2);
  clockTft.setTextColor(ST77XX_CYAN);
  clockTft.setCursor(18, 8);
  clockTft.print("iTime 2.1");
  clockTft.drawFastHLine(8, 32, 144, ST77XX_BLUE);
}

void drawClock() {
  static String previousTime = "";
  static String previousDate = "";
  static int previousWifi = -1;

  char timeBuf[12] = "--:--:--";
  char dateBuf[16] = "waiting ntp";
  struct tm t;
  if (getLocalTime(&t, 15)) {
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &t);
    strftime(dateBuf, sizeof(dateBuf), "%d.%m.%Y", &t);
  }

  const String currentTime = timeBuf;
  const String currentDate = dateBuf;
  const int online = WiFi.status() == WL_CONNECTED ? 1 : 0;

  // Time has constant width: print foreground with an opaque background.
  // Unlike fillRect(), this does not blank the entire field first.
  if (currentTime != previousTime) {
    clockTft.setTextSize(3);
    clockTft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    clockTft.setCursor(8, 45);
    clockTft.print(currentTime);
    previousTime = currentTime;
  }

  // Date is static most of the day: no reason to rewrite it every second.
  if (currentDate != previousDate) {
    clockTft.fillRect(12, 85, 139, 26, ST77XX_BLACK);
    clockTft.setTextSize(2);
    clockTft.setTextColor(ST77XX_YELLOW);
    clockTft.setCursor(19, 87);
    clockTft.print(currentDate);
    previousDate = currentDate;
  }

  if (online != previousWifi) {
    clockTft.fillRect(7, 114, 145, 10, ST77XX_BLACK);
    clockTft.setTextSize(1);
    clockTft.setTextColor(online ? ST77XX_GREEN : ST77XX_RED);
    clockTft.setCursor(7, 114);
    clockTft.print(online ? "wifi online" : "wifi offline");
    previousWifi = online;
  }
}

void drawInfoBase() {
  // Border is drawn once; only the inner region changes on page switches.
  infoTft.fillScreen(ST77XX_BLACK);
  infoTft.drawRect(0, 0, infoTft.width(), infoTft.height(), ST77XX_GREEN);
  infoTft.setTextWrap(false);
  screenBaseDirty = true;
}

void drawInfoPageTitle() {
  infoTft.fillRect(3, 4, 154, 119, ST77XX_BLACK);
  infoTft.setTextColor(ST77XX_CYAN);
  infoTft.setTextSize(2);
  infoTft.setCursor(8, 8);
  infoTft.print(showWeatherPage ? "weather" : "iTime server");
  infoTft.drawFastHLine(6, 30, 148, ST77XX_BLUE);
  infoTft.setTextSize(1);
  infoTft.setTextColor(ST77XX_YELLOW);
  infoTft.setCursor(8, 108);
  infoTft.print(showWeatherPage ? "samara / openweather" : "http server :80");
}

// Redraw a single changing row, preserving the title, border and other rows.
void updateInfoRow(int16_t y, const String& content, uint16_t color) {
  infoTft.fillRect(8, y, 145, 11, ST77XX_BLACK);
  infoTft.setTextSize(1);
  infoTft.setTextColor(color);
  infoTft.setCursor(8, y);
  infoTft.print(content);
}

void drawInfo() {
  static String lastLines[4] = {"", "", "", ""};
  const bool newPage = screenBaseDirty;
  if (newPage) {
    drawInfoPageTitle();
    for (int i=0; i<4; i++) lastLines[i] = "";
    screenBaseDirty = false;
  }

  String lines[4];
  uint16_t colors[4] = { ST77XX_WHITE, ST77XX_WHITE, ST77XX_WHITE, ST77XX_WHITE };

  if (!showWeatherPage) {
    const bool online = WiFi.status() == WL_CONNECTED;
    lines[0] = online ? "wifi: connected" : "wifi: offline";
    colors[0] = online ? ST77XX_GREEN : ST77XX_RED;
    lines[1] = String("ip: ") + (online ? WiFi.localIP().toString() : String("not connected"));
    lines[2] = online ? String("rssi: ") + WiFi.RSSI() + " dbm" : String("rssi: -");
    lines[3] = String("uptime: ") + (millis() / 1000UL) + " s";
  } else {
    WeatherInfo w = weatherSnapshot();
    if (!w.valid) {
      lines[0] = "awaiting weather";
      colors[0] = ST77XX_YELLOW;
      lines[1] = w.error;
      if (WiFi.status() != WL_CONNECTED) {
        lines[2] = "wifi disconnected";
      } else if (time(nullptr) < 1700000000) {
        lines[2] = "waiting for ntp";
      } else {
        lines[2] = "dns / tls: see log";
      }
      lines[3] = "retry in ~60 seconds";
    } else {
      lines[0] = "temp: " + String(w.temp,1) + " C";
      colors[0] = ST77XX_YELLOW;
      lines[1] = "feels: " + String(w.feelsLike,1) + " C";
      lines[2] = "hum " + String(w.humidity) + "% wind " + String(w.wind,1);
      lines[3] = "press: " + String(w.pressure * HPA_TO_MMHG, 0) + " mmHg";
    }
  }
  const int16_t y[4] = {39,56,73,90};
  for (int i=0; i<4; i++) {
    if (newPage || lines[i] != lastLines[i]) {
      updateInfoRow(y[i], lines[i], colors[i]);
      lastLines[i] = lines[i];
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("itime-2.1 weather diagnostics starting");

  // Prevent the second panel from responding during the first panel's init.
  pinMode(CLOCK_CS, OUTPUT);
  pinMode(INFO_CS, OUTPUT);
  digitalWrite(CLOCK_CS, HIGH);
  digitalWrite(INFO_CS, HIGH);

  clockTft.initR(INITR_BLACKTAB);
  clockTft.setRotation(1);  // 160x128 landscape
  drawClockBase();
  drawClock();

  infoTft.initR(INITR_BLACKTAB);
  infoTft.setRotation(1);
  drawInfoBase();
  drawInfo();
  // Start the five-minute weather display interval at boot.
  lastScreenSwitch = millis();

  weatherMutex = xSemaphoreCreateMutex();
  if (weatherMutex == nullptr) {
    Serial.println("fatal: weather mutex creation failed");
  }
  configureServer();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiTry = millis();
  Serial.println("wifi starting");
  if (weatherMutex != nullptr) {
    BaseType_t ok = xTaskCreatePinnedToCore(weatherTask, "owm-fetch", 12288, nullptr, 1, nullptr, 0);
    if (ok != pdPASS) Serial.println("weather task creation failed");
  }
}

void loop() {
  unsigned long now = millis();
  bool online = WiFi.status() == WL_CONNECTED;

  if (online && !wasConnected) {
    Serial.print("wifi connected, ip: ");
    Serial.println(WiFi.localIP());
    if (!ntpStarted) {
      configTime(UTC_PLUS_FOUR_SECONDS, 0, "pool.ntp.org", "time.google.com");
      ntpStarted = true;
    }
    server.begin();
    Serial.println("web server ready on port 80");
    lastInfoDraw = 0;
  }
  if (!online && wasConnected) {
    Serial.println("wifi disconnected");
    lastInfoDraw = 0;
  }
  wasConnected = online;

  if (!online && now - lastWifiTry >= 15000UL) {
    lastWifiTry = now;
    Serial.println("retry wifi");
    WiFi.reconnect();
  }
  if (online) server.handleClient();

  if (now - lastClockDraw >= 1000UL) {
    lastClockDraw = now;
    drawClock();
  }
  // Only change the page when its own display duration has elapsed.
  // millis subtraction stays correct across ordinary counter wraparound.
  const uint32_t pageDuration = showWeatherPage ? WEATHER_PAGE_MS : SERVER_PAGE_MS;
  if ((uint32_t)(now - lastScreenSwitch) >= pageDuration) {
    lastScreenSwitch = now;
    showWeatherPage = !showWeatherPage;
    screenBaseDirty = true;
  }
  if (lastInfoDraw == 0 || now - lastInfoDraw >= 5000UL || screenBaseDirty) {
    lastInfoDraw = now;
    drawInfo();
  }
  if (now - lastSerialLog >= 15000UL) {
    lastSerialLog = now;
    Serial.print("uptime_s: ");
    Serial.print(now / 1000UL);
    Serial.print(" wifi: ");
    Serial.println(online ? "connected" : "offline");
  }
  delay(10);
}
