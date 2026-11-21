#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "pins.h"
#include "secrets.h"
#include "config.h"

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOk = false;

static const float PULSES_PER_LITER = 450.0f;

volatile uint32_t pulses = 0;
uint32_t totalPulses = 0;
uint32_t savedPulses = 0;
uint32_t unsentPulses = 0;
Preferences store;

WiFiClient net;
PubSubClient mqtt(net);
const bool netEnabled = strlen(WIFI_SSID) > 0 && strlen(TB_TOKEN) > 0;
float lastLpm = 0;
uint32_t flowSeconds = 0;
uint32_t idleSeconds = 0;
uint32_t nextAlertAt = ALERT_STEP_SECONDS;
bool testAlertPending = false;

volatile uint32_t lastPulseUs = 0;

void IRAM_ATTR onPulse() {
  uint32_t now = micros();
  if (now - lastPulseUs < 2000) return;
  lastPulseUs = now;
  pulses++;
}

void out(const String &s) {
  Serial.println(s);
  Serial0.println(s);
}

void ledWrite(int pin, bool on) {
  if (pin < 0) return;
  digitalWrite(pin, (on ^ LED_COMMON_ANODE) ? HIGH : LOW);
}

void setLed(bool r, bool g, bool b) {
  ledWrite(PIN_LED_R, r);
  ledWrite(PIN_LED_G, g);
  ledWrite(PIN_LED_B, b);
}

int sendAlert(uint32_t minutes, float lpm, float liters, bool test) {
  if (WiFi.status() != WL_CONNECTED) return 0;
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  if (!http.begin(tls, ALERT_URL)) return 0;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", WATER_DEVICE_KEY);
  String json = "{\"room\":\"" + String(DEVICE_ROOM) + "\",\"minutes\":" + String(minutes) +
                ",\"flow_lpm\":" + String(lpm, 2) + ",\"total_l\":" + String(liters, 1) +
                (test ? ",\"test\":true}" : "}");
  int code = http.POST(json);
  String resp = code > 0 ? http.getString() : "";
  http.end();
  out("แจ้งเตือนน้ำ " + String(minutes) + " นาที: HTTP " + String(code) + (resp.indexOf("\"skipped\":true") >= 0 ? " (ยังไม่ถึงเวลาที่ตั้งไว้)" : ""));
  return code;
}

void resetTotal() {
  totalPulses = savedPulses = 0;
  store.putUInt("pulses", 0);
  out("ล้างปริมาณรวมแล้ว");
}

void onRpc(char *topic, byte *payload, unsigned int len) {
  String body;
  for (unsigned int i = 0; i < len; i++) body += (char)payload[i];
  String t(topic);
  String id = t.substring(t.lastIndexOf('/') + 1);
  if (body.indexOf("\"testAlert\"") >= 0) {
    testAlertPending = true;
    mqtt.publish(("v1/devices/me/rpc/response/" + id).c_str(), "{\"ok\":true}");
    return;
  }
  if (body.indexOf("\"resetTotal\"") >= 0) {
    resetTotal();
    mqtt.publish(("v1/devices/me/rpc/response/" + id).c_str(), "{\"ok\":true}");
  }
}

void netLoop() {
  if (!netEnabled) return;
  static uint32_t lastTry = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastTry > 10000) {
      lastTry = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      out("Wi-Fi: กำลังเชื่อมต่อ " + String(WIFI_SSID));
    }
    return;
  }
  if (!mqtt.connected()) {
    if (millis() - lastTry > 5000) {
      lastTry = millis();
      if (mqtt.connect("waterflow-board", TB_TOKEN, nullptr)) {
        mqtt.subscribe("v1/devices/me/rpc/request/+");
        out("ThingsBoard: เชื่อมต่อแล้ว (" + WiFi.localIP().toString() + ")");
      } else {
        out("ThingsBoard: เชื่อมต่อไม่ได้ รหัส " + String(mqtt.state()));
      }
    }
    return;
  }
  mqtt.loop();
}

void sendTelemetry(float lpm, float liters, bool flowing) {
  if (!mqtt.connected()) return;
  uint32_t used = unsentPulses;
  String json = "{\"flow_lpm\":" + String(lpm, 2) + ",\"total_l\":" + String(liters, 3) +
                ",\"flowing\":" + String(flowing ? "true" : "false") + ",\"rssi\":" + String(WiFi.RSSI()) + ",\"flow_seconds\":" + String(flowSeconds) + ",\"leak\":" + String(flowSeconds >= LEAK_SECONDS ? "true" : "false") +
                (used ? ",\"used_l\":" + String(used / PULSES_PER_LITER, 4) : "") + "}";
  if (mqtt.publish("v1/devices/me/telemetry", json.c_str())) unsentPulses -= used;
}

void scanI2C() {
  if (PIN_SDA < 0 || PIN_SCL < 0) {
    out("I2C: ยังไม่ได้กำหนดขา SDA/SCL ข้ามการค้นหาจอ");
    return;
  }
  Wire.begin(PIN_SDA, PIN_SCL);
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      out("I2C: เจออุปกรณ์ที่ 0x" + String(a, HEX) + (a == 0x3C || a == 0x3D ? " (น่าจะเป็นจอ OLED)" : ""));
      found++;
    }
  }
  if (!found) out("I2C: ไม่เจออุปกรณ์ ตรวจรอยบัดกรีจอ OLED และขา SDA/SCL");
}

void setup() {
  Serial.begin(115200);
  Serial0.begin(115200);
  delay(1500);
  out("\n=== บอร์ดวัดน้ำ: ทดสอบขั้นที่ 1 ===");
  out("ชิป: " + String(ESP.getChipModel()) + " rev " + String(ESP.getChipRevision()));
  out("Flash: " + String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB, PSRAM: " + String(ESP.getPsramSize() / (1024 * 1024)) + " MB");

  for (int p : {PIN_LED_R, PIN_LED_G, PIN_LED_B}) if (p >= 0) pinMode(p, OUTPUT);
  setLed(0, 0, 0);

  store.begin("water", false);
  totalPulses = savedPulses = store.getUInt("pulses", 0);
  out("ปริมาณรวมที่บันทึกไว้: " + String(totalPulses / PULSES_PER_LITER, 3) + " L");

  scanI2C();
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C) || oled.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  out(oledOk ? "จอ OLED: เริ่มทำงาน" : "จอ OLED: เริ่มไม่ได้ ตรวจรอยบัดกรี U7");
  if (oledOk) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("Water Flow Monitor");
    oled.println("starting...");
    oled.display();
  }

  if (netEnabled) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    mqtt.setServer(TB_HOST, TB_PORT);
    mqtt.setCallback(onRpc);
    out("Wi-Fi: กำลังเชื่อมต่อ " + String(WIFI_SSID));
  } else {
    out("Wi-Fi: ยังไม่ได้กรอก WIFI_SSID หรือ TB_TOKEN ใน include/secrets.h ทำงานแบบออฟไลน์");
  }

  pinMode(0, INPUT_PULLUP);
  pinMode(PIN_FLOW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), onPulse, FALLING);
  out("เซนเซอร์น้ำ: รอสัญญาณที่ GPIO" + String(PIN_FLOW) + " ลองเป่าหรือหมุนใบพัดเซนเซอร์ดู");
}

void loop() {
  netLoop();

  static uint32_t pressedAt = 0;
  if (digitalRead(0) == LOW) {
    if (!pressedAt) pressedAt = millis();
    if (millis() - pressedAt > 3000) {
      resetTotal();
      pressedAt = 0;
      while (digitalRead(0) == LOW) delay(10);
    }
  } else {
    pressedAt = 0;
  }

  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();

  noInterrupts();
  uint32_t n = pulses;
  pulses = 0;
  interrupts();
  totalPulses += n;
  unsentPulses += n;

  static uint8_t tick = 0;
  if (++tick >= 10) {
    tick = 0;
    if (totalPulses != savedPulses) {
      store.putUInt("pulses", totalPulses);
      savedPulses = totalPulses;
    }
  }

  float lpm = (n / 7.5f);
  float liters = totalPulses / PULSES_PER_LITER;
  String line = "พัลส์/วินาที=" + String(n) + "  อัตราไหล=" + String(lpm, 2) + " L/min  รวม=" + String(liters, 3) + " L";
  if (netEnabled) line += "  wifi=" + String(WiFi.status()) + " mqtt=" + String(mqtt.state());
  out(line);

  if (n > 0) {
    flowSeconds += 1 + idleSeconds;
    idleSeconds = 0;
  } else if (flowSeconds > 0 && ++idleSeconds > FLOW_GAP_SECONDS) {
    flowSeconds = 0;
    idleSeconds = 0;
    nextAlertAt = ALERT_STEP_SECONDS;
  }
  bool leak = flowSeconds >= LEAK_SECONDS;
  if (flowSeconds >= nextAlertAt && sendAlert(nextAlertAt / 60, lpm, liters, false) > 0) nextAlertAt += ALERT_STEP_SECONDS;
  if (testAlertPending) {
    testAlertPending = false;
    sendAlert(flowSeconds / 60, lpm, liters, true);
  }

  setLed(leak, n > 0 && !leak, n == 0);

  static uint8_t sendTick = 0;
  if (++sendTick >= 5 || (n > 0) != (lastLpm > 0)) {
    sendTick = 0;
    sendTelemetry(lpm, liters, n > 0);
  }
  lastLpm = lpm;

  if (oledOk) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("Water Flow Monitor");
    oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);
    oled.setCursor(0, 16);
    oled.println("Flow (L/min)");
    oled.setTextSize(2);
    oled.setCursor(0, 26);
    oled.println(String(lpm, 2));
    oled.setTextSize(1);
    oled.setCursor(0, 48);
    oled.print("Total: ");
    oled.print(String(liters, 3));
    oled.println(" L");
    oled.setCursor(0, 57);
    oled.print(leak ? "LEAK?" : n > 0 ? "FLOWING" : "idle");
    if (netEnabled) {
      oled.setCursor(80, 57);
      oled.print(mqtt.connected() ? "TB ok" : WiFi.status() == WL_CONNECTED ? "WiFi" : "no WiFi");
    }
    oled.display();
  }
}
