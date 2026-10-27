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
#define MSG_SEARCH_MASTER   0
#define MSG_MASTER_CONFIRM  1
#define MSG_MASTER_REQUEST  2
#define MSG_SLAVE_READING   3

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
}

// update data laporan; dipanggil dari .ino setelah sensor/pompa berubah
inline void commSetReport(const MistingReport &r) {
  portENTER_CRITICAL(&_commMux);
  _report = r;
  portEXIT_CRITICAL(&_commMux);
}

inline bool    commMasterFound() { return _found; }
inline uint8_t commSlaveId()     { return _slaveId; }