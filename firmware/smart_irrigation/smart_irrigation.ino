/*
  Smart irrigation and field monitor - NodeMCU (ESP8266)

  - Waters automatically when the soil is dry, with hysteresis, a maximum run time
    per cycle, a cooldown, and a rain lock-out.
  - Measures air temperature/humidity (DHT), soil temperature (DS18B20) and rain.
  - Shows live values on an SSD1306 OLED and uploads them to ThingSpeak.
  - A PIR sensor triggers a buzzer when motion is detected near the field.
  - Irrigation keeps working when Wi-Fi is down.

  Copy secrets.example.h to secrets.h and fill in your details before uploading.
  Pins and calibration values are constants at the top: adjust them to your wiring.
*/
#include <ESP8266WiFi.h>
#include <ThingSpeak.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD, TS_CHANNEL_ID, TS_WRITE_API_KEY

// ---------------- Pins (NodeMCU) ----------------
const int PIN_SOIL = A0;           // capacitive soil moisture sensor (analog)
const uint8_t PIN_DHT = D5;        // DHT data
const uint8_t PIN_DS18B20 = D6;    // DS18B20 data (4.7 kOhm pull-up to 3.3 V)
const uint8_t PIN_RELAY = D7;      // relay input (water pump)
const uint8_t PIN_RAIN = D0;       // rain sensor digital output
const uint8_t PIN_PIR = D8;        // PIR motion sensor output
const uint8_t PIN_BUZZER = D3;     // buzzer
// OLED I2C uses the default pins: SDA = D2, SCL = D1

// ---------------- Sensor types and polarity ----------------
#define DHTTYPE DHT22              // change to DHT11 if that is what you use
const bool RELAY_ACTIVE_HIGH = true;
const bool RAIN_ACTIVE_LOW = true; // most rain modules pull DO low when wet

// ---------------- Soil calibration (raw ADC 0-1023) ----------------
// Measure your own sensor: raw value in dry air and raw value in a glass of water.
const int SOIL_DRY_RAW = 780;
const int SOIL_WET_RAW = 380;

// ---------------- Irrigation rules ----------------
const int SOIL_DRY_ON_PCT = 30;    // start watering at or below this
const int SOIL_WET_OFF_PCT = 45;   // stop watering at or above this
const unsigned long PUMP_MAX_ON_MS = 60UL * 1000UL;        // max run per cycle
const unsigned long PUMP_COOLDOWN_MS = 10UL * 60UL * 1000UL; // wait after a max-run stop

// ---------------- Timing ----------------
const unsigned long SENSOR_INTERVAL_MS = 5000;
const unsigned long UPLOAD_INTERVAL_MS = 20000;  // ThingSpeak free tier: >= 15 s
const unsigned long WIFI_RETRY_MS = 30000;
const unsigned long BUZZER_ON_MS = 300;
const unsigned long MOTION_ALERT_GAP_MS = 30000;

// ---------------- Objects ----------------
DHT dht(PIN_DHT, DHTTYPE);
OneWire oneWire(PIN_DS18B20);
DallasTemperature soilTempSensor(&oneWire);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiClient client;

// ---------------- State ----------------
int soilRaw = 0;
int soilPct = 0;
bool soilFault = false;
float airTemp = NAN, airHum = NAN, soilTemp = NAN;
bool raining = false;
bool pumpOn = false;
bool coolingDown = false;
unsigned long pumpStartedAt = 0, cooldownUntil = 0;
unsigned long lastSensor = 0, lastUpload = 0, lastWifiTry = 0;
unsigned long buzzerOffAt = 0, lastMotionAlert = 0;
unsigned long motionCount = 0;
bool oledOk = false;

// ---------------- Helpers ----------------
void setPump(bool on) {
  pumpOn = on;
  bool level = RELAY_ACTIVE_HIGH ? on : !on;
  digitalWrite(PIN_RELAY, level ? HIGH : LOW);
  Serial.println(on ? "Pump ON" : "Pump OFF");
}

int soilPercent(int raw) {
  long p = map(raw, SOIL_DRY_RAW, SOIL_WET_RAW, 0, 100);
  return (int)constrain(p, 0, 100);
}

void readSensors() {
  soilRaw = analogRead(PIN_SOIL);
  // A raw value stuck at the ADC limits usually means a disconnected sensor.
  soilFault = (soilRaw <= 5 || soilRaw >= 1020);
  soilPct = soilPercent(soilRaw);

  airTemp = dht.readTemperature();
  airHum = dht.readHumidity();

  soilTempSensor.requestTemperatures();
  float t = soilTempSensor.getTempCByIndex(0);
  soilTemp = (t == DEVICE_DISCONNECTED_C) ? NAN : t;

  int rainPin = digitalRead(PIN_RAIN);
  raining = RAIN_ACTIVE_LOW ? (rainPin == LOW) : (rainPin == HIGH);
}

void updatePump() {
  unsigned long now = millis();

  if (soilFault) {                 // never run the pump on a faulty soil reading
    if (pumpOn) setPump(false);
    return;
  }

  if (pumpOn) {
    bool tooLong = (now - pumpStartedAt) >= PUMP_MAX_ON_MS;
    if (soilPct >= SOIL_WET_OFF_PCT || raining || tooLong) {
      setPump(false);
      if (tooLong) {
        coolingDown = true;
        cooldownUntil = now + PUMP_COOLDOWN_MS;
      }
    }
  } else {
    if (coolingDown && (long)(now - cooldownUntil) >= 0) coolingDown = false;
    if (!coolingDown && !raining && soilPct <= SOIL_DRY_ON_PCT) {
      setPump(true);
      pumpStartedAt = now;
    }
  }
}

void handleMotion() {
  unsigned long now = millis();
  if (buzzerOffAt != 0 && (long)(now - buzzerOffAt) >= 0) {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerOffAt = 0;
  }
  if (digitalRead(PIN_PIR) == HIGH && (now - lastMotionAlert) >= MOTION_ALERT_GAP_MS) {
    lastMotionAlert = now;
    motionCount++;
    digitalWrite(PIN_BUZZER, HIGH);
    buzzerOffAt = now + BUZZER_ON_MS;
    Serial.println("Motion detected");
  }
}

void connectWifiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWifiTry >= WIFI_RETRY_MS || lastWifiTry == 0) {
    lastWifiTry = now;
    Serial.println("Connecting to Wi-Fi...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void formatValue(char *out, size_t n, float v, const char *unit) {
  if (isnan(v)) snprintf(out, n, "--");
  else snprintf(out, n, "%.1f%s", v, unit);
}

void drawDisplay() {
  if (!oledOk) return;
  char a[16], b[16], c[16], line[32];
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  if (soilFault) snprintf(line, sizeof line, "Soil: SENSOR FAULT");
  else snprintf(line, sizeof line, "Soil: %d%%  Pump:%s", soilPct, pumpOn ? "ON" : "OFF");
  display.print(line);

  formatValue(a, sizeof a, airTemp, "C");
  formatValue(b, sizeof b, airHum, "%");
  display.setCursor(0, 14);
  snprintf(line, sizeof line, "Air: %s  %s", a, b);
  display.print(line);

  formatValue(c, sizeof c, soilTemp, "C");
  display.setCursor(0, 28);
  snprintf(line, sizeof line, "Soil temp: %s", c);
  display.print(line);

  display.setCursor(0, 42);
  snprintf(line, sizeof line, "Rain:%s Motion:%lu", raining ? "yes" : "no", motionCount);
  display.print(line);

  display.setCursor(0, 54);
  display.print(WiFi.status() == WL_CONNECTED ? "WiFi: connected" : "WiFi: offline");
  display.display();
}

void uploadToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;
  ThingSpeak.setField(1, soilFault ? -1 : soilPct);
  if (!isnan(airTemp)) ThingSpeak.setField(2, airTemp);
  if (!isnan(airHum)) ThingSpeak.setField(3, airHum);
  if (!isnan(soilTemp)) ThingSpeak.setField(4, soilTemp);
  ThingSpeak.setField(5, pumpOn ? 1 : 0);
  ThingSpeak.setField(6, raining ? 1 : 0);
  ThingSpeak.setField(7, (int)motionCount);
  int code = ThingSpeak.writeFields(TS_CHANNEL_ID, TS_WRITE_API_KEY);
  Serial.println(code == 200 ? "ThingSpeak update OK" : "ThingSpeak update failed");
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_RELAY, OUTPUT);
  setPump(false);                  // pump off at start-up
  pinMode(PIN_RAIN, INPUT);
  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  dht.begin();
  soilTempSensor.begin();

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledOk) Serial.println("OLED not found, continuing without display");

  WiFi.mode(WIFI_STA);
  ThingSpeak.begin(client);
  connectWifiIfNeeded();
}

void loop() {
  unsigned long now = millis();

  connectWifiIfNeeded();
  handleMotion();

  if (lastSensor == 0 || now - lastSensor >= SENSOR_INTERVAL_MS) {
    lastSensor = now;
    readSensors();
    updatePump();
    drawDisplay();
    Serial.printf("soil=%d%% raw=%d air=%.1fC %.1f%% soilT=%.1fC rain=%d pump=%d\n",
                  soilPct, soilRaw, airTemp, airHum, soilTemp, raining, pumpOn);
  }

  if (lastUpload == 0 || now - lastUpload >= UPLOAD_INTERVAL_MS) {
    lastUpload = now;
    uploadToThingSpeak();
  }
}
