/*
 * IOT102 - FA26 - Group 1
 * Offline-First Smart Home Node  -  FIRMWARE OPA1 (tuan 7)
 * Board: ESP8266 NodeMCU (Tools > Board > "NodeMCU 1.0 (ESP-12E Module)")
 *        Tools > Flash Size > "4MB (FS:1MB OTA:~1019KB)" 
 * Chuc nang OPA1:
 *   - DHT11, MQ2 (gas), LDR (qua PCF8591), PIR (qua PCF8574 0x21)
 *   - Bao cao theo su kien (event-driven) hoac theo chu ky (fixed) -> MQTT
 *   - Quat (relay): Auto theo nhiet do (co hysteresis) / Manual tu dashboard
 *   - Gas > nguong: coi + bat quat + pub alert/gas (uu tien cao nhat)
 *   - Den phong: PIR + toi (LDR) hoac manual
 *   - RFID RC522: quet the -> so voi danh sach trong flash -> servo mo cua + log
 *   - DS1307 RTC (dong bo tu NTP khi co mang), LCD1602 (tu bo qua neu khong gan)
 *   - LWT: home/room1/status = "offline" (retain)
 * OPA2 se them: keypad + PIN, offline queue, reconcile, lich RTC.
 *
 * Topic/JSON theo MQTT_CONTRACT.md. Topic them: state (retain), cmd/report, cmd/config.
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <SPI.h>
#include <Servo.h>
#include <LittleFS.h>
#include <time.h>
#include <DHT.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522.h>
#include <ArduinoJson.h>

// ====================== CAU HINH - SUA CHO DUNG ======================
const char* WIFI_SSID = "XuanQuang";
const char* WIFI_PASS = "15091970";
const char* MQTT_HOST = "192.168.1.8";     // IP laptop chay Mosquitto (ipconfig)
const uint16_t MQTT_PORT = 1883;

// Chan (theo Table III trong report) - dung so GPIO
#define PIN_SDA        4    // D2
#define PIN_SCL        5    // D1
#define PIN_DHT        0    // D3  (boot pin, module DHT co pull-up)
#define PIN_SERVO      2    // D4  (boot pin! neu khong boot duoc: them tro keo len 3.3V 10k)
#define PIN_RELAY      16   // D0
#define PIN_RC522_SS   15   // D8  (boot pin). SCK=D5 MISO=D6 MOSI=D7. RST noi 3V3
const bool RELAY_ACTIVE_LOW = false;       // module nay kich muc CAO (do bang thuc nghiem: off=HIGH=HUT)

// Dia chi I2C (Table IV)
#define ADDR_RTC       0x68
#define ADDR_LCD_A     0x3F
#define ADDR_LCD_B     0x27
#define ADDR_PCF_OUT   0x21
#define ADDR_ADC       0x48

// Bit tren PCF8574 0x21 (Table IV). Tai noi dang "sink": ghi 0 = BAT
#define PCF_PIR        (1 << 0)   // input (luon ghi 1)
#define PCF_BUZZ       (1 << 2)
// #define PCF_BUZZ_DOOR   (1 << 1)   // Còi ngoài cửa ở chân P1
#define PCF_LED_ROOM   (1 << 3)
#define PCF_LED_OK     (1 << 4)
#define PCF_LED_FAIL   (1 << 5)
#define PCF_LED_MODE   (1 << 6)

// Nguong & thoi gian
int   gasThresh        = 600;      // theo MQTT contract
const int GAS_HYST     = 40;       // tat canh bao khi gas < nguong - 40
const unsigned long GAS_WARMUP_MS = 30000;  // MQ2 can lam nong, bo qua canh bao 30 s dau
const int GAS_SCALE    = 2;        // MQ2 AOUT qua cau chia 1k/1k -> nhan 2 de ve thang goc
float tOn              = 30.0;     // quat bat khi T >= tOn (Auto)
float tOff             = 28.0;     // quat tat khi T <= tOff
const bool LDR_HIGH_IS_BRIGHT = true;  // doi thanh false neu LDR noi nguoc chieu
const int  DARK_LEVEL  = 60;       // (du phong) muc sang 0..255
int        lightThresh = 650;      // nguong toi theo lightLux (0..1000), chinh tu web (th.light)
const unsigned long PIR_HOLD_MS    = 30000;
const unsigned long DOOR_OPEN_MS   = 3000;
const int DOOR_OPEN_DEG = 90, DOOR_LOCK_DEG = 0;

// Bao cao theo su kien
const float TH_TEMP = 0.5, TH_HUM = 2.0, TH_GAS = 30, TH_LIGHT = 50;
const unsigned long HEARTBEAT_MS = 30000;   // luon gui it nhat moi 30 s
const unsigned long FIXED_MS     = 5000;    // che do fixed (baseline cho RQ2)
const unsigned long STATE_HB_MS  = 60000;
// =====================================================================

enum Mode { M_AUTO = 0, M_ON = 1, M_OFF = 2 };
enum Report { R_EVENT = 0, R_FIXED = 1 };

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
DHT          dht(PIN_DHT, DHT11);
RTC_DS1307   rtc;
LiquidCrystal_I2C* lcd = nullptr;
MFRC522      rfid(PIN_RC522_SS, MFRC522::UNUSED_PIN);
Servo        doorServo;

bool hasRTC = false, hasLCD = false, hasPCF = false, hasADC = false;

// Trang thai
Mode   fanMode = M_AUTO, lightMode = M_AUTO;
bool   buzzerManual = false;
Report reportMode = R_EVENT;
bool   fanOn = false, lightOn = false, buzzerOut = false, buzzerState = false;
bool   gasAlarm = false, tempFan = false, doorOpen = false, stateDirty = false;
Mode   fanModePrev = M_AUTO;      // de phat hien luc quay lai che do AUTO
float  temp = NAN, hum = NAN;
int    gasVal = 0, lightLux = 0, lightLevel = 255;
bool   pirNow = false, dark = false;
unsigned long pirHoldUntil = 0, beepUntil = 0, okUntil = 0, failUntil = 0;
unsigned long doorCloseAt = 0, servoDetachAt = 0, enrollUntil = 0;
unsigned long msgCount = 0, lastMqttTry = 0;
long   lastDriftSec = 0;
bool   ntpStarted = false;

struct Reporter { float last = NAN; unsigned long lastT = 0; };
Reporter rTemp, rHum, rGas, rLight, rPir;

// ---------------------------------------------------------------- tien ich
inline bool before(unsigned long t) { return (long)(t - millis()) > 0; }  // t con o tuong lai?

bool i2cPresent(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }

void i2cScan() {
  Serial.println(F("I2C scan:"));
  for (uint8_t a = 1; a < 127; a++) if (i2cPresent(a)) Serial.printf("  0x%02X\n", a);
}

bool ntpSynced() { return time(nullptr) > 1700000000UL; }

void isoNow(char* buf, size_t n) {
  if (hasRTC) {
    DateTime d = rtc.now();
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d", d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second());
  } else if (ntpSynced()) {
    time_t t = time(nullptr); struct tm* tm = localtime(&t);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%S", tm);
  } else {
    snprintf(buf, n, "2000-01-01T00:00:00");
  }
}

uint32_t unixNow() {
  if (hasRTC) return rtc.now().unixtime();
  if (ntpSynced()) return (uint32_t)time(nullptr) + 7 * 3600;
  return millis() / 1000;
}

void syncRtcFromNtp() {
  static unsigned long last = 0; static bool first = true;
  if (!hasRTC || !ntpSynced() || WiFi.status() != WL_CONNECTED) return;
  if (!first && millis() - last < 3600000UL) return;          // moi 1 gio
  time_t t = time(nullptr); struct tm* tm = localtime(&t);
  DateTime ntp(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
  lastDriftSec = (long)rtc.now().unixtime() - (long)ntp.unixtime();   // RTC - NTP (RQ1)
  rtc.adjust(ntp);
  first = false; last = millis();
  Serial.printf("[RTC] dong bo NTP, lech truoc do: %ld s\n", lastDriftSec);
}

// ------------------------------------------------------------------ PCF/ADC
uint8_t pcfLast = 0x00;
void pcfWrite(uint8_t v) {
  if (!hasPCF) return;
  v |= PCF_PIR;                         // chan PIR phai o muc 1 de doc duoc
  if (v == pcfLast) return;
  Wire.beginTransmission(ADDR_PCF_OUT); Wire.write(v); Wire.endTransmission();
  pcfLast = v;
}
bool readPIR() {
  if (!hasPCF) return false;
  if (Wire.requestFrom((uint8_t)ADDR_PCF_OUT, (uint8_t)1) != 1) return false;
  return Wire.read() & PCF_PIR;
}
int readLdrRaw() {                       // PCF8591 AIN0, 0..255
  if (!hasADC) return -1;
  Wire.beginTransmission(ADDR_ADC); Wire.write(0x00); Wire.endTransmission();
  if (Wire.requestFrom((uint8_t)ADDR_ADC, (uint8_t)2) != 2) return -1;
  Wire.read();                           // byte dau la ket qua cu, bo
  return Wire.read();
}
int readGas() {
  long s = 0; for (int i = 0; i < 4; i++) s += analogRead(A0);
  return (int)(s / 4) * GAS_SCALE;
}

void applyOutputs() {
  uint8_t v = 0xFF;
  if (buzzerOut)           v &= ~PCF_BUZZ;
  // if (before(beepUntil))   v &= ~PCF_BUZZ_DOOR; 
  if (lightOn)             v &= ~PCF_LED_ROOM;
  if (before(okUntil))     v &= ~PCF_LED_OK;
  if (before(failUntil))   v &= ~PCF_LED_FAIL;
  if (mqtt.connected())    v &= ~PCF_LED_MODE;
  pcfWrite(v);
  digitalWrite(PIN_RELAY, (fanOn != RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

void beep(unsigned long ms) { beepUntil = millis() + ms; }

// -------------------------------------------------------------------- MQTT
bool pub(const char* sub, const char* payload, bool retain = false) {
  if (!mqtt.connected()) return false;
  char topic[48]; snprintf(topic, sizeof topic, "home/room1/%s", sub);
  bool ok = mqtt.publish(topic, payload, retain);
  if (ok) msgCount++;
  return ok;
}

const char* modeStr(Mode m) { return m == M_ON ? "on" : (m == M_OFF ? "off" : "auto"); }

void publishState(bool force = false) {
  static uint32_t lastSig = 0xFFFFFFFF; static unsigned long lastT = 0;
  buzzerState = gasAlarm || buzzerManual;
  uint32_t sig = (uint32_t)fanOn | ((uint32_t)lightOn << 1) | ((uint32_t)buzzerState << 2) |
                 ((uint32_t)doorOpen << 3) | ((uint32_t)gasAlarm << 4) | ((uint32_t)fanMode << 5) |
                 ((uint32_t)lightMode << 7) | ((uint32_t)reportMode << 9);
  if (!force && sig == lastSig && millis() - lastT < STATE_HB_MS) return;
  if (!mqtt.connected()) return;
  char ts[24]; isoNow(ts, sizeof ts);
  char p[320];
  snprintf(p, sizeof p,
    "{\"fan\":\"%s\",\"fanMode\":\"%s\",\"light\":\"%s\",\"lightMode\":\"%s\",\"buzzer\":\"%s\","
    "\"door\":\"%s\",\"gasAlarm\":%s,\"report\":\"%s\",\"msgs\":%lu,\"rssi\":%d,\"driftSec\":%ld,\"ts\":\"%s\"}",
    fanOn ? "on" : "off", modeStr(fanMode), lightOn ? "on" : "off", modeStr(lightMode),
    buzzerState ? "on" : "off", doorOpen ? "open" : "locked", gasAlarm ? "true" : "false",
    reportMode == R_FIXED ? "fixed" : "event", msgCount, WiFi.RSSI(), lastDriftSec, ts);
  pub("state", p, true);
  lastSig = sig; lastT = millis();
}

String parseState(const String& s) {
  String r = s; r.trim();
  if (r.startsWith("{")) {
    JsonDocument d;
    if (deserializeJson(d, s)) return "";
    r = String((const char*)(d["state"] | ""));
  }
  r.replace("\"", ""); r.toLowerCase(); r.trim();
  return r;
}

Mode toMode(const String& s) { return s == "on" ? M_ON : (s == "off" ? M_OFF : M_AUTO); }

// Ap dung ban tin config tu dashboard (retained). Ho tro 2 dang:
//   {"ton":30,"toff":28,"gas":600}                      (dang phang, cu)
//   {"mode":"auto","th":{"temp":30,"gas":600},"ts":...} (dang Node-RED gui)
void applyConfig(const String& body) {
  JsonDocument d;
  if (deserializeJson(d, body)) { Serial.println(F("[CFG] JSON loi")); return; }

  float ton = NAN, toff = NAN;
  int   gas = -1, light = -1;
  if (!d["ton"].isNull())  ton  = d["ton"].as<float>();
  if (!d["toff"].isNull()) toff = d["toff"].as<float>();
  if (!d["gas"].isNull())  gas  = d["gas"].as<int>();
  if (!d["light"].isNull()) light = d["light"].as<int>();
  if (d["th"].is<JsonObject>()) {
    JsonObject th = d["th"].as<JsonObject>();
    if (!th["temp"].isNull()) ton = th["temp"].as<float>();
    if (!th["gas"].isNull())  gas = th["gas"].as<int>();
    if (!th["light"].isNull()) light = th["light"].as<int>();
  }
  if (!isnan(ton))  { tOn = ton; tOff = (!isnan(toff)) ? toff : (tOn - 2.0); }
  else if (!isnan(toff)) tOff = toff;
  if (gas >= 0) gasThresh = gas;
  if (light >= 50 && light <= 1000) lightThresh = light;   // 0..1000 la thang tuong doi

  const char* md = d["mode"] | "";
  if (strcmp(md, "auto") == 0) {
    fanMode = M_AUTO; lightMode = M_AUTO;
    if (!isnan(temp)) {
      if (temp >= tOn)       tempFan = true;
      else if (temp <= tOff) tempFan = false;
      else                   tempFan = fanOn;   // trong dai tre: giu nguyen
    }
  } else if (strcmp(md, "manual") == 0) {
    fanMode   = fanOn   ? M_ON : M_OFF;   // giu nguyen, cho lenh tu dashboard
    lightMode = lightOn ? M_ON : M_OFF;
  }
  Serial.printf("[CFG] tOn=%.1f tOff=%.1f gas=%d light=%d mode=%s fanMode=%s\n",
                tOn, tOff, gasThresh, lightThresh, md, modeStr(fanMode));
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  String body; body.reserve(len);
  for (unsigned int i = 0; i < len; i++) body += (char)payload[i];
  String sub = String(topic).substring(strlen("home/room1/"));
  Serial.printf("[MQTT] %s <- %s\n", topic, body.c_str());

  if (sub == "cmd/config" || sub == "config") {
    applyConfig(body);
  } else {
    String st = parseState(body);
    if      (sub == "cmd/fan")    fanMode = toMode(st);
    else if (sub == "cmd/light")  lightMode = toMode(st);
    else if (sub == "cmd/buzzer") buzzerManual = (st == "on");
    else if (sub == "cmd/door") { if (st == "open" || st == "on") { doorUnlock(); accessLog("dashboard", true); } }
    else if (sub == "cmd/report") {
      reportMode = (st == "fixed") ? R_FIXED : R_EVENT;
      rTemp = rHum = rGas = rLight = rPir = Reporter();     // ep gui lai ngay
    }
  }
  pub("status", "online", true);          // contract muc 3, quy tac 4: xac nhan con song
  // KHONG publishState o day: controlLogic() chua chay nen gia tri relay/LED/cong
  // van la cu. publishState() cuoi controlLogic() se gui state DUNG (va chi 1 lan).
  stateDirty = true;
}

void mqttService() {
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttTry < 5000) return;
  lastMqttTry = millis();
  String cid = "esp8266-room1-" + String(ESP.getChipId(), HEX);
  if (mqtt.connect(cid.c_str(), nullptr, nullptr, "home/room1/status", 1, true, "offline")) {
    mqtt.subscribe("home/room1/cmd/#", 1);
    mqtt.subscribe("home/room1/config", 1);   // mode + nguong (retained tu Node-RED)
    mqtt.publish("home/room1/status", "online", true);
    publishState(true);
    Serial.println(F("[MQTT] connected"));
  } else {
    Serial.printf("[MQTT] fail rc=%d\n", mqtt.state());
  }
}

// ----------------------------------------------------------------- bao cao
bool shouldReport(Reporter& r, float v, float theta) {
  unsigned long now = millis();
  if (isnan(r.last)) return true;
  if (reportMode == R_FIXED) return now - r.lastT >= FIXED_MS;
  return fabsf(v - r.last) >= theta || now - r.lastT >= HEARTBEAT_MS;
}
void mark(Reporter& r, float v) { r.last = v; r.lastT = millis(); }

void reportDht() {
  if (isnan(temp) || isnan(hum)) return;
  bool a = shouldReport(rTemp, temp, TH_TEMP), b = shouldReport(rHum, hum, TH_HUM);
  if (!(a || b)) return;
  char ts[24], p[96]; isoNow(ts, sizeof ts);
  snprintf(p, sizeof p, "{\"temp\":%.1f,\"hum\":%.1f,\"ts\":\"%s\"}", temp, hum, ts);
  if (pub("sensor/dht", p)) { mark(rTemp, temp); mark(rHum, hum); }
}
void reportGas(bool force) {
  if (!force && !shouldReport(rGas, gasVal, TH_GAS)) return;
  char ts[24], p[64]; isoNow(ts, sizeof ts);
  snprintf(p, sizeof p, "{\"value\":%d,\"ts\":\"%s\"}", gasVal, ts);
  if (pub("sensor/gas", p)) mark(rGas, gasVal);
}
void reportLight() {
  if (!hasADC || !shouldReport(rLight, lightLux, TH_LIGHT)) return;
  char ts[24], p[64]; isoNow(ts, sizeof ts);
  snprintf(p, sizeof p, "{\"value\":%d,\"ts\":\"%s\"}", lightLux, ts);
  if (pub("sensor/light", p)) mark(rLight, lightLux);
}
void reportPir() {
  if (!shouldReport(rPir, pirNow ? 1 : 0, 0.5)) return;
  char ts[24], p[64]; isoNow(ts, sizeof ts);
  snprintf(p, sizeof p, "{\"value\":%s,\"ts\":\"%s\"}", pirNow ? "true" : "false", ts);
  if (pub("sensor/pir", p)) mark(rPir, pirNow ? 1 : 0);
}

// --------------------------------------------------------------- cam bien
void readSensors() {
  static unsigned long tPir = 0, tGas = 0, tLdr = 0, tDht = 0;
  unsigned long now = millis();

  if (now - tPir >= 100) {
    tPir = now;
    bool p = readPIR();
    if (p) pirHoldUntil = now + PIR_HOLD_MS;
    if (p != pirNow) { pirNow = p; rPir.last = NAN; }   // doi trang thai -> gui ngay
    reportPir();
  }

  if (now - tGas >= 500) {
    tGas = now;
    gasVal = readGas();
    bool justAlarm = false;
    if (!gasAlarm && gasVal > gasThresh && millis() > GAS_WARMUP_MS) {
      gasAlarm = true; justAlarm = true;
      char ts[24], p[96]; isoNow(ts, sizeof ts);
      snprintf(p, sizeof p, "{\"value\":%d,\"threshold\":%d,\"ts\":\"%s\"}", gasVal, gasThresh, ts);
      pub("alert/gas", p);                  // ts bat buoc (contract)
      Serial.printf("[GAS] ALARM %d\n", gasVal);
    } else if (gasAlarm && gasVal < gasThresh - GAS_HYST) {
      gasAlarm = false;
      Serial.println(F("[GAS] het canh bao"));
    }
    reportGas(justAlarm);                   // vuot nguong thi bo qua bo loc
  }

  if (now - tLdr >= 1000) {
    tLdr = now;
    int raw = readLdrRaw();
    if (raw >= 0) {
      lightLevel = LDR_HIGH_IS_BRIGHT ? raw : 255 - raw;
      lightLux = lightLevel * 1000 / 255;   // gia tri tuong doi 0..1000
      dark = (lightLux < lightThresh);
      reportLight();
    }
  }

  if (now - tDht >= 2000) {                 // DHT11 toi thieu 1 s/lan
    tDht = now;
    float t = dht.readTemperature(), h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) { temp = t; hum = h; reportDht(); }
  }
}

// ---------------------------------------------------------- logic dieu khien
void controlLogic() {
  // Khi quay lai AUTO: danh gia lai ngay, khong giu trang thai treo cua che do thu cong
  if (fanMode == M_AUTO && fanModePrev != M_AUTO) {
    if (!isnan(temp)) {
      if (temp >= tOn)       tempFan = true;
      else if (temp <= tOff) tempFan = false;
      else                   tempFan = fanOn;   // trong dai tre (tOff..tOn): giu nguyen
    }
    Serial.printf("[MODE] quat -> auto, T=%.1f, bat=%d\n", temp, tempFan);
  }
  fanModePrev = fanMode;

  // Quat - uu tien: Gas > Manual > Nhiet do (> Lich RTC o OPA2)
  bool wantFan;
  if (gasAlarm) wantFan = true;
  else if (fanMode == M_ON) wantFan = true;
  else if (fanMode == M_OFF) wantFan = false;
  else {
    if (!isnan(temp)) { if (temp >= tOn) tempFan = true; else if (temp <= tOff) tempFan = false; }
    wantFan = tempFan;
  }
  fanOn = wantFan;

  // Den phong
  if (lightMode == M_ON) lightOn = true;
  else if (lightMode == M_OFF) lightOn = false;
  else lightOn = before(pirHoldUntil) && dark;

  buzzerOut = gasAlarm || buzzerManual || before(beepUntil);
  applyOutputs();
  publishState(stateDirty);
  stateDirty = false;
}

// ------------------------------------------------------------- RFID + cua
bool cardKnown(const String& uid) {
  File f = LittleFS.open("/cards.txt", "r");
  if (!f) return false;
  bool found = false;
  while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l == uid) { found = true; break; } }
  f.close();
  return found;
}
void cardAdd(const String& uid) {
  if (cardKnown(uid)) return;
  File f = LittleFS.open("/cards.txt", "a"); if (!f) return;
  f.println(uid); f.close();
}
void cardsPrint() {
  File f = LittleFS.open("/cards.txt", "r");
  if (!f) { Serial.println(F("(chua co the nao)")); return; }
  while (f.available()) Serial.println(f.readStringUntil('\n'));
  f.close();
}

void doorUnlock() {
  doorServo.attach(PIN_SERVO, 500, 2400);   // chi attach khi can de tranh nhieu luc boot
  doorServo.write(DOOR_OPEN_DEG);
  doorOpen = true; doorCloseAt = millis() + DOOR_OPEN_MS; servoDetachAt = 0;
  publishState(true);
}
void doorService() {
  if (doorOpen && !before(doorCloseAt)) {
    doorServo.attach(PIN_SERVO, 500, 2400);
    doorServo.write(DOOR_LOCK_DEG);
    doorOpen = false; servoDetachAt = millis() + 600;
    publishState(true);
  }
  if (servoDetachAt && !before(servoDetachAt)) { doorServo.detach(); servoDetachAt = 0; }
}

void accessLog(const String& uid, bool granted) {
  char ts[24]; isoNow(ts, sizeof ts);
  char p[160];
  snprintf(p, sizeof p, "{\"id\":\"card-%s-%lu\",\"method\":\"RFID\",\"result\":\"%s\",\"ts\":\"%s\"}",
           uid.c_str(), (unsigned long)unixNow(), granted ? "granted" : "denied", ts);
  pub("access/log", p);                     // OPA2: neu offline thi luu queue
}

void handleRfid() {
  static unsigned long last = 0, lastCardT = 0; static String lastUid;
  if (millis() - last < 100) return; last = millis();
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid;
  for (byte i = 0; i < rfid.uid.size; i++) { if (rfid.uid.uidByte[i] < 0x10) uid += "0"; uid += String(rfid.uid.uidByte[i], HEX); }
  uid.toUpperCase();
  rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();

  if (uid == lastUid && millis() - lastCardT < 2000) return;   // chong doi
  lastUid = uid; lastCardT = millis();

  if (enrollUntil && before(enrollUntil)) {                     // che do them the
    cardAdd(uid); enrollUntil = 0; okUntil = millis() + 800; beep(150);
    Serial.printf("[RFID] da them the %s\n", uid.c_str());
    return;
  }
  bool granted = cardKnown(uid);
  Serial.printf("[RFID] %s -> %s\n", uid.c_str(), granted ? "GRANTED" : "DENIED");
  if (granted) { okUntil = millis() + 2000; beep(120); doorUnlock(); }
  else         { failUntil = millis() + 2000; beep(500); }
  accessLog(uid, granted);
}

// ------------------------------------------------------------ LCD + serial
void lcdUpdate() {
  static unsigned long last = 0;
  if (!hasLCD || millis() - last < 1000) return; last = millis();
  char ts[24]; isoNow(ts, sizeof ts);
  char l1[24], l2[24], t2[24], tt[8];
  if (isnan(temp)) strcpy(tt, "--"); else snprintf(tt, sizeof tt, "%d", (int)temp);
  snprintf(t2, sizeof t2, "%sC %d%% G%d %c", tt, isnan(hum) ? 0 : (int)hum, gasVal, fanOn ? 'F' : '-');
  snprintf(l1, 17, "%-8s %-7s", ts + 11, mqtt.connected() ? "ONLINE" : "OFFLINE");
  snprintf(l2, 17, "%-16s", t2);
  lcd->setCursor(0, 0); lcd->print(l1);
  lcd->setCursor(0, 1); lcd->print(l2);
}

void printStatus() {
  char ts[24]; isoNow(ts, sizeof ts);
  Serial.printf("ts=%s wifi=%d mqtt=%d T=%.1f H=%.1f gas=%d lux=%d pir=%d fan=%d light=%d gasAlarm=%d msgs=%lu\n",
    ts, WiFi.status() == WL_CONNECTED, mqtt.connected(), temp, hum, gasVal, lightLux, pirNow, fanOn, lightOn, gasAlarm, msgCount);
}

void handleSerial() {
  if (!Serial.available()) return;
  String c = Serial.readStringUntil('\n'); c.trim();
  if      (c == "scan")       i2cScan();
  else if (c == "status")     printStatus();
  else if (c == "cards")      cardsPrint();
  else if (c == "clearcards") { LittleFS.remove("/cards.txt"); Serial.println(F("da xoa danh sach the")); }
  else if (c == "enroll")     { enrollUntil = millis() + 15000; Serial.println(F("Quet the can them trong 15 s...")); }
  else if (c.startsWith("settime ")) {                         // settime 2026-10-19 10:30:00
    int y, mo, d, h, mi, s;
    if (hasRTC && sscanf(c.c_str() + 8, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
      rtc.adjust(DateTime(y, mo, d, h, mi, s)); Serial.println(F("da chinh RTC"));
    } else Serial.println(F("sai dinh dang hoac khong co RTC"));
  } else Serial.println(F("lenh: scan | status | cards | clearcards | enroll | settime YYYY-MM-DD HH:MM:SS"));
}

// ------------------------------------------------------------------- setup
void setup() {
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);     // quat TAT ngay tu dau
  Serial.begin(115200); delay(200);
  Serial.println(F("\n== IOT102 G1 - OPA1 node =="));
  Serial.printf("BUILD relay: RELAY_ACTIVE_LOW=%d  (D0=%s luc quat TAT)\n",
                RELAY_ACTIVE_LOW, RELAY_ACTIVE_LOW ? "HIGH" : "LOW");

  Wire.begin(PIN_SDA, PIN_SCL); Wire.setClock(100000);        // DS1307 toi da 100 kHz
  hasRTC = i2cPresent(ADDR_RTC) && rtc.begin();
  hasPCF = i2cPresent(ADDR_PCF_OUT);
  hasADC = i2cPresent(ADDR_ADC);
  uint8_t lcdAddr = i2cPresent(ADDR_LCD_A) ? ADDR_LCD_A : (i2cPresent(ADDR_LCD_B) ? ADDR_LCD_B : 0);
  Serial.printf("RTC=%d PCF8574=%d PCF8591=%d LCD=0x%02X\n", hasRTC, hasPCF, hasADC, lcdAddr);
  if (!hasPCF) Serial.println(F("CANH BAO: khong thay PCF8574 0x21 -> khong co PIR/coi/LED"));

  if (hasRTC && !rtc.isrunning()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  if (lcdAddr) {
    lcd = new LiquidCrystal_I2C(lcdAddr, 16, 2);
    lcd->init(); lcd->backlight(); lcd->print(F("IOT102 G1 OPA1")); hasLCD = true;
  }
  pcfWrite(0xFF);                                             // tat het LED/coi

  dht.begin();
  SPI.begin(); rfid.PCD_Init();
  LittleFS.begin();

  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiClient.setTimeout(1000);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(2);
  Serial.println(F("San sang. Go 'enroll' roi quet the de them the hop le."));
}

void loop() {
  if (WiFi.status() == WL_CONNECTED && !ntpStarted) {
    configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com"); ntpStarted = true;
    Serial.print(F("WiFi OK, IP ")); Serial.println(WiFi.localIP());
  }
  syncRtcFromNtp();
  mqttService();
  readSensors();
  controlLogic();
  handleRfid();
  doorService();
  lcdUpdate();
  handleSerial();
  yield();
}
