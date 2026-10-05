#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <LiquidCrystal_I2C.h>
#include <string.h>
#include <vector>
#include <LittleFS.h>

/* =====================================================
   CONFIGURATION
   ===================================================== */
#define apSSID "I-Surf Setup"
#define apPassword "saya-berselancar"
#define WIFI_INTERFACE WIFI_IF_STA
#define STAT_INTERVAL 10000 // 10 sec
#define POLL_INTERVAL 5000  // 5 sec

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

// 4. Misting slave reading report
typedef struct __attribute__((packed)) {
  uint8_t senderMacAddr[6];     
  uint8_t msgType;
  uint8_t slaveID;
  char slaveType[16];

  float airMoist;
  float airTemp;
  float lighting;

  bool isPumpActive;
} misting_reading;

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

WiFiServer server(80);
String routerSSID = "";
String routerPassword = "";
bool connectedToWifi = false;

uint8_t active_channel = 0;

LiquidCrystal_I2C lcd(0x27, 16, 2);

/* =====================================================
   UTILITY FUNCTIONS
   ===================================================== */

String urlDecode(const String& input) {
  String decoded;
  for (size_t i = 0; i < input.length(); i++) {
    if (input[i] == '%') {
      if (i + 2 < input.length()) {
        String hex = input.substring(i + 1, i + 3);
        char c = (char)strtol(hex.c_str(), NULL, 16);
        decoded += c;
        i += 2;
      }
    } else if (input[i] == '+') {
      decoded += ' ';
    } else {
      decoded += input[i];
    }
  }
  return decoded;
}

String getQueryParam(const String& url, const String& param) {
  String search = param + "=";
  int start = url.indexOf(search);
  if (start < 0) return "";
  start += search.length();
  int end = url.indexOf('&', start);
  if (end < 0) end = url.indexOf(' ', start);
  if (end < 0) end = url.length();
  return urlDecode(url.substring(start, end));
}

String getPostValue(const String& body, const String& key) {
  String searchKey = key + "=";
  int start = body.indexOf(searchKey);
  if (start < 0) return "";
  start += searchKey.length();
  int end = body.indexOf('&', start);
  if (end < 0) end = body.length();
  return urlDecode(body.substring(start, end));
}

String htmlEscape(const String& input) {
  String output;
  output.reserve(input.length() + 16);
  for (size_t i = 0; i < input.length(); i++) {
    switch (input[i]) {
      case '&':  output += "&amp;"; break;
      case '<':  output += "&lt;"; break;
      case '>':  output += "&gt;"; break;
      case '"':  output += "&quot;"; break;
      case '\'': output += "&#39;"; break;
      default:   output += input[i]; break;
    }
  }
  return output;
}

String getWiFiIcon(int rssi) {
  if (rssi > -50) return "/wifi-strong.png";
  if (rssi > -60) return "/wifi-good.png";
  if (rssi > -70) return "/wifi-fair.png";
  return "/wifi-weak.png";
}

void sendFile(WiFiClient& client, const char* path, const char* contentType) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    client.println("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n404 Not Found");
    return;
  }

  client.println("HTTP/1.1 200 OK");
  client.print("Content-Type: "); client.println(contentType);
  client.print("Content-Length: "); client.println(file.size());
  client.println("Cache-Control: public, max-age=3600");
  client.println("Connection: close\r\n");

  uint8_t buffer[1024];
  while (file.available()) {
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead > 0) client.write(buffer, bytesRead);
  }
  file.close();
}

// Print mac address as a string
void printMac(const uint8_t *mac) {
  Serial.printf(
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2],
    mac[3], mac[4], mac[5]
  );
}

// Print one line of string in both serial and LCD
void printSingleLog(const char *text, int duration = 0) {
  Serial.println(text);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(text);

  if (duration > 0) {
    delay(duration);
    lcd.clear();
  }
}

// Print two line of string in both serial and LCD
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

// Check if a slave is already in slaves vector
bool isSlaveRegistered(const uint8_t *mac) {
  for (const auto &slave : slaves) {
    if (memcmp(slave.mac, mac, 6) == 0) {
      return true;
    }
  }

  return false;
}

// Add slave as a peer and storing its information
bool addSlavePeer(const uint8_t *mac) {
  // Check if peer is registered in ESP-NOW's peer table
  if (esp_now_is_peer_exist(mac)) {
    Serial.println("Slave peer already exists.");
    return true;
  }

  // Creating a peer instance
  esp_now_peer_info_t peerInfo = {};

  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = active_channel;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  // Adding the peer
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

// =====================================================
// PAGE RENDERERS
// =====================================================

void renderIndexPage(WiFiClient& client) {
  File templateFile = LittleFS.open("/index.html", "r");
  if (!templateFile) {
    client.println("HTTP/1.1 500 Internal Error\r\n\r\nFailed to open template");
    return;
  }

  String html = templateFile.readString();
  templateFile.close();

  // Scan networks
  WiFi.scanDelete();
  int n = WiFi.scanNetworks();
  String listHtml = "";

  if (n == 0) {
    listHtml = "<div style=\"text-align:center;\">No networks found</div>";
  } else {
    for (int i = 0; i < n; i++) {
      String networkSSID = WiFi.SSID(i);
      String safeSSID = htmlEscape(networkSSID);
      bool isOpen = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      String icon = getWiFiIcon(WiFi.RSSI(i));

      listHtml += "<div class=\"network-item\">";
      listHtml += "<a class=\"network-link\" href=\"/join?ssid=" + networkSSID + "&open=" + (isOpen ? "true" : "false") + "\">";
      listHtml += "<img class=\"wifi-icon\" src=\"" + icon + "\" alt=\"WiFi\">";
      listHtml += "<span class=\"ssid\">" + safeSSID + "</span>";
      if (!isOpen) {
        listHtml += "<img class=\"lock-icon\" src=\"/locked.png\" alt=\"Locked\">";
      }
      listHtml += "a></div>";
    }
  }

  html.replace("<!-- NETWORKS_LIST_PLACEHOLDER -->", listHtml);

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Cache-Control: no-cache\r\nConnection: close\r\n");
  client.print(html);

  WiFi.scanDelete();
}

void renderDetailPage(WiFiClient& client, const String& ssid, bool isOpen) {
  File templateFile = LittleFS.open("/join.html", "r");
  if (!templateFile) {
    client.println("HTTP/1.1 500 Internal Error\r\n\r\nFailed to open template");
    return;
  }

  String html = templateFile.readString();
  templateFile.close();

  html.replace("<!-- SSID_PLACEHOLDER -->", htmlEscape(ssid));
  html.replace("<!-- SSID_PLAIN_PLACEHOLDER -->", ssid);
  html.replace("<!-- PASSWORD_CLASS_PLACEHOLDER -->", isOpen ? "hidden" : "");

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Cache-Control: no-cache\r\nConnection: close\r\n");
  client.print(html);
}


// =====================================================
// WIFI MANAGEMENT
// =====================================================

void startSoftAP() {
  Serial.println("Starting SoftAP...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSSID, apPassword);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

bool connectToTargetWiFi() {
  Serial.println("\nConnecting to: " + routerSSID);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  if (routerPassword.length() == 0) WiFi.begin(routerSSID.c_str());
  else WiFi.begin(routerSSID.c_str(), routerPassword.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("\nFailed to connect. Reverting to SoftAP...");
  WiFi.disconnect(true);
  startSoftAP();
  return false;
}

void runSoftAP() {
  WiFiClient client = server.available();
  if (!client) return;

  unsigned long timeout = millis() + 2000;
  while (!client.available() && millis() < timeout) delay(1);

  if (!client.available()) {
    client.stop();
    return;
  }

  String requestLine = client.readStringUntil('\n');
  requestLine.trim();

  int contentLength = 0;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    if (line.startsWith("Content-Length:")) contentLength = line.substring(15).toInt();
  }

  String body = "";
  if (contentLength > 0) {
    unsigned long start = millis();
    while (body.length() < contentLength && millis() - start < 3000) {
      while (client.available() && body.length() < contentLength) {
        body += (char)client.read();
      }
    }
  }

  // --- ROUTING ---
  if (requestLine.startsWith("GET /style.css")) {
    sendFile(client, "/style.css", "text/css");
  } 
  else if (requestLine.indexOf(".png") > 0) {
    int startIdx = requestLine.indexOf("GET ") + 4;
    int endIdx = requestLine.indexOf(" HTTP");
    String path = requestLine.substring(startIdx, endIdx);
    sendFile(client, path.c_str(), "image/png");
  } 
  else if (requestLine.startsWith("GET /join")) {
    String ssid = getQueryParam(requestLine, "ssid");
    bool isOpen = (getQueryParam(requestLine, "open") == "true");
    renderDetailPage(client, ssid, isOpen);
  } 
  else if (requestLine.startsWith("POST /connect")) {
    routerSSID = getPostValue(body, "ssid");
    routerPassword = getPostValue(body, "password");

    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nConnecting...");
    client.flush();
    delay(100);
    client.stop();

    connectedToWifi = connectToTargetWiFi();
  } 
  else if (requestLine.startsWith("GET / ") || requestLine.startsWith("GET /scan")) {
    renderIndexPage(client);
  } 
  else {
    client.println("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n404");
  }

  if (client.connected()) client.stop();
}


/* =====================================================
   SLAVE DISCOVERY
   ===================================================== */

void onDiscSlave(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  // Check if the message type is incorrectly assigned to the data
  // So it is actually not a search_master data
  if (len != sizeof(search_master)) {
    Serial.println("Invalid search_master size.");
    return;
  }

  // Create search message
  search_master searchMessage;

  memcpy(&searchMessage, incomingData, sizeof(searchMessage));

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

  // Check if the slave is already in slaves vector
  if (isSlaveRegistered(info->src_addr)) {
    Serial.println("Slave already registered.");
    return;
  }

  // Try to add slave as a peer
  if (!addSlavePeer(info->src_addr)) {
    Serial.println("Could not register Slave.");
    return;
  }

  // Store the information of the new slave
  SlaveInfo newSlave = {};

  memcpy(newSlave.mac, info->src_addr, 6);
  newSlave.slaveID = slaves.size() + 1;
  newSlave.slaveType = String(searchMessage.slaveType);

  slaves.push_back(newSlave);

  // Create confirmation message
  master_confirm confirmMessage = {};

  WiFi.macAddress(confirmMessage.senderMacAddr);

  confirmMessage.msgType = MSG_MASTER_CONFIRM;
  confirmMessage.slaveID = newSlave.slaveID;

  // Try to send the confirmation message
  esp_err_t result = esp_now_send(newSlave.mac, (uint8_t *)&confirmMessage, sizeof(confirmMessage));

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

void onRecvReading_Irrigation(const uint8_t *incomingData, int len) {
  // Check if the message type is incorrectly assigned to the data
  // So it is actually not a irrigation_reading data
  if (len != sizeof(irrigation_reading)) {
    Serial.println("Invalid irrigation_reading size.");
    return;
  }

  // Create irrigation reading message
  irrigation_reading reading;
  memcpy(&reading, incomingData,sizeof(reading));

  Serial.println("\n==============================");
  Serial.println("IRRIGATION READING RECEIVED");
  Serial.println("==============================");

  Serial.printf("Slave type: %s\n", reading.slaveType);
  Serial.printf("Slave ID: %d\n", reading.slaveID);
  Serial.printf("Soil moisture: %.2f\n", reading.soilMoist);
  Serial.printf("Water pH: %.2f\n", reading.waterPH);
  Serial.printf("Water TDS: %.2f\n", reading.waterTDS);
  Serial.printf("Water valve: %s\n", reading.isWaterValveActive ? "ON" : "OFF");
  Serial.printf("Fertilizer pH: %.2f\n", reading.fertPH);
  Serial.printf("Fertilizer TDS: %.2f\n", reading.fertTDS);
  Serial.printf("Fertilizer valve: %s\n", reading.isFertValveActive ? "ON" : "OFF");
  Serial.printf("Irrigation pump: %s\n", reading.isPumpActive ? "ON" : "OFF");

  // Message footer
  char line[17];
  snprintf(line, sizeof(line), "Slave #%d", reading.slaveID);

  printDoubleLog("Received from:", line, 1000);
}

/* =====================================================
   ESP-NOW RECEIVE CALLBACK
   ===================================================== */

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  Serial.printf("\nReceived data from " MACSTR " | Length: %d\n", MAC2STR(info->src_addr), len);

  // Packet too short for it to be a message from slave
  if (len < 7) {
    Serial.println("Packet too short.");
    return;
  }

  // Get the message type of the incoming message from slave
  uint8_t msgType;
  memcpy(&msgType, incomingData + 6, sizeof(msgType));

  Serial.printf("Message type: %d\n", msgType);

  // Call appropriate handler based on the message type
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

  /*
    WiFi module initialization
  */

   // Initialize the WiFi module
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    return;
  }

  startSoftAP();
  server.begin();
  Serial.println("Web server ready.");

  while(!connectedToWifi) runSoftAP();

  printDoubleLog("Connected to Wifi", routerSSID.c_str(), 0);

  // Storing the wifi channel
  active_channel = WiFi.channel();

  Serial.println("Wi-Fi connected.");
  Serial.printf("Wi-Fi channel: %d\n", active_channel);

  Serial.print("Master MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  /*
     ESP-NOW initialization
  */

  // Initialize the ESP-NOW module
  esp_err_t result = esp_now_init();

  if (result != ESP_OK) {
    Serial.printf("ESP-NOW initialization failed: %s\n", esp_err_to_name(result));

    while (true) {
      delay(1000);
    }
  }

  // Registering callbacks
  result = esp_now_register_recv_cb(onDataRecv);

  if (result != ESP_OK) {
    Serial.printf("Failed to register callback: %s\n", esp_err_to_name(result)
    );

    while (true) {
      delay(1000);
    }
  }

  Serial.println("ESP-NOW initialized.");
  Serial.println("Receive callback registered.");

  /*
     Add broadcast peer.
     Not needed to receive broadcast,
     But needed to send broadcast.
     Currently unneeded, but who knows what the future holds
  */

  // uint8_t broadcastAddress[] = {
  //   0xFF, 0xFF, 0xFF,
  //   0xFF, 0xFF, 0xFF
  // };

  // esp_now_peer_info_t broadcastPeer = {};

  // memcpy(
  //   broadcastPeer.peer_addr,
  //   broadcastAddress,
  //   6
  // );

  // broadcastPeer.channel = active_channel;
  // broadcastPeer.ifidx = WIFI_IF_STA;
  // broadcastPeer.encrypt = false;

  // result = esp_now_add_peer(&broadcastPeer);

  // if (result != ESP_OK &&
  //     result != ESP_ERR_ESPNOW_EXIST) {
  //   Serial.printf(
  //     "Failed to add broadcast peer: %s\n",
  //     esp_err_to_name(result)
  //   );
  // }

  printSingleLog("Master online", 2000);

  Serial.println("Setup complete.");
}

/* =====================================================
   LOOP
   ===================================================== */

void loop() {
  // reconnect if master suddenly loose wifi connection to router
  // if (WiFi.status() != WL_CONNECTED) {
  //   WiFi.reconnect();
  // }

  static unsigned long lastPolling = 0;
  static unsigned long lastStatus = 0;

  /*
     Print status each interval of STAT_INTERVAL secs
  */

  if (millis() - lastStatus >= STAT_INTERVAL) {
    lastStatus = millis();

    Serial.println("\n=== MASTER STATUS ===");

    Serial.printf("Registered slaves: %d\n", slaves.size());

    Serial.printf("Channel: %d\n", active_channel);
  }

  /*
     Polling each interval of POLL_INTERVAL secs
  */

  if (slaves.size() > 0 && millis() - lastPolling >= POLL_INTERVAL) {
    lastPolling = millis();

    Serial.println("\n=== POLLING SLAVES ===");

    // Poll to each slave
    for (auto &slave : slaves) {
      // create message
      master_request requestMessage = {};

      WiFi.macAddress(requestMessage.senderMacAddr);
      
      requestMessage.msgType = MSG_MASTER_REQUEST;
      
      memcpy(requestMessage.receiverMacAddr, slave.mac, 6);
      
      requestMessage.slaveID = slave.slaveID;
      
      strncpy(requestMessage.slaveType, slave.slaveType.c_str(), sizeof(requestMessage.slaveType) - 1);

      Serial.printf("Requesting Slave #%d (", slave.slaveID);
      printMac(slave.mac);
      Serial.println(")");

      // try to send unicast request
      esp_err_t result = esp_now_send(slave.mac, (uint8_t *)&requestMessage, sizeof(requestMessage));

      Serial.printf("Request status: %s\n", esp_err_to_name(result));

      delay(100);
    }
  }

  delay(10);
}