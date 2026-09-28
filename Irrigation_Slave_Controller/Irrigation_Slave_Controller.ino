#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <string.h>

/* === DEFINITIONS === */

#define DEFAULT_SLAVE_CHANNEL 1
#define SLAVE_TYPE "Irrigation"
#define DEBUG_INTERVAL 10000 // 10 secs

#define MSG_SEARCH_MASTER 0
#define MSG_MASTER_CONFIRM 1
#define MSG_MASTER_REQUEST 2
#define MSG_IRRIGATION_READING 3

/* === STRUCTS === */

// 0. Slave searching for a master
typedef struct __attribute__((packed)) {
  uint8_t senderMacAddr[6];     
  uint8_t msgType;
  char slaveType[16];            
} search_master;

// 1. Master confirming slave is searching for master
typedef struct __attribute__((packed)) {
  uint8_t senderMacAddr[6];     
  uint8_t msgType;
  int slaveID;                
} master_confirm;

// 2. Master requesting slave for reading reports
typedef struct __attribute__((packed)) {
  uint8_t senderMacAddr[6];  
  uint8_t msgType;
  uint8_t receiverMacAddr[6];
  uint8_t slaveID;
  char slaveType[16];
} master_request;

// 3. Irrigation slave reading report
typedef struct __attribute__((packed)) {
  uint8_t senderMacAddr[6];     
  uint8_t msgType;
  uint8_t slaveID;
  char slaveType[16];

  float soilMoist;
  float waterPH;
  float fertPH;
  float waterTDS;
  float fertTDS;

  bool isPumpActive;
  bool isWaterValveActive;
  bool isFertValveActive;
} irrigation_reading;



/* === GLOBAL VARIABLES === */

uint8_t master_mac[6] = {0};
uint16_t slaveID = 0;

volatile bool master_found = false;     // safely used in the background
volatile bool received_confirm = false;
uint8_t current_scan_channel;
uint8_t active_channel;

bool master_peer_added = false;

// Dummy sensor data
bool isPumpActive = true;
float soilMoist = 0.9;
float waterPH = 0.5;
float waterTDS = 0.6;
bool isWaterValveActive = false;
float fertPH = 0.1;
float fertTDS = 0.2;
bool isFertValveActive = true;



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
  peerInfo.channel = active_channel;
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
  active_channel = current_scan_channel;

  // Adding the master to ESP-NOW peer table
  if (!addMasterPeer(master_mac)) {
    Serial.println("Failed to register Master");
    return;
  }

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

  // create request message
  master_request request;

  memcpy(&request, incomingData, sizeof(request));

  Serial.println("\n=== MASTER REQUEST RECEIVED ===");

  Serial.printf("Request for Slave ID: %d\n", request.slaveID);

  // create reading report message
  irrigation_reading reading = {};

  WiFi.macAddress(reading.senderMacAddr);
  
  reading.msgType = MSG_IRRIGATION_READING;
  reading.slaveID = slaveID;

  strncpy(reading.slaveType,SLAVE_TYPE,sizeof(reading.slaveType) - 1);

  reading.isPumpActive = isPumpActive;
  reading.soilMoist = soilMoist;
  reading.waterPH = waterPH;
  reading.fertPH = fertPH;
  reading.waterTDS = waterTDS;
  reading.fertTDS = fertTDS;
  reading.isWaterValveActive = isWaterValveActive;
  reading.isFertValveActive = isFertValveActive;

  // send the created message by unicast
  sendToMaster((uint8_t *)&reading, sizeof(reading));

  Serial.println("Irrigation reading sent.");
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

  // create the search message
  search_master searchMessage = {};

  WiFi.macAddress(searchMessage.senderMacAddr);
  
  searchMessage.msgType = MSG_SEARCH_MASTER;
  
  strncpy(searchMessage.slaveType, SLAVE_TYPE, sizeof(searchMessage.slaveType) - 1);


  // channel hopping loop

  current_scan_channel = DEFAULT_SLAVE_CHANNEL;

  while (!master_found) {
    
    WiFi.setChannel(current_scan_channel);

    Serial.printf("Broadcasting search on channel %d...\n", current_scan_channel);

    // sending a message to broadcast address (broadcasting the message)
    result = esp_now_send(broadcastAddress, (uint8_t *)&searchMessage, sizeof(searchMessage));

    Serial.printf("Broadcast status: %s\n", esp_err_to_name(result));

    /*
       Callback onDataRecv can still be running while this process is on going
    */

    for (int i = 0; i < 10 && !master_found; i++) {
      delay(100);
    }

    if(master_found){
      break;
    }

    current_scan_channel = (current_scan_channel % 13) + 1;
  }

  Serial.println("\nConnected to Master!");
  Serial.println("Starting normal operation...");
}

/* =====================================================
   LOOP
   ===================================================== */

void loop() {
  static unsigned long lastDebug = 0;

  // print debug each interval of DEBUG_INTERVAL sec
  if (millis() - lastDebug >= DEBUG_INTERVAL) {
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

    Serial.printf("Channel: %d\n", active_channel);
  }

  delay(100);
}