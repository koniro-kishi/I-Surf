#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include "config.h"

// =====================================================
// FORMAT PAKET - HARUS IDENTIK dengan Master_Controller.ino
// dan Irrigation_Slave_Controller.ino. Jangan diubah sepihak.
// =====================================================
#define MSG_SEARCH_MASTER       0
#define MSG_MASTER_CONFIRM      1
#define MSG_MASTER_REQUEST      2
#define MSG_SLAVE_READING       3
#define MSG_MASTER_COMMAND      4
#define MSG_SLAVE_COMMAND_ACK   5
#define MSG_SLAVE_COMMAND_STAT  6

#define COMMAND_BUFFER_SIZE     64

// cmdType
#define CMD_TIMED_INSTANT   1
#define CMD_INSTANT_CHANGE  2
#define CMD_DEACTIVATION    3
// cmdStatus (dipakai di slave_command_stat)
#define CMD_ST_DONE         3
#define CMD_ST_FAILED       4

#define READING_PACKET_MAX  250
#define READING_HEADER_SIZE 24
#define REPORT_BUFFER_SIZE  (READING_PACKET_MAX - READING_HEADER_SIZE)

typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];
  uint8_t msgType;
  char slaveType[16];
} search_master;

typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];
  uint8_t msgType;
  uint8_t slaveID;
  uint8_t channel;
} master_confirm;

typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];
  uint8_t msgType;
  uint8_t dstAddr[6];
  uint8_t slaveID;
  char slaveType[16];
  uint32_t reqTime;
  float   airTemp;
  uint8_t airTempValid;
  uint32_t epoch;
} master_request;
static_assert(sizeof(master_request) == 43, "master_request size mismatch");

typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];
  uint8_t msgType;
  uint8_t slaveID;
  char slaveType[16];
  char reportBuffer[REPORT_BUFFER_SIZE];
} slave_reading;
static_assert(offsetof(slave_reading, reportBuffer) == READING_HEADER_SIZE, "header size mismatch");
static_assert(sizeof(slave_reading) == READING_PACKET_MAX, "slave_reading must be 250 bytes");

// 4. Master sends a command to a slave
typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];
  uint8_t msgType;
  uint8_t dstAddr[6];
  uint8_t slaveID;
  uint8_t cmdID;
  uint8_t cmdType;
  char commandBuffer[COMMAND_BUFFER_SIZE];   // serialized JSON, null-terminated
} master_command;
static_assert(sizeof(master_command) == 16 + COMMAND_BUFFER_SIZE, "master_command size mismatch");

// 5. Slave confirms it received a command (commandBuffer empty)
typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];
  uint8_t msgType;
  uint8_t cmdID;
  uint8_t cmdType;
  char commandBuffer[COMMAND_BUFFER_SIZE];
} slave_command_ack;
static_assert(sizeof(slave_command_ack) == 9 + COMMAND_BUFFER_SIZE, "slave_command_ack size mismatch");

// 6. Slave reports the outcome (commandBuffer: {} or {"reason":"rejected"|"interrupted"})
typedef struct __attribute__((packed)) {
  uint8_t srcAddr[6];
  uint8_t msgType;
  uint8_t cmdID;
  uint8_t cmdType;
  char commandBuffer[COMMAND_BUFFER_SIZE];
  uint8_t cmdStatus;                         // CMD_ST_DONE or CMD_ST_FAILED
} slave_command_stat;
static_assert(sizeof(slave_command_stat) == 10 + COMMAND_BUFFER_SIZE, "slave_command_stat size mismatch");

#define CMD_DEDUP_N          8
#define CMD_DEDUP_WINDOW_MS  30000UL
#define STAT_REPEAT          3
#define STAT_GAP_MS          500

// perintah dari master untuk .ino
struct MistingCmd {
  uint8_t id;
  uint8_t type;
  char    buf[COMMAND_BUFFER_SIZE];
};

// =====================================================
// DATA LAPORAN (diisi oleh .ino, dibaca saat master polling)
// =====================================================
struct MistingReport {
  float airTemp = 0;
  float airHum  = 0;
  float uvIndex = 0;
  bool  dhtValid = false;
  bool  pumpOn   = false;
  bool  manual   = false;
};

// =====================================================
// STATE INTERNAL (awalan _ = jangan disentuh dari .ino)
// =====================================================
static portMUX_TYPE _commMux = portMUX_INITIALIZER_UNLOCKED;
static const uint8_t _BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t  _masterMac[6] = {0};
static uint8_t  _slaveId = 0;
static uint8_t  _confirmChannel = 0;
static volatile bool _found = false;
static volatile bool _confirmPending = false;
static volatile bool _reqPending = false;
static volatile uint32_t _reqTime = 0;
static volatile unsigned long _lastRequest = 0;

static uint8_t  _curChannel = DEFAULT_SLAVE_CHANNEL;
static uint8_t  _activeChannel = 0;
static unsigned long _lastSearchStep = 0;
static bool _searchStepPending = false;

static MistingReport _report;

static uint8_t _selfMac[6] = {0};

// antrean perintah: diisi _onRecv, di-ack lalu diserahkan ke .ino oleh commLoop()/commGetCommand()
#define _Q_FREE         0
#define _Q_NEW_EXEC     1   // baru diterima, perlu ack lalu eksekusi
#define _Q_NEW_ACKONLY  2   // duplikat (retry master), cukup di-ack ulang
#define _Q_READY        3   // sudah di-ack, menunggu diambil .ino
struct _CmdSlot { uint8_t st; uint8_t id; uint8_t type; uint32_t seq; char buf[COMMAND_BUFFER_SIZE]; };
static _CmdSlot _cmdQ[6];
static uint32_t _cmdSeq = 0;

// dedup: hanya disentuh dari _onRecv (satu task)
static bool          _seenUsed[CMD_DEDUP_N] = {};
static uint8_t       _seenId[CMD_DEDUP_N]   = {};
static unsigned long _seenMs[CMD_DEDUP_N]   = {};
static uint8_t       _seenNext = 0;

// laporan hasil yang dikirim 3x (kiriman buta)
struct _StatJob { bool used; uint8_t id, type, status; char reason[16]; uint8_t left; unsigned long nextMs; };
static _StatJob _statJobs[4];

// =====================================================
// CALLBACK RECEIVE: hanya menyalin, pemrosesan di commLoop()
// =====================================================
static void _onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < 7) return;
  uint8_t type = data[6];

  if (type == MSG_MASTER_CONFIRM && len == (int)sizeof(master_confirm)) {
    // saat sudah terdaftar, hanya terima dari master yang sama
    if (_found && memcmp(info->src_addr, _masterMac, 6) != 0) return;
    master_confirm c;
    memcpy(&c, data, sizeof(c));
    portENTER_CRITICAL(&_commMux);
    memcpy(_masterMac, info->src_addr, 6);
    _slaveId = c.slaveID;
    _confirmChannel = c.channel;
    _confirmPending = true;
    portEXIT_CRITICAL(&_commMux);
  }
  else if (type == MSG_MASTER_REQUEST && len == (int)sizeof(master_request)) {
    if (!_found || memcmp(info->src_addr, _masterMac, 6) != 0) return;  // bukan dari master kita
    master_request r;
    memcpy(&r, data, sizeof(r));   // epoch/airTemp diabaikan: misting tidak butuh
    portENTER_CRITICAL(&_commMux);
    _reqTime = r.reqTime;
    _reqPending = true;
    _lastRequest = millis();
    portEXIT_CRITICAL(&_commMux);
  }
  else if (type == MSG_MASTER_COMMAND && len == (int)sizeof(master_command)) {
    if (!_found || memcmp(info->src_addr, _masterMac, 6) != 0) return;
    master_command m;
    memcpy(&m, data, sizeof(m));
    if (memcmp(m.dstAddr, _selfMac, 6) != 0 || m.slaveID != _slaveId) return;
    m.commandBuffer[sizeof(m.commandBuffer) - 1] = '\0';

    unsigned long now = millis();
    bool dup = false;
    for (uint8_t i = 0; i < CMD_DEDUP_N; i++) {
      if (_seenUsed[i] && _seenId[i] == m.cmdID && now - _seenMs[i] < CMD_DEDUP_WINDOW_MS) { dup = true; break; }
    }

    bool queued = false;
    portENTER_CRITICAL(&_commMux);
    for (auto &s : _cmdQ) {
      if (s.st != _Q_FREE) continue;
      s.id = m.cmdID;
      s.type = m.cmdType;
      s.seq = ++_cmdSeq;
      if (dup) {
        s.st = _Q_NEW_ACKONLY;
      } else {
        memcpy(s.buf, m.commandBuffer, COMMAND_BUFFER_SIZE);
        s.st = _Q_NEW_EXEC;
      }
      queued = true;
      break;
    }
    portEXIT_CRITICAL(&_commMux);

    if (queued && !dup) {   // not queued (full) -> not recorded, master retries
      _seenUsed[_seenNext] = true;
      _seenId[_seenNext] = m.cmdID;
      _seenMs[_seenNext] = now;
      _seenNext = (_seenNext + 1) % CMD_DEDUP_N;
    }
  }
}

// =====================================================
// FUNGSI INTERNAL
// =====================================================
static bool _addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_err_t r = esp_now_add_peer(&p);
  if (r != ESP_OK) Serial.printf("[COMM] add peer gagal: %s\n", esp_err_to_name(r));
  return r == ESP_OK;
}

static void _startSearch() {
  if (esp_now_is_peer_exist(_masterMac)) esp_now_del_peer(_masterMac);
  portENTER_CRITICAL(&_commMux);
  memset(_masterMac, 0, 6);
  _reqPending = false;
  portEXIT_CRITICAL(&_commMux);
  _found = false;
  _curChannel = (_activeChannel != 0) ? _activeChannel : DEFAULT_SLAVE_CHANNEL;
  _searchStepPending = true;
}

static void _searchStep() {
  if (!_searchStepPending) {
    if (millis() - _lastSearchStep < SEARCH_INT_MS) return;
    _curChannel = (_curChannel % 13) + 1;
  }
  _searchStepPending = false;
  _lastSearchStep = millis();

  search_master msg = {};
  WiFi.macAddress(msg.srcAddr);
  msg.msgType = MSG_SEARCH_MASTER;
  strncpy(msg.slaveType, SLAVE_TYPE, sizeof(msg.slaveType) - 1);

  esp_wifi_set_channel(_curChannel, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[COMM] mencari master di channel %d...\n", _curChannel);
  esp_now_send(_BCAST, (uint8_t *)&msg, sizeof(msg));
}

static void _sendReport(uint32_t reqTime) {
  MistingReport r;
  portENTER_CRITICAL(&_commMux);
  r = _report;
  portEXIT_CRITICAL(&_commMux);

  JsonDocument doc;
  doc["reqTime"] = reqTime;            // WAJIB, kalau tidak laporan dibuang master
  doc["slaveID"] = _slaveId;
  if (r.dhtValid) {
    doc["airTemp"] = roundf(r.airTemp * 10.0f) / 10.0f;
    doc["airHum"]  = roundf(r.airHum  * 10.0f) / 10.0f;
  }
  doc["uvIndex"]      = roundf(r.uvIndex * 100.0f) / 100.0f;
  doc["mode"]         = r.manual ? "manual" : "auto";
  doc["isPumpActive"] = r.pumpOn;
  doc["dhtError"]     = !r.dhtValid;

  slave_reading pkt = {};
  WiFi.macAddress(pkt.srcAddr);
  pkt.msgType = MSG_SLAVE_READING;
  pkt.slaveID = _slaveId;
  strncpy(pkt.slaveType, SLAVE_TYPE, sizeof(pkt.slaveType) - 1);

  size_t jsonLen = measureJson(doc);
  if (doc.overflowed() || jsonLen >= sizeof(pkt.reportBuffer)) {
    Serial.println("[COMM] laporan terlalu besar, tidak dikirim");
    return;
  }
  serializeJson(doc, pkt.reportBuffer, sizeof(pkt.reportBuffer));

  size_t packetLen = READING_HEADER_SIZE + jsonLen + 1;
  esp_err_t res = esp_now_send(_masterMac, (uint8_t *)&pkt, packetLen);
  Serial.printf("[COMM] laporan -> master: %s (%u byte) %s\n",
                esp_err_to_name(res), (unsigned)packetLen, pkt.reportBuffer);
}

static void _sendAck(uint8_t id, uint8_t type) {
  slave_command_ack a = {};
  memcpy(a.srcAddr, _selfMac, 6);
  a.msgType = MSG_SLAVE_COMMAND_ACK;
  a.cmdID = id;
  a.cmdType = type;
  esp_err_t r = esp_now_send(_masterMac, (uint8_t *)&a, sizeof(a));
  Serial.printf("[COMM] ack #%u -> master: %s\n", id, esp_err_to_name(r));
}

static void _pumpStatJobs() {
  unsigned long now = millis();
  for (auto &j : _statJobs) {
    if (!j.used || (long)(now - j.nextMs) < 0) continue;

    slave_command_stat s = {};
    memcpy(s.srcAddr, _selfMac, 6);
    s.msgType = MSG_SLAVE_COMMAND_STAT;
    s.cmdID = j.id;
    s.cmdType = j.type;
    if (j.reason[0]) snprintf(s.commandBuffer, sizeof(s.commandBuffer), "{\"reason\":\"%s\"}", j.reason);
    s.cmdStatus = j.status;
    if (_found) esp_now_send(_masterMac, (uint8_t *)&s, sizeof(s));
    Serial.printf("[COMM] stat #%u status %u -> master (%u left)\n", j.id, j.status, j.left - 1);

    if (--j.left == 0) j.used = false;
    else j.nextMs = now + STAT_GAP_MS;
  }
}

// =====================================================
// API PUBLIK (dipanggil dari .ino)
// =====================================================
inline bool commBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  esp_wifi_set_channel(DEFAULT_SLAVE_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.print("[COMM] MAC slave: ");
  Serial.println(WiFi.macAddress());
  WiFi.macAddress(_selfMac);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[COMM] esp_now_init GAGAL");
    return false;
  }
  if (esp_now_register_recv_cb(_onRecv) != ESP_OK) {
    Serial.println("[COMM] register callback GAGAL");
    return false;
  }
  _addPeer(_BCAST);
  _startSearch();
  Serial.println("[COMM] siap, mencari master");
  return true;
}

// panggil tiap iterasi loop()
inline void commLoop() {
  if (_confirmPending) {
    uint8_t mac[6], ch;
    portENTER_CRITICAL(&_commMux);
    memcpy(mac, _masterMac, 6);
    ch = _confirmChannel;
    _confirmPending = false;
    portEXIT_CRITICAL(&_commMux);

    if (_addPeer(mac)) {
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      _curChannel = ch;
      _activeChannel = ch;
      _lastRequest = millis();   // masa tenggang
      _found = true;
      Serial.printf("[COMM] master terdaftar. ID=%d channel=%d\n", _slaveId, ch);
    }
  }

  if (_found) {
    if (_reqPending) {
      uint32_t rt;
      portENTER_CRITICAL(&_commMux);
      rt = _reqTime;
      _reqPending = false;
      portEXIT_CRITICAL(&_commMux);
      _sendReport(rt);
    }
    if (millis() - _lastRequest >= REQ_TIMEOUT_MS) {
      Serial.println("[COMM] master tidak polling, mencari ulang");
      _startSearch();
    }
  } else {
    _searchStep();
  }

    // ack perintah yang diterima; duplikat cukup di-ack ulang
  for (auto &s : _cmdQ) {
    uint8_t st, id, type;
    portENTER_CRITICAL(&_commMux);
    st = s.st; id = s.id; type = s.type;
    portEXIT_CRITICAL(&_commMux);
    if (st != _Q_NEW_EXEC && st != _Q_NEW_ACKONLY) continue;

    if (_found) _sendAck(id, type);
    portENTER_CRITICAL(&_commMux);
    s.st = (st == _Q_NEW_EXEC) ? _Q_READY : _Q_FREE;
    portEXIT_CRITICAL(&_commMux);
  }
  _pumpStatJobs();
}

// update data laporan; dipanggil dari .ino setelah sensor/pompa berubah
inline void commSetReport(const MistingReport &r) {
  portENTER_CRITICAL(&_commMux);
  _report = r;
  portEXIT_CRITICAL(&_commMux);
}

// ambil perintah tertua yang sudah di-ack; false jika tidak ada
inline bool commGetCommand(MistingCmd &out) {
  bool got = false;
  portENTER_CRITICAL(&_commMux);
  _CmdSlot *best = nullptr;
  for (auto &s : _cmdQ) {
    if (s.st == _Q_READY && (best == nullptr || s.seq < best->seq)) best = &s;
  }
  if (best) {
    out.id = best->id;
    out.type = best->type;
    memcpy(out.buf, best->buf, COMMAND_BUFFER_SIZE);
    best->st = _Q_FREE;
    got = true;
  }
  portEXIT_CRITICAL(&_commMux);
  return got;
}

// hasil perintah: CMD_ST_DONE / CMD_ST_FAILED; reason: nullptr, "rejected", "interrupted"
inline void commSendStat(uint8_t id, uint8_t type, uint8_t status, const char *reason) {
  for (auto &j : _statJobs) {
    if (j.used) continue;
    j = {};
    j.used = true;
    j.id = id; j.type = type; j.status = status;
    if (reason) strncpy(j.reason, reason, sizeof(j.reason) - 1);
    j.left = STAT_REPEAT;
    j.nextMs = millis();
    _pumpStatJobs();
    return;
  }
  Serial.printf("[COMM] antrean stat penuh, laporan #%u dibuang\n", id);
}

inline bool    commMasterFound() { return _found; }
inline uint8_t commSlaveId()     { return _slaveId; }