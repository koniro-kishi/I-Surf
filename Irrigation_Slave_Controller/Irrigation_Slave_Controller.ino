#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <string.h>

/* === DEFINITIONS === */

#define DEFAULT_WIFI_CHANNEL 6
#define SLAVE_CHANNEL 6
#define SLAVE_TYPE "Irrigation"

#define MSG_SEARCH_MASTER 0
#define MSG_MASTER_CONFIRM 1
#define MSG_MASTER_REQUEST 2
#define MSG_IRRIGATION_READING 3

/* === STRUCTS === */

typedef struct __attribute__((packed)) {
  uint8_t senderMacAddr[6];     
  uint8_t msgType;
  char slaveType[16];            
} search_master;

typedef struct __attribute__((packed)) {
  uint8_t senderMacAddr[6];     
  uint8_t msgType;
  int slaveID;                
} master_confirm;

typedef struct __attribute__((packed)) {
  uint8_t senderMacAddr[6];  
  uint8_t msgType;
  uint8_t receiverMacAddr[6];
  uint8_t slaveID;
  char slaveType[16];
} master_request;

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

void printMac(const uint8_t *mac) {
  Serial.printf(
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2],
    mac[3], mac[4], mac[5]
  );
}

bool addMasterPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) {
    Serial.println("Master peer already exists");
    return true;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);

  peerInfo.channel = SLAVE_CHANNEL;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

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

bool sendToMaster(
  const uint8_t *data,
  size_t length
) {
  if (!master_found) {
    Serial.println("Cannot send: Master not found");
    return false;
  }

  esp_err_t result = esp_now_send(
    master_mac,
    data,
    length
  );

  Serial.printf(
    "Send to Master: %s\n",
    esp_err_to_name(result)
  );

  return result == ESP_OK;
}



/* =====================================================
   MESSAGE HANDLERS
   ===================================================== */

void onMasterConfirm(
  const uint8_t *incomingData,
  int len,
  const uint8_t *senderMac
) {
  if (len != sizeof(master_confirm)) {
    Serial.println("Invalid master_confirm size");
    return;
  }

  master_confirm confirmMessage;
  memcpy(
    &confirmMessage,
    incomingData,
    sizeof(confirmMessage)
  );

  if (confirmMessage.msgType != MSG_MASTER_CONFIRM) {
    return;
  }

  Serial.println("\n=== MASTER CONFIRMATION RECEIVED ===");

  Serial.print("Master MAC: ");
  printMac(senderMac);
  Serial.println();

  memcpy(master_mac, senderMac, 6);

  slaveID = confirmMessage.slaveID;

  if (!addMasterPeer(master_mac)) {
    Serial.println("Failed to register Master");
    return;
  }

  master_found = true;
  received_confirm = true;

  Serial.printf(
    "Slave ID: %d\n",
    slaveID
  );

  Serial.println("Master successfully registered!");
  Serial.println("Channel hopping stopped.");
}

void onMasterRequest(
  const uint8_t *incomingData,
  int len
) {
  if (len != sizeof(master_request)) {
    Serial.println("Invalid master_request size");
    return;
  }

  master_request request;
  memcpy(&request, incomingData, sizeof(request));

  if (request.msgType != MSG_MASTER_REQUEST) {
    return;
  }

  Serial.println("\n=== MASTER REQUEST RECEIVED ===");

  Serial.printf(
    "Request for Slave ID: %d\n",
    request.slaveID
  );

  irrigation_reading reading = {};

  WiFi.macAddress(reading.senderMacAddr);

  reading.msgType = MSG_IRRIGATION_READING;
  reading.slaveID = slaveID;

  strncpy(
    reading.slaveType,
    SLAVE_TYPE,
    sizeof(reading.slaveType) - 1
  );

  reading.isPumpActive = isPumpActive;
  reading.soilMoist = soilMoist;
  reading.waterPH = waterPH;
  reading.fertPH = fertPH;
  reading.waterTDS = waterTDS;
  reading.fertTDS = fertTDS;
  reading.isWaterValveActive = isWaterValveActive;
  reading.isFertValveActive = isFertValveActive;

  sendToMaster(
    (uint8_t *)&reading,
    sizeof(reading)
  );

  Serial.println("Irrigation reading sent.");
}

/* =====================================================
   ESP-NOW RECEIVE CALLBACK
   ===================================================== */

void onDataRecv(
  const esp_now_recv_info_t *info,
  const uint8_t *incomingData,
  int len
) {
  Serial.printf(
    "\nReceived data from " MACSTR
    " | Length: %d\n",
    MAC2STR(info->src_addr),
    len
  );

  if (len < 1) {
    return;
  }

  uint8_t msgType;

  /*
     Semua struktur memiliki msgType pada byte ke-6.
     Pastikan paket memiliki ukuran minimum sebelum
     membaca byte tersebut.
  */

  if (len < 7) {
    Serial.println("Packet too short");
    return;
  }

  memcpy(
    &msgType,
    incomingData + 6,
    sizeof(msgType)
  );

  Serial.printf(
    "Message type: %d\n",
    msgType
  );

  switch (msgType) {
    case MSG_MASTER_CONFIRM:
      onMasterConfirm(
        incomingData,
        len,
        info->src_addr
      );
      break;

    case MSG_MASTER_REQUEST:
      onMasterRequest(
        incomingData,
        len
      );
      break;

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

  // Set fixed channel
  esp_wifi_set_channel(
    SLAVE_CHANNEL,
    WIFI_SECOND_CHAN_NONE
  );

  Serial.printf(
    "Slave channel: %d\n",
    SLAVE_CHANNEL
  );

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
     Broadcast peer diperlukan agar pengiriman
     broadcast berjalan dengan baik.
  */

  uint8_t broadcastAddress[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF
  };

  esp_now_peer_info_t broadcastPeer = {};

  memcpy(
    broadcastPeer.peer_addr,
    broadcastAddress,
    6
  );

  broadcastPeer.channel = SLAVE_CHANNEL;
  broadcastPeer.ifidx = WIFI_IF_STA;
  broadcastPeer.encrypt = false;

  result = esp_now_add_peer(&broadcastPeer);

  if (result != ESP_OK &&
      result != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf(
      "Failed to add broadcast peer: %s\n",
      esp_err_to_name(result)
    );
  }

  /*
     Channel hopping dinonaktifkan sementara.
     Pastikan Master menggunakan channel yang sama.
  */

  search_master searchMessage = {};

  WiFi.macAddress(searchMessage.senderMacAddr);

  searchMessage.msgType = MSG_SEARCH_MASTER;

  strncpy(
    searchMessage.slaveType,
    SLAVE_TYPE,
    sizeof(searchMessage.slaveType) - 1
  );

  while (!master_found) {
    Serial.printf(
      "Broadcasting search on channel %d...\n",
      SLAVE_CHANNEL
    );

    result = esp_now_send(
      broadcastAddress,
      (uint8_t *)&searchMessage,
      sizeof(searchMessage)
    );

    Serial.printf(
      "Broadcast status: %s\n",
      esp_err_to_name(result)
    );

    /*
       Callback onDataRecv tetap dapat berjalan
       selama proses ini berlangsung.
    */

    for (int i = 0; i < 10 && !master_found; i++) {
      delay(100);
    }
  }

  Serial.println("\nConnected to Master!");
  Serial.println("Starting normal operation...");
}

/* =====================================================
   LOOP
   ===================================================== */

void loop() {
  static unsigned long lastDebug = 0;

  if (millis() - lastDebug >= 10000) {
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

    Serial.printf(
      "Slave ID: %d\n",
      slaveID
    );

    Serial.printf(
      "Channel: %d\n",
      SLAVE_CHANNEL
    );
  }

  delay(100);
}