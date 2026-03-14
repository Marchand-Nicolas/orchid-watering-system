#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include "env.h"

static const uint64_t US_TO_S_FACTOR = 1000000ULL;
static const uint64_t SLEEP_SECONDS = 3600ULL;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
static const uint8_t WIFI_CONNECT_ATTEMPTS = 3;
static const uint32_t HTTP_TIMEOUT_MS = 10000;
static const uint32_t MAX_WATERING_SECONDS = 3600;
static const int PUMP_PIN = 4;

static const char *wifiStatusToString(wl_status_t status)
{
  switch (status)
  {
  case WL_IDLE_STATUS:
    return "IDLE";
  case WL_NO_SSID_AVAIL:
    return "NO_SSID_AVAIL";
  case WL_SCAN_COMPLETED:
    return "SCAN_COMPLETED";
  case WL_CONNECTED:
    return "CONNECTED";
  case WL_CONNECT_FAILED:
    return "CONNECT_FAILED";
  case WL_CONNECTION_LOST:
    return "CONNECTION_LOST";
  case WL_DISCONNECTED:
    return "DISCONNECTED";
  default:
    return "UNKNOWN";
  }
}

static bool connectToWifi()
{
  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  wl_status_t finalStatus = WL_DISCONNECTED;

  for (uint8_t attempt = 1; attempt <= WIFI_CONNECT_ATTEMPTS; ++attempt)
  {
    Serial.print("WiFi connect attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.println(WIFI_CONNECT_ATTEMPTS);

    WiFi.disconnect(false, true);
    delay(200);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const unsigned long startMs = millis();
    wl_status_t lastStatus = WiFi.status();
    unsigned long lastProgressLogMs = 0;

    Serial.print("Initial WiFi status: ");
    Serial.print(wifiStatusToString(lastStatus));
    Serial.print(" (");
    Serial.print(static_cast<int>(lastStatus));
    Serial.println(")");

    while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_CONNECT_TIMEOUT_MS)
    {
      const wl_status_t currentStatus = WiFi.status();
      if (currentStatus != lastStatus)
      {
        Serial.print("WiFi status changed: ");
        Serial.print(wifiStatusToString(currentStatus));
        Serial.print(" (");
        Serial.print(static_cast<int>(currentStatus));
        Serial.println(")");
        lastStatus = currentStatus;
      }

      if (millis() - lastProgressLogMs >= 5000)
      {
        const unsigned long elapsedSeconds = (millis() - startMs) / 1000;
        Serial.print("Still connecting... elapsed ");
        Serial.print(elapsedSeconds);
        Serial.print("s, status ");
        Serial.print(wifiStatusToString(currentStatus));
        Serial.print(" (");
        Serial.print(static_cast<int>(currentStatus));
        Serial.println(")");
        lastProgressLogMs = millis();
      }

      delay(250);
    }

    finalStatus = WiFi.status();
    if (finalStatus == WL_CONNECTED)
    {
      Serial.print("WiFi connected on attempt ");
      Serial.println(attempt);
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("RSSI (dBm): ");
      Serial.println(WiFi.RSSI());
      Serial.print("Channel: ");
      Serial.println(WiFi.channel());
      return true;
    }

    Serial.print("Attempt failed after ");
    Serial.print((millis() - startMs) / 1000);
    Serial.println("s");
    Serial.print("Attempt final status: ");
    Serial.print(wifiStatusToString(finalStatus));
    Serial.print(" (");
    Serial.print(static_cast<int>(finalStatus));
    Serial.println(")");
  }

  Serial.println("All WiFi connection attempts failed");
  Serial.print("Final WiFi status: ");
  Serial.print(wifiStatusToString(finalStatus));
  Serial.print(" (");
  Serial.print(static_cast<int>(finalStatus));
  Serial.println(")");

  return finalStatus == WL_CONNECTED;
}

static bool fetchWateringInstruction(bool &wateringNeeded, uint32_t &durationSeconds)
{
  String url = String(API_BASE_URL) + "/esp/is_watering_needed?plant_id=" + PLANT_ID + "&token=" + API_TOKEN;

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url))
  {
    Serial.println("Failed to initialize watering check request");
    return false;
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK)
  {
    Serial.print("Watering check failed with HTTP code: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }

  DynamicJsonDocument doc(512);
  const String payload = http.getString();
  http.end();

  DeserializationError error = deserializeJson(doc, payload);
  if (error)
  {
    Serial.print("Invalid JSON from watering check: ");
    Serial.println(error.c_str());
    return false;
  }

  wateringNeeded = doc["watering_needed"] | false;
  durationSeconds = doc["duration"] | 0;
  if (durationSeconds > MAX_WATERING_SECONDS)
  {
    durationSeconds = MAX_WATERING_SECONDS;
  }

  return true;
}

static bool notifyWateringStatus(const char *status)
{
  String url = String(API_BASE_URL) + "/esp/water_plant";

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url))
  {
    Serial.println("Failed to initialize water_plant request");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument body(256);
  body["plant_id"] = PLANT_ID;
  body["status"] = status;
  body["token"] = API_TOKEN;

  String requestBody;
  serializeJson(body, requestBody);

  const int httpCode = http.POST(requestBody);
  if (httpCode < 200 || httpCode >= 300)
  {
    Serial.print("water_plant status update failed (HTTP ");
    Serial.print(httpCode);
    Serial.println(")");
    http.end();
    return false;
  }

  http.end();
  return true;
}

static void waterPlantForDuration(uint32_t durationSeconds)
{
  if (durationSeconds == 0)
  {
    return;
  }

  durationSeconds = min(durationSeconds, MAX_WATERING_SECONDS);

  notifyWateringStatus("started");

  digitalWrite(PUMP_PIN, HIGH);
  delay(durationSeconds * 1000UL);
  digitalWrite(PUMP_PIN, LOW);

  notifyWateringStatus("completed");
}

static void sleepOneCycle()
{
  esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * US_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void setup()
{
  Serial.begin(115200);
  Serial.println("Starting up...");

  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);

  if (connectToWifi())
  {
    Serial.println("WiFi connected");

    bool wateringNeeded = false;
    uint32_t durationSeconds = 0;

    if (fetchWateringInstruction(wateringNeeded, durationSeconds))
    {
      Serial.print("Watering needed: ");
      Serial.println(wateringNeeded ? "true" : "false");

      if (wateringNeeded)
      {
        Serial.print("Watering duration (s): ");
        Serial.println(durationSeconds);
        waterPlantForDuration(durationSeconds);
      }
    }
  }
  else
  {
    Serial.println("WiFi connection failed");
  }

  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);

  Serial.println("Entering deep sleep...");

  sleepOneCycle();
}

void loop()
{
  // Device always deep-sleeps at the end of setup().
}
