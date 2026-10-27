#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <string.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <vector>
#include <algorithm>
#include <esp_timer.h>
#include <time.h>
#include <stdarg.h>

/* === DEFINITIONS === */

#define DEFAULT_SLAVE_CHANNEL 1
#define SLAVE_TYPE "Irrigation"

// Intervals of function/usecase call
#define DEBUG_INT 10000      // 10 secs
#define SEARCH_INT 1000      // ms; dwell time per channel while searching master
#define REQ_INT 15000        // ms; 3x master POLL_INTERVAL (5000)
#define NEW_INT 2000         // ms; interval of reading all sensors (newSlaveState)
// #define SENSOR_DEBUG   // enable raw/state sensor lines on each NEW_INT (for calibration)
// #define LOG_DEBUG      // enable raw ESP-NOW packets, JSON payloads, and each search channel

// Temperature value from misting slave
#define TDS_TEMP_DEFAULT 25.0f   // C; used when master has no misting temperature
#define TDS_TEMP_MIN 0.0f
#define TDS_TEMP_MAX 60.0f

// pins
#define pin_WaterSV 26
#define pin_FertSV 27
#define pin_Pump 25
#define pin_WaterUS_Echo 18
#define pin_WaterUS_Trig 5
#define pin_FertUS_Echo 4
#define pin_FertUS_Trig 19
#define pin_FertTemp 36
#define pin_FertPH 39
#define pin_FertTDS 34
#define pin_SoilMoist 33
#define pin_SoilPH 32

// Environment constantas
#define SCOUNT 30            // TDS sample points (median filter)
#define SOUND_SPEED 0.034    // cm/us
#define reservoirDepth 60    // cm, sensor to reservoir bottom (dummy)

// ESP-NOW
#define READING_PACKET_MAX  250   // batas payload ESP-NOW
#define READING_HEADER_SIZE 24    // srcAddr(6)+msgType(1)+slaveID(1)+slaveType(16)
#define REPORT_BUFFER_SIZE  (READING_PACKET_MAX - READING_HEADER_SIZE)  // 226

// Schedule (NVS)
#define NVS_SCHEDULE_NAMESPACE "saved_schedules"   // 15 karakter = batas NVS
#define SECONDS_PER_DAY        86400UL
static_assert(sizeof(NVS_SCHEDULE_NAMESPACE) - 1 <= 15, "NVS namespace max 15 chars");

// Time sync
#define TIME_RESYNC_THRESHOLD_MS 2000     // epoch baru diterapkan hanya jika selisih >= ini
#define MIN_VALID_EPOCH          1700000000UL   // samakan dengan master
#define WIB_OFFSET_S             25200L   // UTC+7, tanpa DST

/* === MESSAGE TYPES ==== */

#define MSG_SEARCH_MASTER   0
#define MSG_MASTER_CONFIRM  1
#define MSG_MASTER_REQUEST  2
#define MSG_SLAVE_READING   3

// Logger: one line "<time> [TAG   ] message". Prototypes allow use anywhere in the sketch.
void slog(const char *tag, const char *fmt, ...)  __attribute__((format(printf, 2, 3)));
void slogw(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef LOG_DEBUG
  #define slogd(tag, ...) slog(tag, __VA_ARGS__)
#else
  #define slogd(tag, ...) ((void)0)
#endif

/* === STRUCTS === */

// 0. Slave searching for a master
typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];     
  uint8_t msgType;
  char slaveType[16];            
} search_master;

// 1. Master confirming slave is searching for master
typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];     
  uint8_t msgType;
  uint8_t slaveID;
  uint8_t channel;                
} master_confirm;

// 2. Master requesting slave for reading reports
typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];  
  uint8_t msgType;
  uint8_t dstAddr[6];
  uint8_t slaveID;
  char slaveType[16];
  uint32_t reqTime;          // master millis() when polling
  float   airTemp;           // misting air temperature relayed by master (C)
  uint8_t airTempValid;      // 1 = airTemp usable
  uint32_t epoch;            // UTC epoch (second) from NTP; 0 = master time is invalid
} master_request;
static_assert(sizeof(master_request) == 43, "master_request size mismatch");

// 3. Slave reading report
typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];
  uint8_t msgType;
  uint8_t slaveID;
  char slaveType[16];
  char reportBuffer[REPORT_BUFFER_SIZE];   // serialized JSON, null-terminated
} slave_reading;

static_assert(offsetof(slave_reading, reportBuffer) == READING_HEADER_SIZE, "header size mismatch");
static_assert(sizeof(slave_reading) == READING_PACKET_MAX, "slave_reading must be 250 bytes");

struct SlaveState {
  float waterLevel;        // 0-1
  float fertLevel;         // 0-1
  float soilMoist;         // 0-1
  float soilPH;
  float fertPH;
  float fertTDS;           // ppm
  bool  isPumpActive;
  bool  isWaterSVActive;
  bool  isFertSVActive;
};

// Daily schedule. time in sec in day, 0..86400.
struct schedule {
  uint32_t start;   // start time
  uint32_t stop;    // stop time (= start + duration), start < stop <= 86400
};

struct DefaultSchedule {
  const char *key;          // "HH:MM"
  uint32_t    durationSec;
};

const DefaultSchedule defaultSchedules[] = {
  {"08:00", 120}, {"12:00", 120}, {"16:00", 120}
};

std::vector<schedule> schedules;   // sorted ascending on start

/* === GLOBAL VARIABLES === */

uint8_t master_mac[6] = {0};
uint8_t slaveID = 0;

// ESP-NOW
volatile bool master_found = false;     // safely used in the background
volatile bool received_confirm = false;
uint8_t current_scan_channel;
volatile uint8_t active_channel;
unsigned long lastSearchStep = 0;
bool searchStepPending = false;         // true -> next searchMasterStep() runs immediately

bool master_peer_added = false;
volatile unsigned long lastRequestTime = 0;

// Calibration constants.
// All are PLACEHOLDERS until calibrated (see checklist).
const int   AirValue   = 3500;    // raw ADC (0-4095) in dry air
const int   WaterValue = 1300;    // raw ADC in water/saturated soil
const float fertPH_V7    = 2.50f; // volt at pH 7 (PH-4502C)
const float fertPH_Slope = 0.18f; // volt per pH unit (V at pH4 minus V at pH7, divided by 3)
const float soilPH_V7    = 2.50f; // PLACEHOLDER, sensor output unknown
const float soilPH_Slope = 0.18f; // PLACEHOLDER
const float tdsK         = 1.0f;  // TDS correction factor (standard solution / reading)

volatile float tdsTempC = TDS_TEMP_DEFAULT;  // water temperature for TDS compensation (overwrited in the future)
const uint16_t tdsGapMs = 40;                 // delay between TDS samples
const unsigned long echoTimeoutUs = 30000;

// Sensor data
// SlaveState slaveState = {0.9f, 0.5f, 0.1f, 0.6f, 0.2f, true, false, true};  // dummy
SlaveState slaveState = {};
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

// Dipanggil dari loop() oleh kode pembacaan sensor nanti (belum dipanggil: data dummy)
void updateSlaveState(const SlaveState &s) {
  portENTER_CRITICAL(&stateMux);
  slaveState = s;
  portEXIT_CRITICAL(&stateMux);
}



/* === SENSOR READING === */

float round2(float x) { return roundf(x * 100.0f) / 100.0f; }

// median filter
int getMedianNum(int bArray[], int iFilterLen) {
  int bTab[SCOUNT];
  for (int i = 0; i < iFilterLen; i++) bTab[i] = bArray[i];
  for (int j = 0; j < iFilterLen - 1; j++) {
    for (int i = 0; i < iFilterLen - j - 1; i++) {
      if (bTab[i] > bTab[i + 1]) {
        int t = bTab[i]; bTab[i] = bTab[i + 1]; bTab[i + 1] = t;
      }
    }
  }
  if (iFilterLen & 1) return bTab[(iFilterLen - 1) / 2];
  return (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2;
}

// Read a pin 'count' times (gapMs apart), return the median in millivolts
int readMedianMilliVolts(uint8_t pin, int count, uint16_t gapMs) {
  int buf[SCOUNT];
  if (count > SCOUNT) count = SCOUNT;
  if (count < 1) count = 1;
  for (int i = 0; i < count; i++) {
    buf[i] = analogReadMilliVolts(pin);
    if (gapMs > 0 && i < count - 1) delay(gapMs);
  }
  return getMedianNum(buf, count);
}

float phFromMilliVolts(int mV, float v7, float slope) {
  float ph = 7.0f + (v7 - mV / 1000.0f) / slope;
  return constrain(ph, 0.0f, 14.0f);
}

// Returns distance in cm, or -1 when no echo arrived
float readDistanceCm(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  unsigned long duration = pulseIn(echoPin, HIGH, echoTimeoutUs);
  if (duration == 0) return -1.0f;
  return duration * SOUND_SPEED / 2.0f;
}

// distance (sensor above the water) -> level 0-1; no echo -> 0
float distanceToLevel(float distCm) {
  if (distCm < 0) return 0.0f;
  return constrain(1.0f - distCm / (float)reservoirDepth, 0.0f, 1.0f);
}

// TDS (ppm); returns the median millivolts through medianMv for debugging
float readTDS(int &medianMv) {
  medianMv = readMedianMilliVolts(pin_FertTDS, SCOUNT, tdsGapMs);
  float voltage = medianMv / 1000.0f;
  float compensationCoefficient = 1.0f + 0.02f * (tdsTempC - 25.0f);
  float v = voltage / compensationCoefficient;
  float tds = (133.42f * v * v * v - 255.86f * v * v + 857.39f * v) * 0.5f;
  return tds * tdsK;
}

// Read all sensors once and return the new state.
// Order: pH first, TDS last (keeps the two probes apart in time).
SlaveState newSlaveState() {
  SlaveState s = {};

  // 1. pH (single read)
  int fertPH_mV = readMedianMilliVolts(pin_FertPH, 1, 0);
  int soilPH_mV = readMedianMilliVolts(pin_SoilPH, 1, 0);
  s.fertPH = round2(phFromMilliVolts(fertPH_mV, fertPH_V7, fertPH_Slope));
  s.soilPH = round2(phFromMilliVolts(soilPH_mV, soilPH_V7, soilPH_Slope));

  // 2. ultrasonic -> level
  float waterDist = readDistanceCm(pin_WaterUS_Trig, pin_WaterUS_Echo);
  float fertDist  = readDistanceCm(pin_FertUS_Trig, pin_FertUS_Echo);
  s.waterLevel = round2(distanceToLevel(waterDist));
  s.fertLevel  = round2(distanceToLevel(fertDist));

  // 3. soil moisture (raw ADC, calibrated by AirValue/WaterValue)
  int soilRaw = analogRead(pin_SoilMoist);
  s.soilMoist = round2(constrain((float)(AirValue - soilRaw) / (float)(AirValue - WaterValue), 0.0f, 1.0f));

  // 4. relay state (active-HIGH)
  s.isPumpActive    = (digitalRead(pin_Pump) == HIGH);
  s.isWaterSVActive = (digitalRead(pin_WaterSV) == HIGH);
  s.isFertSVActive  = (digitalRead(pin_FertSV) == HIGH);

  // 5. TDS last (SCOUNT samples, blocks ~1.2 s)
  int tds_mV = 0;
  s.fertTDS = round2(readTDS(tds_mV));

#ifdef SENSOR_DEBUG
    slog("SENSOR", "raw fertPH %dmV soilPH %dmV water %.1fcm fert %.1fcm soil %d tds %dmV @%.1fC",
      fertPH_mV, soilPH_mV, waterDist, fertDist, soilRaw, tds_mV, (float)tdsTempC);
    slog("SENSOR", "state wLvl %.2f fLvl %.2f moist %.2f soilPH %.2f fertPH %.2f TDS %.2f pump %d wSV %d fSV %d",
      s.waterLevel, s.fertLevel, s.soilMoist, s.soilPH, s.fertPH, s.fertTDS,
      s.isPumpActive, s.isWaterSVActive, s.isFertSVActive);
#endif

  return s;
}

// Pins and safe relay state. Called first in setup().
void initSensorsAndRelays() {
  pinMode(pin_WaterSV, OUTPUT);
  pinMode(pin_FertSV, OUTPUT);
  pinMode(pin_Pump, OUTPUT);
  digitalWrite(pin_WaterSV, LOW);   // active-HIGH: LOW = off
  digitalWrite(pin_FertSV, LOW);
  digitalWrite(pin_Pump, LOW);

  pinMode(pin_WaterUS_Trig, OUTPUT);
  pinMode(pin_WaterUS_Echo, INPUT);
  pinMode(pin_FertUS_Trig, OUTPUT);
  pinMode(pin_FertUS_Echo, INPUT);

  pinMode(pin_FertPH, INPUT);
  pinMode(pin_SoilPH, INPUT);
  pinMode(pin_FertTDS, INPUT);
  pinMode(pin_SoilMoist, INPUT);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
}



/* === UTILITY FUNCTIONS === */

void macToStr(const uint8_t *mac, char *buf) {
  snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Add master as a peer and storing its information
bool addMasterPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    slogw("MASTER", "failed to add peer: %s", esp_err_to_name(result));
    return false;
  }
  return true;
}

// Send unicast message to master
bool sendToMaster(const uint8_t *data, size_t length) {
  if (!master_found) {
    slogw("MASTER", "cannot send: master not found");
    return false;
  }
  esp_err_t result = esp_now_send(master_mac, data, length);
  if (result != ESP_OK) slogw("MASTER", "send failed: %s", esp_err_to_name(result));
  return result == ESP_OK;
}

// Prepare a new search. Does not block and does not send anything.
void startSearch() {
  // drop stale master peer (master may have rebooted on another channel)
  if (esp_now_is_peer_exist(master_mac)) esp_now_del_peer(master_mac);
  memset(master_mac, 0, 6);
  
  tdsTempC = TDS_TEMP_DEFAULT;   // no master -> no misting temperature

  // start from the last known channel when re-searching
  current_scan_channel = (active_channel != 0) ? active_channel : DEFAULT_SLAVE_CHANNEL;
  slog("SEARCH", "looking for master, starting at ch %d", current_scan_channel);
  searchStepPending = true;   // first broadcast runs on the next step
}

// One non-blocking step: broadcast on the current channel, wait SEARCH_INT, then hop.
void searchMasterStep() {
  static const uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  if (master_found) return;

  if (!searchStepPending) {
    if (millis() - lastSearchStep < SEARCH_INT) return;
    current_scan_channel = (current_scan_channel % 13) + 1;
    if (current_scan_channel == 1) slog("SEARCH", "wrapped to ch 1, still searching");
  }
  searchStepPending = false;
  lastSearchStep = millis();

  search_master searchMessage = {};
  WiFi.macAddress(searchMessage.srcAddr);
  searchMessage.msgType = MSG_SEARCH_MASTER;
  strncpy(searchMessage.slaveType, SLAVE_TYPE, sizeof(searchMessage.slaveType) - 1);

  WiFi.setChannel(current_scan_channel);
  slogd("SEARCH", "broadcast on ch %d", current_scan_channel);
  if (current_scan_channel == 1) slog("SEARCH", "sweep complete, still searching");

  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&searchMessage, sizeof(searchMessage));
  if (result != ESP_OK) slogw("SEARCH", "broadcast failed: %s", esp_err_to_name(result));
}



/* === TIME === */

// Written from ESP-NOW callback (WiFi task), read from loop(): must be protected by mux
portMUX_TYPE timeMux = portMUX_INITIALIZER_UNLOCKED;
int64_t timeOffsetMs = 0;     // epoch_UTC_ms - uptime_ms
bool    timeSynced   = false;

int64_t uptimeMs() { return esp_timer_get_time() / 1000; }

// epoch = UTC from master, rxMs = uptimeMs() when packet is received
void syncTime(uint32_t epoch, int64_t rxMs) {
  if (epoch < MIN_VALID_EPOCH) return;   // master time is invalid

  int64_t newOffset = (int64_t)epoch * 1000 - rxMs;
  bool applied, first;
  int64_t diff;

  portENTER_CRITICAL(&timeMux);
  first = !timeSynced;
  diff = newOffset - timeOffsetMs;
  applied = first || diff >= TIME_RESYNC_THRESHOLD_MS || diff <= -TIME_RESYNC_THRESHOLD_MS;
  if (applied) {
    timeOffsetMs = newOffset;
    timeSynced = true;
  }
  portEXIT_CRITICAL(&timeMux);

  if (applied) {
    if (first) slog("TIME", "first sync from master");
    else slog("TIME", "resync step %lld ms", (long long)diff);
  }
}

// Local WIB epoch (second), -1 if not synced
int64_t getLocalEpoch() {
  portENTER_CRITICAL(&timeMux);
  bool ok = timeSynced;
  int64_t off = timeOffsetMs;
  portEXIT_CRITICAL(&timeMux);
  if (!ok) return -1;
  return (uptimeMs() + off) / 1000 + WIB_OFFSET_S;
}

// sec-in-day WIB (0..86399), -1 if not synced
int32_t getLocalSecondOfDay() {
  int64_t t = getLocalEpoch();
  return t < 0 ? -1 : (int32_t)(t % (int64_t)SECONDS_PER_DAY);
}

// Local day number WIB, -1 if not synced
int32_t getLocalDayNumber() {
  int64_t t = getLocalEpoch();
  return t < 0 ? -1 : (int32_t)(t / (int64_t)SECONDS_PER_DAY);
}



/* === LOGGING === */

void logPrefix(char *buf, size_t len) {
  int64_t t = getLocalEpoch();
  if (t >= 0) {
    uint32_t s = (uint32_t)(t % (int64_t)SECONDS_PER_DAY);
    snprintf(buf, len, "%02lu:%02lu:%02lu",
             (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  } else {
    snprintf(buf, len, "+%.1fs", (double)uptimeMs() / 1000.0);
  }
}

static void vlog(const char *tag, const char *mark, const char *fmt, va_list ap) {
  char pre[16];
  char msg[200];
  logPrefix(pre, sizeof(pre));
  vsnprintf(msg, sizeof(msg), fmt, ap);
  Serial.printf("%s [%-6s] %s%s\n", pre, tag, mark, msg);
}

void slog(const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog(tag, "", fmt, ap);
  va_end(ap);
}

void slogw(const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlog(tag, "WARN: ", fmt, ap);
  va_end(ap);
}


/* === SCHEDULE === */

// "HH:MM" -> sec-in-day. false if format/range is incorrect.
bool parseScheduleKey(const char *key, uint32_t &startSec) {
  if (strlen(key) != 5 || key[2] != ':' ||
      !isdigit((unsigned char)key[0]) || !isdigit((unsigned char)key[1]) ||
      !isdigit((unsigned char)key[3]) || !isdigit((unsigned char)key[4])) return false;
  int h = (key[0] - '0') * 10 + (key[1] - '0');
  int m = (key[3] - '0') * 10 + (key[4] - '0');
  if (h > 23 || m > 59) return false;
  startSec = (uint32_t)h * 3600UL + (uint32_t)m * 60UL;
  return true;
}

void formatSecondOfDay(uint32_t sec, char *buf, size_t len) {
  snprintf(buf, len, "%02lu:%02lu:%02lu",
           (unsigned long)(sec / 3600), (unsigned long)((sec / 60) % 60), (unsigned long)(sec % 60));
}

// Seed 3 default schedule to NVS (key "HH:MM", stop value is in sec-in-day)
bool seedDefaultSchedules() {
  Preferences prefs;
  if (!prefs.begin(NVS_SCHEDULE_NAMESPACE, false)) return false;

  bool ok = true;
  for (const auto &d : defaultSchedules) {
    uint32_t startSec;
    if (!parseScheduleKey(d.key, startSec)) { ok = false; continue; }
    if (prefs.putUInt(d.key, startSec + d.durationSec) == 0) ok = false;
  }
  prefs.end();
  return ok;
}

// Read all NVS entry to 'out' (unsorted). Invalid entri is skipped and logged.
// Return: number of raw entry in NVS (including the invalid(s) one); 0 = empty namespace.
size_t readScheduleEntries(std::vector<schedule> &out) {
  out.clear();

  Preferences prefs;
  if (!prefs.begin(NVS_SCHEDULE_NAMESPACE, false)) {
    slogw("SCHED", "failed to open NVS namespace");
    return 0;
  }

  size_t entries = 0;
  nvs_iterator_t it = NULL;
  esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_SCHEDULE_NAMESPACE, NVS_TYPE_U32, &it);

  while (err == ESP_OK && it != NULL) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    entries++;

    uint32_t startSec = 0;
    uint32_t stopSec  = prefs.getUInt(info.key, 0);

    if (!parseScheduleKey(info.key, startSec)) {
      slogw("SCHED", "skip key '%s': invalid key", info.key);
    } else if (stopSec <= startSec || stopSec > SECONDS_PER_DAY) {
      slogw("SCHED", "skip key '%s': invalid stop %lu", info.key, (unsigned long)stopSec);
    } else {
      out.push_back({startSec, stopSec});
    }

    err = nvs_entry_next(&it);   // it = NULL if there is no more entry
  }

  if (it) nvs_release_iterator(it);
  prefs.end();
  return entries;
}

// ascending based on start, remove overlapping schedules, store to 'schedules'
void buildSchedules(std::vector<schedule> &raw) {
  std::sort(raw.begin(), raw.end(),
            [](const schedule &a, const schedule &b) { return a.start < b.start; });

  schedules.clear();
  for (const auto &s : raw) {
    if (!schedules.empty() && s.start < schedules.back().stop) {
      char a[9];
      formatSecondOfDay(s.start, a, sizeof(a));
      slogw("SCHED", "skip %s: overlaps previous schedule", a);
      continue;
    }
    schedules.push_back(s);
  }
}

void printSchedules() {
  slog("SCHED", "%u schedule(s) loaded", (unsigned)schedules.size());
  for (size_t i = 0; i < schedules.size(); i++) {
    char a[9], b[9];
    formatSecondOfDay(schedules[i].start, a, sizeof(a));
    formatSecondOfDay(schedules[i].stop, b, sizeof(b));
        slog("SCHED", "#%u %s-%s (%lu s)", (unsigned)i, a, b,
          (unsigned long)(schedules[i].stop - schedules[i].start));
  }
}

// called once in setup(): seed default if NVS is empty, then export to 'schedules'
void initSchedules() {
  std::vector<schedule> raw;
  size_t entries = readScheduleEntries(raw);

  if (entries == 0) {
    slog("SCHED", "NVS empty, writing defaults");
    if (!seedDefaultSchedules()) slogw("SCHED", "failed to write defaults");
    readScheduleEntries(raw);
  }

  buildSchedules(raw);
  printSchedules();
}



/* === SCHEDULE EXECUTION === */

size_t   nextSchedule_i = 0;            // index of the next schedule in 'schedules'
schedule nextSchedule_time = {0, 0};    // copy of schedules[nextSchedule_i]
schedule curSchedule_time  = {0, 0};    // current or most recently executed schedule
bool     nextIsTomorrow = false;        // true: nextSchedule_time = first schedule tomorrow
bool     curScheduleDone = true;        // true: curSchedule_time already executed (or there isnt one)
bool     isExecutingSchedule = false;   // true: pump is being activated (according to schedule)
unsigned long pumpStartMs = 0;
unsigned long pumpRunMs   = 0;
int32_t  lastDayNumber = -1;            // -1: not yet initialized schedule (waiting for valid time)

// Bring nextSchedule_i back to the first daily schedule with start > nowSec. Passed schedule is ignored not removed,
// if no more schedule today: back to 0 and flag it as tomorrow's schedule
void getNextSchedule(uint32_t nowSec) {
  if (schedules.empty()) return;

  while (nextSchedule_i < schedules.size() && schedules[nextSchedule_i].start <= nowSec) {
    nextSchedule_i++;
  }
  if (nextSchedule_i >= schedules.size()) {
    nextSchedule_i = 0;
    nextIsTomorrow = true;
  }
  nextSchedule_time = schedules[nextSchedule_i];
}

void startPump(unsigned long runMs) {
  digitalWrite(pin_Pump, HIGH);
  pumpStartMs = millis();
  pumpRunMs = runMs;
  isExecutingSchedule = true;
  curScheduleDone = true;     // this schedule is already executed dont execute it again
  char a[9], b[9];
  formatSecondOfDay(curSchedule_time.start, a, sizeof(a));
  formatSecondOfDay(curSchedule_time.stop, b, sizeof(b));
  slog("PUMP", "ON for %lu s (schedule %s-%s)", runMs / 1000UL, a, b);
}

void stopPump(const char *reason) {
  digitalWrite(pin_Pump, LOW);
  isExecutingSchedule = false;
  slog("PUMP", "OFF (%s)", reason);
}

// Called first time of valid time and each day change
void resyncSchedules(uint32_t nowSec) {
  if (isExecutingSchedule) stopPump("new day");   // make sure the previous day's schedule has stopped

  nextSchedule_i = 0;
  nextIsTomorrow = false;
  getNextSchedule(nowSec);

  // slave turns on or synced mid schedule: that schedule is still executed as long as its remaining time
  curSchedule_time = {0, 0};
  curScheduleDone = true;
  for (const auto &s : schedules) {
    if (s.start <= nowSec && nowSec < s.stop) {
      curSchedule_time = s;
      curScheduleDone = false;
      break;
    }
  }
}

// Called in each loop() iteration
void maintainSchedule() {
  // Stop is based on millis(): keep running even if clock shift or not synced
  if (isExecutingSchedule && millis() - pumpStartMs >= pumpRunMs) stopPump("duration reached");

  if (schedules.empty()) return;

  int64_t t = getLocalEpoch();
  if (t < 0) return;                      // not synced: pump will not be activated
  int32_t  day    = (int32_t)(t / (int64_t)SECONDS_PER_DAY);
  uint32_t nowSec = (uint32_t)(t % (int64_t)SECONDS_PER_DAY);

  if (lastDayNumber >= 0 && day < lastDayNumber) return;   // past mid night the clock moves backward
  if (lastDayNumber < 0 || day > lastDayNumber) {
    lastDayNumber = day;
    resyncSchedules(nowSec);
  }

  // 1. next schedule start time has been passed
  if (!nextIsTomorrow && nowSec >= nextSchedule_time.start) {
    if (isExecutingSchedule) stopPump("next schedule reached");
    curSchedule_time = nextSchedule_time;
    curScheduleDone = false;
    getNextSchedule(nowSec);
  }

  // 2. active pump on interval [start, stop)
  if (!isExecutingSchedule && !curScheduleDone &&
      nowSec >= curSchedule_time.start && nowSec < curSchedule_time.stop) {
    startPump((unsigned long)(curSchedule_time.stop - nowSec) * 1000UL);
  }
}

void printStatus() {
  char mac[18] = "none";
  if (master_found) macToStr(master_mac, mac);

  char nxt[24] = "-";
  if (lastDayNumber >= 0) {
    uint32_t s = nextSchedule_time.start;
    snprintf(nxt, sizeof(nxt), "%02lu:%02lu%s", (unsigned long)(s / 3600),
             (unsigned long)((s / 60) % 60), nextIsTomorrow ? " (tomorrow)" : "");
  }

  uint8_t pri;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&pri, &sec);

  slog("STATUS", "master %s | id %d | ch %d | next %s | pump %s",
       mac, slaveID, pri, nxt, isExecutingSchedule ? "ON" : "OFF");

  SlaveState s;
  portENTER_CRITICAL(&stateMux);
  s = slaveState;
  portEXIT_CRITICAL(&stateMux);

  slog("SENSOR", "wLvl %.2f fLvl %.2f moist %.2f soilPH %.2f fertPH %.2f TDS %.1f | relays %d/%d/%d",
       s.waterLevel, s.fertLevel, s.soilMoist, s.soilPH, s.fertPH, s.fertTDS,
       s.isPumpActive, s.isWaterSVActive, s.isFertSVActive);
}



/* =====================================================
   MESSAGE HANDLERS
   ===================================================== */

// When master send a confirmation message that this slave is registered by it
void onMasterConfirm(const uint8_t *incomingData, int len, const uint8_t *senderMac) {
  if (len != sizeof(master_confirm)) {
    slogw("MASTER", "invalid master_confirm size: %d (expected %u)", len, (unsigned)sizeof(master_confirm));
    return;
  }

  master_confirm confirmMessage;
  memcpy(&confirmMessage, incomingData, sizeof(confirmMessage));

  memcpy(master_mac, senderMac, 6);
  slaveID = confirmMessage.slaveID;
  active_channel = confirmMessage.channel;

  // Adding the master to ESP-NOW peer table
  if (!addMasterPeer(master_mac)) {
    slogw("MASTER", "failed to register master");
    return;
  }

  lastRequestTime = millis();   // grace period after (re)registration
  master_found = true;
  received_confirm = true;

  char mac[18];
  macToStr(master_mac, mac);
  slog("MASTER", "registered %s | slave ID %d | ch %d", mac, slaveID, active_channel);
}

// When master is polling for reading reports
void onMasterRequest( const uint8_t *incomingData, int len) {
    int64_t rxMs = uptimeMs();

  // Check if the message type is incorrectly assigned to the data
  // So it is actually not a master_request data
  if (len != sizeof(master_request)) {
    slogw("REQ", "invalid master_request size: %d (expected %u)", len, (unsigned)sizeof(master_request));
    return;
  }

  // reset the timer for request
  lastRequestTime = millis();

  // create request message
  master_request request;

  memcpy(&request, incomingData, sizeof(request));
  syncTime(request.epoch, rxMs);
  uint32_t reqTime = request.reqTime;

  // air temperature of the misting slave (relayed by master) for TDS compensation
  bool tempOk = request.airTempValid &&
                request.airTemp >= TDS_TEMP_MIN && request.airTemp <= TDS_TEMP_MAX;
  tdsTempC = tempOk ? request.airTemp : TDS_TEMP_DEFAULT;

  // consistent snapshot of the last state (not guaranted to be the actual latest / current)
  SlaveState s;
  portENTER_CRITICAL(&stateMux);
  s = slaveState;
  portEXIT_CRITICAL(&stateMux);

  // create reading report message
  slave_reading reading = {};
  WiFi.macAddress(reading.srcAddr);
  reading.msgType = MSG_SLAVE_READING;
  reading.slaveID = slaveID;
  strncpy(reading.slaveType,SLAVE_TYPE,sizeof(reading.slaveType) - 1);

  // create json report
  JsonDocument report;
  report["reqTime"]            = reqTime;
  report["slaveID"]            = slaveID;
  report["waterLevel"]      = s.waterLevel;
  report["fertLevel"]       = s.fertLevel;
  report["soilMoist"]       = s.soilMoist;
  report["soilPH"]          = s.soilPH;
  report["fertPH"]          = s.fertPH;
  report["fertTDS"]         = s.fertTDS;
  report["isPumpActive"]    = s.isPumpActive;
  report["isWaterSVActive"] = s.isWaterSVActive;
  report["isFertSVActive"]  = s.isFertSVActive;

  size_t jsonLen = measureJson(report);
  if (report.overflowed() || jsonLen >= sizeof(reading.reportBuffer)) {
    slogw("REQ", "report too large: %u/%u B, not sent",
          (unsigned)jsonLen, (unsigned)(sizeof(reading.reportBuffer) - 1));
    return;
  }
  serializeJson(report, reading.reportBuffer, sizeof(reading.reportBuffer));

  // send the created message by unicast
  // header + JSON + '\0'
  size_t packetLen = READING_HEADER_SIZE + jsonLen + 1;
  bool sent = sendToMaster((uint8_t *)&reading, packetLen);
  slog("REQ", "#%lu tds-temp %.1fC (%s) -> %s, %u B", (unsigned long)reqTime,
       (float)tdsTempC, tempOk ? "misting" : "default",
       sent ? "reading sent" : "SEND FAILED", (unsigned)packetLen);
  slogd("REQ", "payload %s", reading.reportBuffer);
}

/* =====================================================
   ESP-NOW RECEIVE CALLBACK
   ===================================================== */

void onDataRecv( const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  if (len < 7) {
    slogw("NOW", "packet too short (%d B)", len);
    return;
  }

  uint8_t msgType;
  memcpy(&msgType, incomingData + 6, sizeof(msgType));
  slogd("NOW", "rx type %d, %d B from " MACSTR, msgType, len, MAC2STR(info->src_addr));

  switch (msgType) {
    case MSG_MASTER_CONFIRM:
      onMasterConfirm(incomingData, len, info->src_addr);
      break;

    case MSG_MASTER_REQUEST:
      onMasterRequest(incomingData, len);
      break;

    // add new message handler here

    default:
      slogw("NOW", "unknown message type %d", msgType);
      break;
  }
}

/* =====================================================
   SETUP
   ===================================================== */

void setup() {
  Serial.begin(115200);
  initSensorsAndRelays();
  delay(1000);

  slog("BOOT", "irrigation slave starting");

  schedules.reserve(8);
  initSchedules();

  WiFi.mode(WIFI_STA);

  WiFi.disconnect();

  delay(100);

  // Set default channel
  esp_wifi_set_channel(DEFAULT_SLAVE_CHANNEL, WIFI_SECOND_CHAN_NONE);

  // Initialize ESP-NOW
  esp_err_t result = esp_now_init();

  if (result != ESP_OK) {
    slogw("BOOT", "ESP-NOW init failed: %s", esp_err_to_name(result));

    while (true) {
      delay(1000);
    }
  }

  // Register native receive callback
  result = esp_now_register_recv_cb(onDataRecv);

  if (result != ESP_OK) {
    slogw("BOOT", "failed to register recv callback: %s", esp_err_to_name(result));

    while (true) {
      delay(1000);
    }
  }

  /*
     Broadcast peer needed for sending broadcast
  */

  // broadcast address that is FF:FF:FF:FF:FF:FF:FF
  uint8_t broadcastAddress[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF
  };

  // create the peer
  esp_now_peer_info_t broadcastPeer = {};

  memcpy(broadcastPeer.peer_addr, broadcastAddress, 6);

  broadcastPeer.channel = 0;
  broadcastPeer.ifidx = WIFI_IF_STA;
  broadcastPeer.encrypt = false;

  // adding the broadcast peer to ESP-NOW peer table
  result = esp_now_add_peer(&broadcastPeer);

  if (result != ESP_OK && result != ESP_ERR_ESPNOW_EXIST) {
    slogw("BOOT", "failed to add broadcast peer: %s", esp_err_to_name(result));
  }

  uint8_t m[6];
  char mac[18];
  WiFi.macAddress(m);
  macToStr(m, mac);
  slog("BOOT", "MAC %s | ch %d | ESP-NOW ready", mac, DEFAULT_SLAVE_CHANNEL);

  // Search for master runs non-blocking from loop().
  startSearch();
}

/* =====================================================
   LOOP
   ===================================================== */

void loop() {
  // Master confirmation is set by the WiFi callback; move the radio to its channel.
  if (received_confirm) {
    received_confirm = false;
    esp_wifi_set_channel(active_channel, WIFI_SECOND_CHAN_NONE);
    current_scan_channel = active_channel;
    lastRequestTime = millis();
    slogd("MASTER", "radio synced to master channel %d", active_channel);
  }

  if (master_found) {
    // master was registered but no request arrives: master/router may have rebooted
    if (millis() - lastRequestTime >= REQ_INT) {
      slogw("MASTER", "no request for %d s, searching again", REQ_INT / 1000);
      master_found = false;
      startSearch();
    }
  } else {
    searchMasterStep();
  }

  maintainSchedule();

  // read all sensors every NEW_INT and publish the snapshot
  static unsigned long lastNewState = 0;
  static bool firstState = true;
  if (firstState || millis() - lastNewState >= NEW_INT) {
    firstState = false;
    lastNewState = millis();                 // start-to-start period (read takes ~1.3 s)
    SlaveState s = newSlaveState();
    updateSlaveState(s);
  }

  static unsigned long lastDebug = 0;

  if (millis() - lastDebug >= DEBUG_INT) {
    lastDebug = millis();
    printStatus();
  }

  delay(100);
}