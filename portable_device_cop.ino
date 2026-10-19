// ClimateNet node 1 -> AWS IoT Core (Living Highlands)
// Reads every sensor once a second and publishes one JSON message per second to
//   highlands/<THING_NAME>/data
// over MQTT/TLS (port 8883) with the Thing's certificate. The local web dashboard still works.
// Pins: I2C SDA 21 / SCL 22, RTC SQW 25, fan tach 27, battery divider (1M/1M) 34.
//
// Libraries (Arduino Library Manager): PubSubClient by Nick O'Leary, plus the ones you already had:
//   RTClib, Sensirion I2C SCD4x, Sensirion I2C SPS30,
//   Adafruit BME280, Adafruit LTR390.
// Wi-Fi, AWS endpoint and certificates go in secrets.h (the other tab).

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <time.h>
#include <sys/time.h>
#include <RTClib.h>
#include <SensirionI2cScd4x.h>
#include <SensirionI2cSps30.h>
#include <Adafruit_BME280.h>
#include <Adafruit_LTR390.h>
#include "secrets.h"

// ---------- settings ----------
const uint32_t PUBLISH_EVERY_MS = 1000;

// ---------- pins ----------
#define SDA_PIN       21
#define SCL_PIN       22
#define RTC_SQW_PIN   25
#define FAN_TACH_PIN  27
#define BATT_SENS_PIN 34

// ---------- sensors, server, cloud ----------
RTC_DS3231 rtc;
Adafruit_BME280 bme;
SensirionI2cScd4x scd;
SensirionI2cSps30 sps;
Adafruit_LTR390 ltr;
WebServer server(80);
WiFiClientSecure net;
PubSubClient mqtt(net);
char topicData[96];

bool rtcAvailable = false;
bool bmeAvailable = false;
bool scdAvailable = false;
bool spsAvailable = false;
bool ltrAvailable = false;

volatile uint32_t fanPulses = 0;
void IRAM_ATTR onFanPulse() { fanPulses++; }

// Average of the readings taken since the last publish, so every message shows what
// is happening *now*. Slow sensors (the SCD41 gives CO2 every 5 s) keep their last value.
struct Win {
  double sum = 0;
  uint32_t n = 0;
  float last = NAN;
  void add(float v) { if (!isnan(v)) { sum += v; n++; } }
  float take() { if (n) { last = sum / n; sum = 0; n = 0; } return last; }  // for publishing
  float peek() const { return n ? sum / n : last; }                          // for the dashboard
};

Win bmeTempW, bmeHumW, bmePressW, pm1W, pm25W, pm4W, pm10W, sizeW, co2W, luxW, uviW;
float lastT = 25, lastH = 50;
float currentFanRpm = 0;
float currentBattery = 0;

// ---------- helpers ----------
void wifiConnect() {
  Serial.print("[WiFi] Connecting to SSID: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected! Local dashboard: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] Connection failed! Restarting...");
    ESP.restart();
  }
}

float readBattery() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BATT_SENS_PIN);
  return mv / 16.0f / 1000.0f * 2.0f;
}

void readSensors() {
  if (bmeAvailable) {
    float t = bme.readTemperature();
    float h = bme.readHumidity();
    float p = bme.readPressure() / 100.0f;
    if (!isnan(t) && !isnan(h)) {
      bmeTempW.add(t); bmeHumW.add(h); bmePressW.add(p);
    }
  }

  if (spsAvailable) {
    float pm1, pm25, pm4, pm10, n05, n1, n25, n4, n10, size;
    uint16_t ready = 0;
    if (sps.readDataReadyFlag(ready) == 0 && ready) {
      if (sps.readMeasurementValuesFloat(pm1, pm25, pm4, pm10, n05, n1, n25, n4, n10, size) == 0) {
        pm1W.add(pm1); pm25W.add(pm25); pm4W.add(pm4); pm10W.add(pm10);
        sizeW.add(size);  // typical particle size: ~0.5 µm smoke, >1 µm dust
      }
    }
  }

  if (scdAvailable) {
    bool co2Ready = false;
    uint16_t co2; float ct, ch;
    if (scd.getDataReadyStatus(co2Ready) == 0 && co2Ready) {
      if (scd.readMeasurement(co2, ct, ch) == 0 && co2 > 0) {
        co2W.add(co2);
      }
    }
  }

  if (ltrAvailable) {
    static bool uvMode = false;
    if (ltr.newDataAvailable()) {
      if (uvMode) {
        uviW.add(ltr.readUVS() / 575.0f);
        ltr.setGain(LTR390_GAIN_1);  ltr.setMode(LTR390_MODE_ALS);
      } else {
        luxW.add(0.6f * ltr.readALS());
        ltr.setGain(LTR390_GAIN_18); ltr.setMode(LTR390_MODE_UVS);
      }
      uvMode = !uvMode;
    }
  }
}

// ---------- clock: NTP when online, the DS3231 keeps it across reboots ----------
// AWS checks certificate dates, so a correct clock helps the TLS connection.
void clockFromRtc() {
  if (!rtcAvailable || rtc.lostPower()) return;
  timeval tv = { (time_t)rtc.now().unixtime(), 0 };
  settimeofday(&tv, nullptr);
  Serial.println("[Time] Clock set from the RTC");
}
bool clockValid() { return time(nullptr) > 1700000000; }
void saveClockToRtcOnce() {
  static bool saved = false;
  if (saved || !rtcAvailable || !clockValid()) return;
  rtc.adjust(DateTime((uint32_t)time(nullptr)));
  saved = true;
  Serial.println("[Time] RTC updated from the internet");
}

// ---------- AWS IoT ----------
void ensureWifi() {
  static uint32_t lastTry = 0;
  if (WiFi.status() == WL_CONNECTED || millis() - lastTry < 10000) return;
  lastTry = millis();
  Serial.println("[WiFi] Lost, reconnecting...");
  WiFi.reconnect();
}

void ensureMqtt() {
  static uint32_t lastTry = 0;
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (!clockValid() && millis() < 20000) return;  // give NTP a few seconds first
  if (lastTry && millis() - lastTry < 5000) return;
  lastTry = millis();
  Serial.print("[MQTT] Connecting to AWS IoT as ");
  Serial.println(THING_NAME);
  // The client ID must be the Thing name: that is what the device policy allows.
  if (mqtt.connect(THING_NAME)) {
    Serial.print("[MQTT] Connected, publishing to ");
    Serial.println(topicData);
  } else {
    // -2: TLS failed (wrong endpoint, badly pasted certificate, or certificate not Active)
    // connects then drops at once: policy not attached, or THING_NAME doesn't match the policy
    Serial.printf("[MQTT] Failed, state=%d\n", mqtt.state());
  }
}

void addField(String& s, const char* key, float v, int dec) {
  s += ",\"";
  s += key;
  s += "\":";
  if (isnan(v)) s += "null";
  else s += String(v, dec);
}

// One message in the format the Living Highlands page reads:
// t, rh, p, co2, pm1, pm25, pm4, pm10, size, lux, uvi (+ extras it ignores)
void publishReadings() {
  float bmeT = bmeTempW.take(), bmeH = bmeHumW.take();
  float t = bmeT;
  float rh = bmeH;

  String s = "{\"id\":\"";
  s += THING_NAME;
  s += "\"";
  addField(s, "p", bmePressW.take(), 2);
  addField(s, "co2", co2W.take(), 0);
  addField(s, "pm1", pm1W.take(), 1);
  addField(s, "pm25", pm25W.take(), 1);
  addField(s, "pm10", pm10W.take(), 1);
  addField(s, "size", sizeW.take(), 3);
  addField(s, "lux", luxW.take(), 1);
  addField(s, "uvi", uviW.take(), 2);
  addField(s, "t", bmeT, 2);
  addField(s, "rh", bmeH, 1);
  addField(s, "fan", currentFanRpm, 0);
  addField(s, "batt", currentBattery, 2);
  s += ",\"up\":";
  s += (unsigned long)(millis() / 1000);
  if (clockValid()) {
    s += ",\"ts\":";
    s += (unsigned long)time(nullptr);
  }
  s += "}";

  if (mqtt.connected() && mqtt.publish(topicData, s.c_str())) {
    Serial.println(s);
  } else {
    Serial.print("[MQTT] Not sent (offline): ");
    Serial.println(s);
  }
}

// ---------- local HTML dashboard (shows the latest values) ----------
void handleRoot() {
  String html = "<html lang=\"en\"><head><meta charset=\"UTF-8\">";
  html += "<meta http-equiv=\"refresh\" content=\"5\">";
  html += "<title>ClimateNet Live Dashboard</title>";
  html += "<style>body{font-family:Arial,sans-serif;background:#f4f4f9;color:#333;margin:0;padding:20px;}";
  html += "h1{color:#0066cc;} .card{background:#fff;padding:15px;margin-bottom:10px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}";
  html += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:15px;}";
  html += ".val{font-size:24px;font-weight:bold;color:#00796b;}</style></head><body>";
  html += "<h1>ClimateNet Node Live Monitor</h1>";
  html += "<p>AWS IoT: ";
  html += mqtt.connected() ? "connected, publishing to " + String(topicData) : String("not connected");
  html += ". Page auto-refreshes every 5 seconds.</p><div class=\"grid\">";

  auto addCard = [&](String title, float val, String unit) {
    html += "<div class=\"card\"><h3>" + title + "</h3><div class=\"val\">";
    if (isnan(val)) html += "N/A";
    else html += String(val, 1) + " " + unit;
    html += "</div></div>";
  };

  addCard("BME Temperature", bmeTempW.peek(), "°C");
  addCard("BME Humidity", bmeHumW.peek(), "%");
  addCard("Pressure", bmePressW.peek(), "hPa");
  addCard("CO2", co2W.peek(), "ppm");
  addCard("PM2.5", pm25W.peek(), "µg/m³");
  addCard("PM10", pm10W.peek(), "µg/m³");
  addCard("Particle size", sizeW.peek(), "µm");
  addCard("Ambient Lux", luxW.peek(), "lx");
  addCard("UV Index", uviW.peek(), "");
  addCard("Fan Speed", currentFanRpm, "RPM");
  addCard("Battery", currentBattery, "V");

  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void freeI2CBus() {
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 16 && digitalRead(SDA_PIN) == LOW; i++) {
    digitalWrite(SCL_PIN, LOW);  delayMicroseconds(10);
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(10);
  }
  pinMode(SCL_PIN, INPUT_PULLUP);
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[Boot] ClimateNet node -> AWS IoT (Living Highlands)");
  snprintf(topicData, sizeof topicData, "highlands/%s/data", THING_NAME);

  pinMode(FAN_TACH_PIN, INPUT_PULLUP);
  freeI2CBus();
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(50);

  // Initialize sensors
  rtcAvailable = rtc.begin();
  clockFromRtc();

  bmeAvailable = bme.begin(0x76, &Wire) || bme.begin(0x77, &Wire);

  scd.begin(Wire, 0x62);
  scd.wakeUp();
  delay(30);
  scd.stopPeriodicMeasurement();
  delay(500);
  scd.startPeriodicMeasurement();
  scdAvailable = true;

  sps.begin(Wire, SPS30_I2C_ADDR_69);
  sps.wakeUpSequence();
  sps.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT);
  spsAvailable = true;

  ltrAvailable = ltr.begin();
  if (ltrAvailable) {
    ltr.setResolution(LTR390_RESOLUTION_18BIT);
    ltr.setGain(LTR390_GAIN_1);
    ltr.setMode(LTR390_MODE_ALS);
    ltr.enable(true);
  }

  Serial.printf("[Sensors] RTC %s, BME280 %s, LTR390 %s (SCD41 and SPS30 started)\n",
                rtcAvailable ? "ok" : "MISSING", bmeAvailable ? "ok" : "MISSING", ltrAvailable ? "ok" : "MISSING");

  wifiConnect();
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  // AWS IoT: TLS with the Thing's certificate
  net.setCACert(AWS_ROOT_CA);
  net.setCertificate(DEVICE_CERT);
  net.setPrivateKey(DEVICE_KEY);
  mqtt.setServer(AWS_IOT_ENDPOINT, 8883);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);

  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), onFanPulse, FALLING);

  server.on("/", handleRoot);
  server.begin();
  Serial.println("[HTTP] Local dashboard started");
}

// ---------- loop ----------
unsigned long lastSampleTime = 0;
unsigned long pulseTimer = 0;
unsigned long lastPublish = 0;

void loop() {
  server.handleClient();
  ensureWifi();
  ensureMqtt();
  mqtt.loop();
  saveClockToRtcOnce();

  // Sample sensors every second
  if (millis() - lastSampleTime >= 1000) {
    lastSampleTime = millis();
    readSensors();
  }

  // Fan RPM and battery every 2 seconds
  if (millis() - pulseTimer >= 2000) {
    noInterrupts();
    uint32_t pulses = fanPulses;
    fanPulses = 0;
    interrupts();

    currentFanRpm = (pulses * 60.0f / 2.0f) / 2.0f;  // 2 s window, 2 pulses per revolution
    currentBattery = readBattery();
    pulseTimer = millis();
  }

  // Send to AWS once a second
  if (millis() - lastPublish >= PUBLISH_EVERY_MS) {
    lastPublish = millis();
    publishReadings();
  }
}

