#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <string.h>
#include <ArduinoJson.h>

/* === DEFINITIONS === */

#define DEFAULT_SLAVE_CHANNEL 1
#define SLAVE_TYPE "Irrigation"

// Intervals of function/usecase call
#define DEBUG_INT 10000      // 10 secs
#define SEARCH_INT 1000      // ms; dwell time per channel while searching master
#define REQ_INT 15000        // ms; 3x master POLL_INTERVAL (5000)
#define NEW_INT 2000         // ms; interval of reading all sensors (newSlaveState)
#define SENSOR_DEBUG         // comment out to silence the raw sensor line

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
#define pin_FertPH 36
#define pin_FertTDS 39
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

/* === MESSAGE TYPES ==== */

#define MSG_SEARCH_MASTER   0
#define MSG_MASTER_CONFIRM  1
#define MSG_MASTER_REQUEST  2
#define MSG_SLAVE_READING   3

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
} master_request;
static_assert(sizeof(master_request) == 39, "master_request size mismatch");

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
   Serial.printf("RAW | fertPH:%dmV soilPH:%dmV waterDist:%.1fcm fertDist:%.1fcm soilRaw:%d tds:%dmV @%.1fC\n",
                fertPH_mV, soilPH_mV, waterDist, fertDist, soilRaw, tds_mV, (float)tdsTempC);
  Serial.printf("STATE | wLvl:%.2f fLvl:%.2f moist:%.2f soilPH:%.2f fertPH:%.2f TDS:%.2f pump:%d wSV:%d fSV:%d\n",
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

// Print mac address as a string
void printMac(const uint8_t *mac) {
  Serial.printf(
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2],
    mac[3], mac[4], mac[5]
  );
}

// Add master as a peer and storing its information
bool addMasterPeer(const uint8_t *mac) {
  // Check if peer is registered in ESP-NOW's peer table
  if (esp_now_is_peer_exist(mac)) {
    Serial.println("Master peer already exists");
    return true;
  }

  // Creating a peer instance for master
  esp_now_peer_info_t peerInfo = {};
  
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0; // current active channel
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  // Adding the peer
  esp_err_t result = esp_now_add_peer(&peerInfo);

  if (result != ESP_OK) {
    Serial.printf(
      "Failed to add Master peer: %s\n",
      esp_err_to_name(result)
    );
    return false;
  }

  Serial.println("Master peer added successfully");
  return true;
}

// Send unicast message to master
bool sendToMaster(const uint8_t *data, size_t length) {
  // check if this slave already registered a master
  if (!master_found) {
    Serial.println("Cannot send: Master not found");
    return false;
  }

  // try to send the message
  esp_err_t result = esp_now_send(master_mac, data, length);

  Serial.printf("Send to Master: %s\n", esp_err_to_name(result));

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
  searchStepPending = true;   // first broadcast runs on the next step
}

// One non-blocking step: broadcast on the current channel, wait SEARCH_INT, then hop.
void searchMasterStep() {
  static const uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  if (master_found) return;

  if (!searchStepPending) {
    if (millis() - lastSearchStep < SEARCH_INT) return;
    current_scan_channel = (current_scan_channel % 13) + 1;
  }
  searchStepPending = false;
  lastSearchStep = millis();

  search_master searchMessage = {};
  WiFi.macAddress(searchMessage.srcAddr);
  searchMessage.msgType = MSG_SEARCH_MASTER;
  strncpy(searchMessage.slaveType, SLAVE_TYPE, sizeof(searchMessage.slaveType) - 1);

  WiFi.setChannel(current_scan_channel);
  Serial.printf("Broadcasting search on channel %d...\n", current_scan_channel);

  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&searchMessage, sizeof(searchMessage));
  Serial.printf("Broadcast status: %s\n", esp_err_to_name(result));
}



/* =====================================================
   MESSAGE HANDLERS
   ===================================================== */

// When master send a confirmation message that this slave is registered by it
void onMasterConfirm(const uint8_t *incomingData, int len, const uint8_t *senderMac) {
  // Check if the message type is incorrectly assigned to the data
  // So it is actually not a search_master data
  if (len != sizeof(master_confirm)) {
    Serial.println("Invalid master_confirm size");
    return;
  }

  // Create search message
  master_confirm confirmMessage;
  memcpy(&confirmMessage, incomingData, sizeof(confirmMessage));

  Serial.println("\n=== MASTER CONFIRMATION RECEIVED ===");
  Serial.print("Master MAC: ");
  printMac(senderMac);
  Serial.println();

  memcpy(master_mac, senderMac, 6);
  slaveID = confirmMessage.slaveID;
  active_channel = confirmMessage.channel;

  // Adding the master to ESP-NOW peer table
  if (!addMasterPeer(master_mac)) {
    Serial.println("Failed to register Master");
    return;
  }

  lastRequestTime = millis();   // grace period after (re)registration
  master_found = true;
  received_confirm = true;

  Serial.printf("Slave ID: %d\n", slaveID);

  Serial.println("Master successfully registered!");
  Serial.println("Channel hopping stopped.");
}

// When master is polling for reading reports
void onMasterRequest( const uint8_t *incomingData, int len) {
  // Check if the message type is incorrectly assigned to the data
  // So it is actually not a master_request data
  if (len != sizeof(master_request)) {
    Serial.println("Invalid master_request size");
    return;
  }

  // reset the timer for request
  lastRequestTime = millis();

  // create request message
  master_request request;

  memcpy(&request, incomingData, sizeof(request));
  uint32_t reqTime = request.reqTime;

  // air temperature of the misting slave (relayed by master) for TDS compensation
  bool tempOk = request.airTempValid &&
                request.airTemp >= TDS_TEMP_MIN && request.airTemp <= TDS_TEMP_MAX;
  tdsTempC = tempOk ? request.airTemp : TDS_TEMP_DEFAULT;
  Serial.printf("TDS temperature: %.1f C (%s)\n", (float)tdsTempC, tempOk ? "from misting" : "default");

  Serial.println("\n=== MASTER REQUEST RECEIVED ===");
  Serial.printf("Request for Slave ID: %d\n", request.slaveID);

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
    Serial.printf("Report too large: %u/%u bytes. Not sent.\n",
                  (unsigned)jsonLen, (unsigned)(sizeof(reading.reportBuffer) - 1));
    return;
  }
  serializeJson(report, reading.reportBuffer, sizeof(reading.reportBuffer));

  // send the created message by unicast
  // header + JSON + '\0'
  size_t packetLen = READING_HEADER_SIZE + jsonLen + 1;
  sendToMaster((uint8_t *)&reading, packetLen);

  Serial.printf("Reading sent (%u bytes): %s\n", (unsigned)packetLen, reading.reportBuffer);
}

/* =====================================================
   ESP-NOW RECEIVE CALLBACK
   ===================================================== */

void onDataRecv( const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  Serial.printf("\nReceived data from " MACSTR " | Length: %d\n", MAC2STR(info->src_addr), len);

  if (len < 1) {
    return;
  }

  uint8_t msgType;

  /*
     all message struct has msgType in the 6th byte
     Check if the packet has at least the minimum size
  */

  if (len < 7) {
    Serial.println("Packet too short");
    return;
  }

  // store the message type in a variable
  memcpy(&msgType, incomingData + 6, sizeof(msgType));

  Serial.printf("Message type: %d\n", msgType);

  // call apropriate handler based on the message type
  switch (msgType) {
    case MSG_MASTER_CONFIRM:
      onMasterConfirm(incomingData, len, info->src_addr);
      break;

    case MSG_MASTER_REQUEST:
      onMasterRequest(incomingData, len);
      break;

    // add new message handler here

    default:
      Serial.println("Unknown message type");
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

  Serial.println("\n==============================");
  Serial.println("  IRRIGATION SLAVE STARTING");
  Serial.println("==============================");

  WiFi.mode(WIFI_STA);

  WiFi.disconnect();

  delay(100);

  // Set default channel
  esp_wifi_set_channel(DEFAULT_SLAVE_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.printf("Slave channel: %d\n", DEFAULT_SLAVE_CHANNEL);

  Serial.print("Slave MAC: ");
  Serial.println(WiFi.macAddress());

  // Initialize ESP-NOW
  esp_err_t result = esp_now_init();

  if (result != ESP_OK) {
    Serial.printf(
      "ESP-NOW initialization failed: %s\n",
      esp_err_to_name(result)
    );

    while (true) {
      delay(1000);
    }
  }

  // Register native receive callback
  result = esp_now_register_recv_cb(onDataRecv);

  if (result != ESP_OK) {
    Serial.printf(
      "Failed to register callback: %s\n",
      esp_err_to_name(result)
    );

    while (true) {
      delay(1000);
    }
  }

  Serial.println("ESP-NOW initialized successfully.");
  Serial.println("Receive callback registered.");

  Serial.println("\nSearching for Master...");

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
    Serial.printf( "Failed to add broadcast peer: %s\n",esp_err_to_name(result)
    );
  }

   // search for master runs non-blocking from loop()
  startSearch();

  Serial.println("\nSetup complete. Searching for Master from loop()...");
}

/* =====================================================
   LOOP
   ===================================================== */

void loop() {
    // master confirmed (set by callback in WiFi task): pull radio to master's channel.
  // The radio may have stopped on an overlapping neighbour channel while searching.
  if (received_confirm) {
    received_confirm = false;
    esp_wifi_set_channel(active_channel, WIFI_SECOND_CHAN_NONE);
    current_scan_channel = active_channel;
    lastRequestTime = millis();
    Serial.printf("Radio synced to master channel %d\n", active_channel);
  }

  if (master_found) {
    // master was registered but no request arrives: master/router may have rebooted
    if (millis() - lastRequestTime >= REQ_INT) {
      Serial.println("No master request within REQ_INT. Searching master again...");
      master_found = false;
      startSearch();
    }
  } else {
    searchMasterStep();
  }

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

  // print debug each interval of DEBUG_INTERVAL sec
  if (millis() - lastDebug >= DEBUG_INT) {
    lastDebug = millis();

    Serial.println("\n=== SLAVE STATUS ===");

    Serial.print("Master connected: ");
    Serial.println(master_found ? "YES" : "NO");

    Serial.print("Master MAC: ");

    if (master_found) {
      printMac(master_mac);
    } else {
      Serial.print("None");
    }

    Serial.println();

    Serial.printf("Slave ID: %d\n", slaveID);

    Serial.printf("Active channel: %d\n", active_channel);

    uint8_t pri; wifi_second_chan_t sec;
    esp_wifi_get_channel(&pri, &sec);
    Serial.printf("Radio channel (actual): %d\n", pri);
  }

  delay(100);
}