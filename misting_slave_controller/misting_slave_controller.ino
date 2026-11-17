#include <DHT.h>
#include <Preferences.h>
#include "config.h"
#include "misting_comm.h"

DHT dht(PIN_DHT, DHT22);
Preferences prefs;

struct SensorData {
  float suhu = NAN;
  float hum  = NAN;
  float uvIndex = NAN;
  bool  dhtValid = false;
} data;

enum Mode : uint8_t { MODE_AUTO, MODE_MANUAL };
Mode     mode = MODE_AUTO;

uint8_t  dhtFail = 0;
uint32_t lastRead = 0;
bool     relayOn = false;
uint32_t relayOnSince = 0;
float    humMin = DEFAULT_HUM_MIN;
float    humMax = DEFAULT_HUM_MAX;
uint32_t lastOffAt = 0 - AUTO_REST_MS;  // boot: tidak perlu menunggu jeda

// perintah dari master
uint8_t  instantID   = 0;       // cmdID timed_instant yang sedang berjalan (0 = tidak ada)
bool     holdOn      = false;   // tahan aktivasi otomatis (deactivation), hanya RAM
uint32_t holdUntilMs = 0;

bool holdActive() { return holdOn && (int32_t)(millis() - holdUntilMs) < 0; }

// akhir run timed_instant. completed = berjalan penuh ON_LIMIT
void endInstant(bool completed) {
  if (instantID != 0) {
    commSendStat(instantID, CMD_TIMED_INSTANT, completed ? CMD_ST_DONE : CMD_ST_FAILED,
                 completed ? nullptr : "interrupted");
    instantID = 0;
  }
}

void rejectCmd(const MistingCmd &c, const char *why) {
  Serial.printf("[CMD] #%u type %u ditolak: %s\n", c.id, c.type, why);
  commSendStat(c.id, c.type, CMD_ST_FAILED, "rejected");
}

void handleCommand(const MistingCmd &c) {
  JsonDocument d;
  deserializeJson(d, c.buf);

  switch (c.type) {
    case CMD_TIMED_INSTANT:
      if (strcmp(d["actuator"] | "", "pump") != 0) { rejectCmd(c, "actuator salah"); break; }
      if (mode == MODE_MANUAL) { rejectCmd(c, "run manual sedang berjalan"); break; }
      mode = MODE_MANUAL;                // juga jika pompa sedang ON karena AUTO: diambil alih
      relaySet(true);
      relayOnSince = millis();           // reset timer jika sebelumnya ON dari AUTO
      instantID = c.id;
      Serial.printf("[CMD] #%u timed_instant: ON %lu detik\n", c.id, ON_LIMIT_MIST_MS / 1000UL);
      break;

    case CMD_DEACTIVATION:
      holdOn = true;
      holdUntilMs = millis() + OFF_TIME_MIST_MS;
      if (mode == MODE_MANUAL) endInstant(false);
      relaySet(false);
      mode = MODE_AUTO;
      Serial.printf("[CMD] #%u deactivation: pompa OFF, aktivasi otomatis ditahan %lu detik\n",
                    c.id, OFF_TIME_MIST_MS / 1000UL);
      commSendStat(c.id, c.type, CMD_ST_DONE, nullptr);   // done = diterapkan sekarang
      break;

    default:   // instant_change hanya untuk irigasi
      rejectCmd(c, "tipe tidak didukung");
      break;
  }
}

// ---------- Relay ----------
void relaySet(bool on) {
  if (relayOn && !on) lastOffAt = millis();     // catat awal jeda
  if (!relayOn && on) relayOnSince = millis();
  relayOn = on;
  digitalWrite(PIN_RELAY, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

// ---------- Sensor ----------
float readUvIndex() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogReadMilliVolts(PIN_UV);
    delayMicroseconds(200);
  }
  float mv = sum / 16.0f;
  return mv / 100.0f; // GUVA-S12SD: 100 mV per 1 UV index
}

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  bool ok = !isnan(t) && !isnan(h) && h >= 0 && h <= 100 && t > -40 && t < 80;

  if (ok) {
    dhtFail = 0;
    data.suhu = data.dhtValid ? EMA_ALPHA * t + (1 - EMA_ALPHA) * data.suhu : t;
    data.hum  = data.dhtValid ? EMA_ALPHA * h + (1 - EMA_ALPHA) * data.hum  : h;
    data.dhtValid = true;
  } else {
    if (dhtFail < 255) dhtFail++;
    if (dhtFail >= SENSOR_FAIL_LIMIT) {
      data.dhtValid = false;
      data.suhu = NAN;
      data.hum  = NAN;
    }
  }
  data.uvIndex = readUvIndex();
}

// ---------- Konfigurasi (NVS) ----------
bool thresholdValid(float lo, float hi) {
  return lo >= 0 && hi <= 100 && (hi - lo) >= HUM_MIN_GAP;
}

void loadConfig() {
  prefs.begin("misting", false);
  float lo = prefs.getFloat("hmin", DEFAULT_HUM_MIN);
  float hi = prefs.getFloat("hmax", DEFAULT_HUM_MAX);
  if (!thresholdValid(lo, hi)) { lo = DEFAULT_HUM_MIN; hi = DEFAULT_HUM_MAX; }
  humMin = lo; humMax = hi;
}

bool setThresholds(float lo, float hi) {
  if (!thresholdValid(lo, hi)) return false;
  humMin = lo; humMax = hi;
  prefs.putFloat("hmin", lo);
  prefs.putFloat("hmax", hi);
  return true;
}

// ---------- Kontrol pompa ----------
void controlPump() {
  uint32_t now = millis();

  if (holdOn && !holdActive()) { holdOn = false; Serial.println("[HOLD] selesai, AUTO aktif kembali"); }

  if (mode == MODE_MANUAL) {
    if (relayOn && now - relayOnSince >= ON_LIMIT_MIST_MS) {
      relaySet(false);
      mode = MODE_AUTO;
      endInstant(true);
      Serial.println("[MANUAL] batas durasi tercapai, kembali AUTO");
    }
    return;
  }

  // MODE_AUTO
  if (holdActive()) {
    if (relayOn) { relaySet(false); Serial.println("[AUTO] ditahan, pompa OFF"); }
    return;
  }
  if (!data.dhtValid) {
    if (relayOn) { relaySet(false); Serial.println("[AUTO] DHT error, pompa OFF"); }
    return;
  }
  if (relayOn) {
    if (data.hum >= humMax) {
      relaySet(false); Serial.println("[AUTO] RH mencapai max, OFF");
    } else if (now - relayOnSince >= AUTO_MAX_ON_MS) {
      relaySet(false); Serial.println("[AUTO] batas durasi, OFF");
    }
  } else if (data.hum < humMin && now - lastOffAt >= AUTO_REST_MS) {
    relaySet(true); Serial.println("[AUTO] RH di bawah min, ON");
  }
}

// // ---------- Perintah Serial (sementara, diganti ESP-NOW di Tahap 3) ----------
// void handleSerial() {
//   if (!Serial.available()) return;
//   String s = Serial.readStringUntil('\n');
//   s.trim();
//   if (s == "on") {
//     mode = MODE_MANUAL;
//     relaySet(true);
//     relayOnSince = millis();          // reset timer jika sebelumnya ON dari AUTO
//     Serial.println("[MANUAL] ON");
//   } else if (s == "off") {
//     relaySet(false);
//     mode = MODE_AUTO;
//     Serial.println("[MANUAL] OFF, kembali AUTO");
//   } else if (s.startsWith("set ")) {
//     float lo, hi;
//     if (sscanf(s.c_str() + 4, "%f %f", &lo, &hi) == 2 && setThresholds(lo, hi))
//       Serial.printf("Threshold disimpan: min=%.1f max=%.1f\n", humMin, humMax);
//     else
//       Serial.println("Format salah / nilai tidak valid. Contoh: set 60 80");
//   } else if (s == "status") {
//     Serial.printf("mode=%s relay=%d min=%.1f max=%.1f\n",
//                   mode == MODE_AUTO ? "AUTO" : "MANUAL", relayOn, humMin, humMax);
//   }
// }

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_RELAY, OUTPUT);
  relaySet(false);          // pompa mati sejak awal
  loadConfig();
  dht.begin();
  analogSetPinAttenuation(PIN_UV, ADC_11db);
  commBegin();
  Serial.println("Perintah: on | off | set <min> <max> | status  (line ending: Newline)");
}

void loop() {
  commLoop();
  // handleSerial();
  MistingCmd cmd;
  while (commGetCommand(cmd)) handleCommand(cmd);
  controlPump();

  MistingReport rep;
  rep.airTemp  = data.suhu;
  rep.airHum   = data.hum;
  rep.uvIndex  = data.uvIndex;
  rep.dhtValid = data.dhtValid;
  rep.pumpOn   = relayOn;
  rep.manual   = (mode == MODE_MANUAL);
  commSetReport(rep);

  if (millis() - lastRead >= SENSOR_INTERVAL_MS) {
    lastRead = millis();
    readSensors();
    if (data.dhtValid) {
      Serial.printf("T=%.1fC RH=%.1f%% UV=%.2f relay=%d\n",
                    data.suhu, data.hum, data.uvIndex, relayOn);
    } else {
      Serial.printf("DHT ERROR (gagal %u kali) UV=%.2f relay=%d\n",
                    dhtFail, data.uvIndex, relayOn);
    }
  }
}