#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <LiquidCrystal_I2C.h>
#include <string.h>
#include <vector>
#include <LittleFS.h>
#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>

/* =====================================================
   CONFIGURATION
   ===================================================== */
#define apSSID "I-Surf Setup"
#define apPassword "saya-berselancar"
#define WIFI_INTERFACE WIFI_IF_STA
#define STAT_INTERVAL 10000 // 10 sec
#define POLL_INTERVAL 5000  // 5 sec
#define NVS_SLAVE_NAMESPACE "saved_slave"
#define NVS_CRED_NAMESPACE "saved_cred"
#define TRY_CONNECT_WINDOW 15000    // 15 sec
#define SOFTAP_WINDOW      300000   // 5 min
#define OFFLINE_CHANNEL    1        // sama dengan DEFAULT_SLAVE_CHANNEL di slave

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
  uint8_t slaveID;                
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
  uint8_t slaveID;
  String slaveType;
};

// because nvs cannot be multilevel, so it is stored as
// key:value --> slaveID(string):macAddr(string)
struct slaveID_to_macAddr {
  uint8_t slaveID;
  uint8_t macAddr[6];
};

std::vector<SlaveInfo> slaves;
std::vector<slaveID_to_macAddr> savedSlaves;

/* =====================================================
   GLOBAL VARIABLES
   ===================================================== */

WiFiServer server(80);
String routerSSID = "";
String routerPassword = "";

uint8_t active_channel = 0;

enum class WifiState : uint8_t {
  IDLE,
  TRYING,
  RUNNING_SOFTAP,
  CONNECTED,
  FAILED,          // gagal connect ke kredensial tersimpan
  NO_REGISTERED    // softAP_window habis / tidak ada kredensial
};

// volatile: ditulis dari event task WiFi, dibaca dari loop task
volatile WifiState wifiFlag = WifiState::IDLE;

const char* wifiStateName(WifiState s) {
  switch (s) {
    case WifiState::IDLE:           return "IDLE";
    case WifiState::TRYING:         return "TRYING";
    case WifiState::RUNNING_SOFTAP: return "RUNNING_SOFTAP";
    case WifiState::CONNECTED:      return "CONNECTED";
    case WifiState::FAILED:         return "FAILED";
    case WifiState::NO_REGISTERED:  return "NO_REGISTERED";
  }
  return "?";
}

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
// NVS SAVED SLAVES
// =====================================================

String macToString(const uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool stringToMac(const String &str, uint8_t *mac) {
  unsigned int b[6];
  if (sscanf(str.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x",
             &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
  return true;
}

// Load all saved slaves from NVS into savedSlaves
void exportSavedSlaves() {
  savedSlaves.clear();

  Preferences prefs;
  // read-write so the namespace is created on first boot
  if (!prefs.begin(NVS_SLAVE_NAMESPACE, false)) {
    Serial.println("Failed to open NVS namespace.");
    return;
  }

  nvs_iterator_t it = NULL;
  esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_SLAVE_NAMESPACE, NVS_TYPE_STR, &it);

  while (err == ESP_OK && it != NULL) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);

    int id = atoi(info.key);
    String macStr = prefs.getString(info.key, "");

    slaveID_to_macAddr entry = {};
    if (id >= 1 && id <= 255 && stringToMac(macStr, entry.macAddr)) {
      entry.slaveID = (uint8_t)id;
      savedSlaves.push_back(entry);
      Serial.printf("Loaded saved slave #%d: %s\n", id, macStr.c_str());
    } else {
      Serial.printf("Skipping invalid NVS entry: key=%s\n", info.key);
    }

    err = nvs_entry_next(&it);   // sets it = NULL when no more entries
  }

  if (it) nvs_release_iterator(it);
  prefs.end();
}

// Returns saved slaveID for this MAC, or -1 if not saved
int isSlaveSaved(const uint8_t *mac) {
  for (const auto &s : savedSlaves) {
    if (memcmp(s.macAddr, mac, 6) == 0) return s.slaveID;
  }
  return -1;
}

// max(ID) + 1, or -1 if the ID space (uint8_t) is full
int getNextSlaveID() {
  int maxID = 0;
  for (const auto &s : savedSlaves) {
    if (s.slaveID > maxID) maxID = s.slaveID;
  }
  return (maxID >= 255) ? -1 : maxID + 1;
}

// Persist a new slave to NVS and to savedSlaves
bool saveSlave(uint8_t id, const uint8_t *mac) {
  Preferences prefs;
  if (!prefs.begin(NVS_SLAVE_NAMESPACE, false)) return false;

  char key[4];
  snprintf(key, sizeof(key), "%u", id);

  size_t written = prefs.putString(key, macToString(mac));
  prefs.end();

  if (written == 0) return false;

  slaveID_to_macAddr entry = {};
  entry.slaveID = id;
  memcpy(entry.macAddr, mac, 6);
  savedSlaves.push_back(entry);
  return true;
}

// Send master_confirm to a slave
void sendConfirm(const uint8_t *mac, uint8_t id) {
  master_confirm confirmMessage = {};
  WiFi.macAddress(confirmMessage.senderMacAddr);
  confirmMessage.msgType = MSG_MASTER_CONFIRM;
  confirmMessage.slaveID = id;

  esp_err_t result = esp_now_send(mac, (uint8_t *)&confirmMessage, sizeof(confirmMessage));
  Serial.printf("Confirmation status: %s\n", esp_err_to_name(result));
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
      listHtml += "</a></div>";
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

void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP && wifiFlag == WifiState::TRYING) {
    wifiFlag = WifiState::CONNECTED;
  }
}

// true jika ada SSID tersimpan. SSID kosong = belum ada kredensial.
bool loadCredentials(String &ssid, String &pass) {
  Preferences prefs;
  if (!prefs.begin(NVS_CRED_NAMESPACE, false)) {
    Serial.println("Failed to open credential NVS.");
    return false;
  }
  ssid = prefs.getString("saved_SSID", "");
  pass = prefs.getString("saved_password", "");
  prefs.end();
  return ssid.length() > 0;
}

bool saveCredentials(const String &ssid, const String &pass) {
  Preferences prefs;
  if (!prefs.begin(NVS_CRED_NAMESPACE, false)) return false;

  // hindari tulis flash jika tidak berubah
  if (prefs.getString("saved_SSID", "") != ssid ||
      prefs.getString("saved_password", "") != pass) {
    prefs.putString("saved_SSID", ssid);
    prefs.putString("saved_password", pass);
  }

  // verifikasi lewat read-back, bukan return value putString
  bool ok = (prefs.getString("saved_SSID", "") == ssid) &&
            (prefs.getString("saved_password", "") == pass);
  prefs.end();

  Serial.println(ok ? "Credentials saved." : "Failed to save credentials.");
  return ok;
}

// revertToSoftAP = true  : dipanggil dari SoftAP (gagal -> AP dinyalakan lagi)
// revertToSoftAP = false : dipanggil dari kredensial NVS (gagal -> FAILED)
bool connectToTargetWiFi(bool revertToSoftAP) {
  Serial.println("\nConnecting to: " + routerSSID);
  wifiFlag = WifiState::TRYING;

  if (revertToSoftAP) WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  if (routerPassword.length() == 0) WiFi.begin(routerSSID.c_str());
  else WiFi.begin(routerSSID.c_str(), routerPassword.c_str());

  unsigned long start = millis();
  while (wifiFlag == WifiState::TRYING && millis() - start < TRY_CONNECT_WINDOW) {
    delay(100);
  }

  if (wifiFlag == WifiState::CONNECTED) {
    Serial.println("Connected! IP: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("Failed to connect.");
  WiFi.disconnect(true);

  if (revertToSoftAP) {
    Serial.println("Reverting to SoftAP...");
    startSoftAP();
    wifiFlag = WifiState::RUNNING_SOFTAP;
  } else {
    wifiFlag = WifiState::FAILED;
  }
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

    connectToTargetWiFi(true);
  } 
  else if (requestLine.startsWith("GET / ") || requestLine.startsWith("GET /scan")) {
    renderIndexPage(client);
  } 
  else {
    client.println("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n404");
  }

  if (client.connected()) client.stop();
}

void setupWifi() {
  WiFi.onEvent(onWiFiEvent);

  // 1. Ada kredensial tersimpan -> coba connect
  String savedSSID, savedPass;
  if (loadCredentials(savedSSID, savedPass)) {
    routerSSID = savedSSID;
    routerPassword = savedPass;
    connectToTargetWiFi(false);   // hasil: CONNECTED atau FAILED
    return;
  }

  // 2. Tidak ada -> SoftAP
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    wifiFlag = WifiState::NO_REGISTERED;
    return;
  }

  wifiFlag = WifiState::RUNNING_SOFTAP;
  startSoftAP();
  server.begin();
  Serial.println("Web server ready.");

  unsigned long softAPStart = millis();
  while (wifiFlag == WifiState::RUNNING_SOFTAP &&
         millis() - softAPStart < SOFTAP_WINDOW) {
    runSoftAP();
    // perpanjang window selama ada klien terhubung ke AP
    if (WiFi.softAPgetStationNum() > 0) softAPStart = millis();
  }

  if (wifiFlag == WifiState::CONNECTED) {
    saveCredentials(routerSSID, routerPassword);
  } else {
    wifiFlag = WifiState::NO_REGISTERED;
  }
}

// Mode offline: tanpa router, ESP-NOW tetap jalan di channel tetap
void goOffline() {
  WiFi.softAPdisconnect(true);
  WiFi.setAutoReconnect(false);   // cegah radio pindah channel diam-diam
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  esp_wifi_set_channel(OFFLINE_CHANNEL, WIFI_SECOND_CHAN_NONE);
  active_channel = OFFLINE_CHANNEL;
  Serial.printf("Offline mode, ESP-NOW channel: %d\n", active_channel);
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

  // Already registered in RAM (e.g. slave lost master and re-broadcasted):
  // resend confirmation with the same ID, otherwise the slave waits forever
  for (const auto &s : slaves) {
    if (memcmp(s.mac, info->src_addr, 6) == 0) {
      Serial.println("Slave already registered. Resending confirmation.");
      sendConfirm(s.mac, s.slaveID);
      return;
    }
  }

  // Check NVS list: reuse the saved ID, or allocate a new one
  int savedID = isSlaveSaved(info->src_addr);
  bool isNewSlave = (savedID < 0);
  int assignedID = savedID;

  if (isNewSlave) {
    assignedID = getNextSlaveID();
    if (assignedID < 0) {
      Serial.println("Slave ID space is full.");
      return;
    }
  }

  // Try to add slave as a peer
  if (!addSlavePeer(info->src_addr)) {
    Serial.println("Could not register Slave.");
    return;
  }

  // New slave: persist to NVS (only after the peer was added successfully)
  if (isNewSlave && !saveSlave((uint8_t)assignedID, info->src_addr)) {
    Serial.println("Failed to save slave to NVS.");
    return;
  }

  // Store the information of the slave in RAM
  SlaveInfo newSlave = {};
  memcpy(newSlave.mac, info->src_addr, 6);
  newSlave.slaveID = (uint8_t)assignedID;
  newSlave.slaveType = String(searchMessage.slaveType);
  slaves.push_back(newSlave);

  Serial.printf("Registered Slave #%d (%s)\n", newSlave.slaveID, isNewSlave ? "new" : "saved");
  sendConfirm(newSlave.mac, newSlave.slaveID);
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

  setupWifi();

  if (wifiFlag == WifiState::CONNECTED) {
    printDoubleLog("Connected to Wifi", routerSSID.c_str(), 0);
    active_channel = WiFi.channel();
    Serial.printf("Wi-Fi channel: %d\n", active_channel);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    goOffline();
    printDoubleLog("Offline mode", wifiStateName(wifiFlag), 0);
  }

  Serial.print("Master MAC: ");
  Serial.println(WiFi.macAddress());

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

  slaves.reserve(8);        // avoid reallocation while loop() iterates (see note 2)
  savedSlaves.reserve(8);

  exportSavedSlaves();
  Serial.printf("Saved slaves in NVS: %d\n", savedSlaves.size());

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

  printSingleLog("Master ready", 2000);

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

    Serial.printf("Mode: %s\n", wifiFlag == WifiState::CONNECTED ? "ONLINE" : "OFFLINE");
    
    Serial.printf("WiFi state: %s\n", wifiStateName(wifiFlag));

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