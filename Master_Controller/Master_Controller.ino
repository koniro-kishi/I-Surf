#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <LiquidCrystal_I2C.h>
#include <string.h>
#include <vector>

/* =====================================================
   CONFIGURATION
   ===================================================== */
#define SLAVE_CHANNEL 6
#define WIFI_SSID "Sapiq"
#define WIFI_PASSWORD "123581321"

#define WIFI_INTERFACE WIFI_IF_STA

/* =====================================================
   MESSAGE TYPES
   ===================================================== */

#define MSG_SEARCH_MASTER 0
#define MSG_MASTER_CONFIRM 1
#define MSG_MASTER_REQUEST 2
#define MSG_IRRIGATION_READING 3
#define MSG_MISTING_READING 4

/* =====================================================
   DATA STRUCTURES
   ===================================================== */

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

/* =====================================================
   SLAVE INFORMATION
   ===================================================== */

struct SlaveInfo {
  uint8_t mac[6];
  uint16_t slaveID;
  String slaveType;
};

std::vector<SlaveInfo> slaves;

/* =====================================================
   GLOBAL VARIABLES
   ===================================================== */

uint8_t active_channel = 0;

LiquidCrystal_I2C lcd(0x27, 16, 2);

/* =====================================================
   UTILITY FUNCTIONS
   ===================================================== */

void printMac(const uint8_t *mac) {
  Serial.printf(
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2],
    mac[3], mac[4], mac[5]
  );
}

void printSingleLog(
  const char *text,
  int duration = 0
) {
  Serial.println(text);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(text);

  if (duration > 0) {
    delay(duration);
    lcd.clear();
  }
}

void printDoubleLog(
  const char *line1,
  const char *line2,
  int duration = 0
) {
  Serial.println(line1);
  Serial.println(line2);

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(line1);

  lcd.setCursor(0, 1);
  lcd.print(line2);

  if (duration > 0) {
    delay(duration);
    lcd.clear();
  }
}

bool isSlaveRegistered(const uint8_t *mac) {
  for (const auto &slave : slaves) {
    if (memcmp(slave.mac, mac, 6) == 0) {
      return true;
    }
  }

  return false;
}

bool addSlavePeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) {
    Serial.println("Slave peer already exists.");
    return true;
  }

  esp_now_peer_info_t peerInfo = {};

  memcpy(
    peerInfo.peer_addr,
    mac,
    6
  );

  peerInfo.channel = active_channel;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  esp_err_t result = esp_now_add_peer(&peerInfo);

  if (result != ESP_OK) {
    Serial.printf(
      "Failed to add Slave peer: %s\n",
      esp_err_to_name(result)
    );

    return false;
  }

  Serial.println("Slave peer added successfully.");

  return true;
}

/* =====================================================
   SLAVE DISCOVERY
   ===================================================== */

void onDiscSlave(
  const esp_now_recv_info_t *info,
  const uint8_t *incomingData,
  int len
) {
  if (len != sizeof(search_master)) {
    Serial.println("Invalid search_master size.");
    return;
  }

  search_master searchMessage;

  memcpy(
    &searchMessage,
    incomingData,
    sizeof(searchMessage)
  );

  if (searchMessage.msgType != MSG_SEARCH_MASTER) {
    return;
  }

  Serial.println("\n==============================");
  Serial.println("SLAVE DISCOVERY RECEIVED");
  Serial.println("==============================");

  Serial.print("Slave MAC: ");
  printMac(info->src_addr);
  Serial.println();

  Serial.printf(
    "Slave type: %s\n",
    searchMessage.slaveType
  );

  if (isSlaveRegistered(info->src_addr)) {
    Serial.println("Slave already registered.");
    return;
  }

  if (!addSlavePeer(info->src_addr)) {
    Serial.println("Could not register Slave.");
    return;
  }

  SlaveInfo newSlave = {};

  memcpy(
    newSlave.mac,
    info->src_addr,
    6
  );

  newSlave.slaveID = slaves.size() + 1;
  newSlave.slaveType = String(searchMessage.slaveType);

  slaves.push_back(newSlave);

  // Create confirmation message
  master_confirm confirmMessage = {};

  WiFi.macAddress(confirmMessage.senderMacAddr);

  confirmMessage.msgType = MSG_MASTER_CONFIRM;
  confirmMessage.slaveID = newSlave.slaveID;

  esp_err_t result = esp_now_send(
    newSlave.mac,
    (uint8_t *)&confirmMessage,
    sizeof(confirmMessage)
  );

  Serial.printf(
    "Confirmation status: %s\n",
    esp_err_to_name(result)
  );

  if (result == ESP_OK) {
    Serial.printf(
      "Successfully registered Slave #%d\n",
      newSlave.slaveID
    );
  }
}

/* =====================================================
   SENSOR READING HANDLER
   ===================================================== */

void onRecvReading_Irrigation(
  const uint8_t *incomingData,
  int len
) {
  if (len != sizeof(irrigation_reading)) {
    Serial.println("Invalid irrigation_reading size.");
    return;
  }

  irrigation_reading reading;

  memcpy(
    &reading,
    incomingData,
    sizeof(reading)
  );

  if (reading.msgType != MSG_IRRIGATION_READING) {
    return;
  }

  Serial.println("\n==============================");
  Serial.println("IRRIGATION READING RECEIVED");
  Serial.println("==============================");

  Serial.printf(
    "Slave type: %s\n",
    reading.slaveType
  );

  Serial.printf(
    "Slave ID: %d\n",
    reading.slaveID
  );

  Serial.printf(
    "Soil moisture: %.2f\n",
    reading.soilMoist
  );

  Serial.printf(
    "Water pH: %.2f\n",
    reading.waterPH
  );

  Serial.printf(
    "Water TDS: %.2f\n",
    reading.waterTDS
  );

  Serial.printf(
    "Water valve: %s\n",
    reading.isWaterValveActive ? "ON" : "OFF"
  );

  Serial.printf(
    "Fertilizer pH: %.2f\n",
    reading.fertPH
  );

  Serial.printf(
    "Fertilizer TDS: %.2f\n",
    reading.fertTDS
  );

  Serial.printf(
    "Fertilizer valve: %s\n",
    reading.isFertValveActive ? "ON" : "OFF"
  );

  Serial.printf(
    "Irrigation pump: %s\n",
    reading.isPumpActive ? "ON" : "OFF"
  );

  char line[17];

  snprintf(
    line,
    sizeof(line),
    "Slave #%d",
    reading.slaveID
  );

  printDoubleLog(
    "Received from:",
    line,
    1000
  );
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

  if (len < 7) {
    Serial.println("Packet too short.");
    return;
  }

  uint8_t msgType;

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
    case MSG_SEARCH_MASTER:
      onDiscSlave(info, incomingData, len);
      break;

    case MSG_IRRIGATION_READING:
      onRecvReading_Irrigation(incomingData, len);
      break;

    default:
      Serial.println("Unknown message type.");
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
  Serial.println("     MASTER CONTROLLER");
  Serial.println("==============================");

  lcd.init();
  lcd.backlight();

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  Serial.print("Connecting to Wi-Fi");

  unsigned long startTime = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startTime < 30000
  ) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection failed.");

    while (true) {
      delay(1000);
    }
  }

  active_channel = WiFi.channel();

  Serial.println("Wi-Fi connected.");
  Serial.printf(
    "Wi-Fi channel: %d\n",
    active_channel
  );

  Serial.print("Master MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  /*
     ESP-NOW initialization
  */

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

  Serial.println("ESP-NOW initialized.");
  Serial.println("Receive callback registered.");

  /*
     Add broadcast peer.
     Ini tidak wajib untuk menerima broadcast,
     tetapi diperlukan untuk mengirim broadcast.
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

  broadcastPeer.channel = active_channel;
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

  printSingleLog(
    "Master online",
    2000
  );

  Serial.println("Setup complete.");
}

/* =====================================================
   LOOP
   ===================================================== */

void loop() {
  static unsigned long lastPolling = 0;
  static unsigned long lastStatus = 0;

  /*
     Print status setiap 10 detik
  */

  if (millis() - lastStatus >= 10000) {
    lastStatus = millis();

    Serial.println("\n=== MASTER STATUS ===");

    Serial.printf(
      "Registered slaves: %d\n",
      slaves.size()
    );

    Serial.printf(
      "Channel: %d\n",
      active_channel
    );
  }

  /*
     Polling setiap 5 detik
  */

  if (
    slaves.size() > 0 &&
    millis() - lastPolling >= 5000
  ) {
    lastPolling = millis();

    Serial.println("\n=== POLLING SLAVES ===");

    for (auto &slave : slaves) {
      master_request requestMessage = {};

      WiFi.macAddress(
        requestMessage.senderMacAddr
      );

      requestMessage.msgType = MSG_MASTER_REQUEST;

      memcpy(
        requestMessage.receiverMacAddr,
        slave.mac,
        6
      );

      requestMessage.slaveID = slave.slaveID;

      strncpy(
        requestMessage.slaveType,
        slave.slaveType.c_str(),
        sizeof(requestMessage.slaveType) - 1
      );

      Serial.printf(
        "Requesting Slave #%d (",
        slave.slaveID
      );

      printMac(slave.mac);
      Serial.println(")");

      esp_err_t result = esp_now_send(
        slave.mac,
        (uint8_t *)&requestMessage,
        sizeof(requestMessage)
      );

      Serial.printf(
        "Request status: %s\n",
        esp_err_to_name(result)
      );

      delay(100);
    }
  }

  delay(10);
}