#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
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
#include <time.h>
#include <ArduinoJson.h>

/* =====================================================
   CONFIGURATION
   ===================================================== */

#define apSSID "I-Surf Setup"
#define apPassword "saya-berselancar"
#define WIFI_INTERFACE WIFI_IF_STA

#define STAT_INTERVAL 10000 // 10 sec; master prints its state
#define POLL_INTERVAL 5000  // 5 sec; master sent polling request to slaves

#define NVS_SLAVE_NAMESPACE "saved_slave"
#define NVS_CRED_NAMESPACE "saved_cred"

#define TRY_CONNECT_WINDOW 15000    // 15 sec; master trying to connect to a network
#define SOFTAP_WINDOW      300000   // 5 min; master running softAP
#define OFFLINE_CHANNEL    1        // sama dengan DEFAULT_SLAVE_CHANNEL di slave

#define WIFI_RET_INTERVAL    120000  // 2 menit antar percobaan retry (state FAILED)
#define WIFI_RET_TRY_WINDOW   10000  // 10 detik per percobaan retry
#define WIFI_RET_LIMIT           10  // percobaan gagal berturut-turut (termasuk saat boot)
#define SOFTAP_RET_WINDOW    300000  // 5 menit ON, 5 menit OFF (state NO_REGISTERED)

#define ENV_PATH            "/.env"
#define COLLECT_TIMEOUT     2000     // ms; batas tunggu balasan sejak request terakhir dikirim
#define CLOUD_HTTP_TIMEOUT  5000     // ms
#define CLOUD_TASK_STACK    12288    // byte; cek dengan log stack di bawah
#define SIGNIN_RETRY_MS     30000    // jeda setelah gagal jaringan / ditolak (401)
#define SIGNIN_BAD_CRED_MS  300000   // jeda setelah HTTP 400 (kredensial atau konfigurasi salah)
#define TOKEN_MARGIN_S      300      // login ulang 5 menit sebelum token kedaluwarsa

/* =====================================================
   MESSAGE TYPES AND SIZE
   ===================================================== */

#define MSG_SEARCH_MASTER   0
#define MSG_MASTER_CONFIRM  1
#define MSG_MASTER_REQUEST  2
#define MSG_SLAVE_READING   3

#define READING_PACKET_MAX  250   // batas payload ESP-NOW
#define READING_HEADER_SIZE 24    // srcAddr(6)+msgType(1)+slaveID(1)+slaveType(16)
#define REPORT_BUFFER_SIZE  (READING_PACKET_MAX - READING_HEADER_SIZE)  // 226

/* =====================================================
   DATA STRUCTURES
   ===================================================== */

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
  uint32_t reqTime;          // master millis() saat polling
} master_request;

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

#define READING_QUEUE_DEPTH 20

// reading queue
struct RxReading {
  uint8_t  srcAddr[6];   // dari info->src_addr, bukan dari isi paket
  uint16_t len;
  slave_reading pkt;
};

// database login
struct CloudConfig {
  String apiKey;
  String dbUrl;
  String email;
  String password;
  bool valid = false;
};
CloudConfig cloudCfg;

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

// =====================================================
// GLOBAL VARIABLES
// =====================================================

// Wifi connection variables

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

bool hardWifiLost = false;
uint8_t noApFailCount = 0;                 
volatile uint8_t lastDisconnectReason = 0; // 0 = belum ada alasan putus
volatile bool authFailSeen = false;
unsigned long wifiRetryLast = 0;     // akhir percobaan terakhir
unsigned long wifiRetryStart = 0;    // awal percobaan yang sedang berjalan
unsigned long softApPhaseStart = 0;  // awal fase ON/OFF SoftAP
bool softApServerStarted = false;
WifiState lastHandledFlag = WifiState::IDLE;


// time synchronization

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 3600;
struct tm timeinfo;


// read queue
QueueHandle_t readingQueue = NULL;


// Agregate report
JsonDocument aggregate_rep;
enum class CycleState : uint8_t { IDLE, COLLECTING };
CycleState cycleState = CycleState::IDLE;
uint32_t cycleReqTime = 0;
unsigned long cycleSentAt = 0;
size_t cycleExpected = 0;      // number of slave when the cycle start

// Queue for task upload: item = char* in heap
QueueHandle_t cloudQueue = NULL;
static WiFiClientSecure dbClient;          // reused between request (keep-alive)
static String cloudToken;
static unsigned long cloudTokenAt = 0;
static unsigned long cloudTokenTtlMs = 0;
static unsigned long cloudLastFail = 0;
static unsigned long cloudRetryDelay = 0;  // 0 = currently not waiting


// I2C module

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
  peerInfo.channel = 0;
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

void enqueueReading(const uint8_t *srcAddr, const uint8_t *data, int len) {
  if (len <= READING_HEADER_SIZE || len > (int)sizeof(slave_reading)) {
    Serial.println("Invalid slave_reading size.");
    return;
  }
  RxReading item;
  memcpy(item.srcAddr, srcAddr, 6);
  item.len = len;
  memset(&item.pkt, 0, sizeof(item.pkt));
  memcpy(&item.pkt, data, len);

  if (xQueueSend(readingQueue, &item, 0) != pdTRUE) {
    Serial.println("Reading queue full. Packet dropped.");
  }
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
  WiFi.macAddress(confirmMessage.srcAddr);
  confirmMessage.msgType = MSG_MASTER_CONFIRM;
  confirmMessage.slaveID = id;

  // read current radio channel, not active channel variable that may be expired
  uint8_t pri;
  wifi_second_chan_t sec;
  esp_wifi_get_channel(&pri, &sec);
  confirmMessage.channel = pri;
  active_channel = pri;

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
  WiFi.softAP(apSSID, apPassword, OFFLINE_CHANNEL);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
}

void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP && wifiFlag == WifiState::TRYING) {
    wifiFlag = WifiState::CONNECTED;
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t reason = info.wifi_sta_disconnected.reason;
    if (reason == WIFI_REASON_ASSOC_LEAVE) return;   // akibat WiFi.disconnect() kita sendiri

    if (wifiFlag == WifiState::TRYING) {
      lastDisconnectReason = reason;
      if (reason == WIFI_REASON_AUTH_FAIL) authFailSeen = true;
    }
    else if (wifiFlag == WifiState::CONNECTED) {
      // koneksi yang sudah berjalan terputus: serahkan ke skema retry FAILED
      Serial.printf("WiFi lost at runtime. Reason: %d\n", reason);
      wifiRetryLast = millis();    // retry pertama setelah WIFI_RET_INTERVAL
      wifiFlag = WifiState::FAILED;
    }
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

void clearAttemptResult() {
  lastDisconnectReason = 0;
  authFailSeen = false;
}

void eraseCredentials() {
  Preferences prefs;
  if (prefs.begin(NVS_CRED_NAMESPACE, false)) {
    prefs.clear();   // hanya namespace saved_cred
    prefs.end();
    Serial.println("Saved credentials erased.");
  }
  routerSSID = "";
  routerPassword = "";
}

void giveUpOnSavedCredentials() {
  hardWifiLost = true;
  eraseCredentials();
  noApFailCount = 0;
  wifiFlag = WifiState::NO_REGISTERED;
  softApPhaseStart = millis() - SOFTAP_RET_WINDOW;   // fase SoftAP ON langsung
}

// Dipanggil setelah percobaan dengan kredensial TERSIMPAN gagal (boot maupun retry).
// Hasil: FAILED (retry berlanjut) atau NO_REGISTERED (NVS dihapus).
void evaluateSavedCredFailure() {
  Serial.printf("Connect failed. Disconnect reason: %d\n", lastDisconnectReason);

  if (authFailSeen) {
    Serial.println("AUTH_FAIL: wrong password.");
    giveUpOnSavedCredentials();
    return;
  }

  if (lastDisconnectReason == WIFI_REASON_NO_AP_FOUND) {
    noApFailCount++;
    Serial.printf("NO_AP_FOUND %d/%d\n", noApFailCount, WIFI_RET_LIMIT);
    if (noApFailCount >= WIFI_RET_LIMIT) {
      giveUpOnSavedCredentials();
      return;
    }
  }

  wifiFlag = WifiState::FAILED;
  wifiRetryLast = millis();
}

// revertToSoftAP = true  : dipanggil dari SoftAP (gagal -> AP dinyalakan lagi)
// revertToSoftAP = false : dipanggil dari kredensial NVS (gagal -> FAILED)
bool connectToTargetWiFi(bool revertToSoftAP) {
  Serial.println("\nConnecting to: " + routerSSID);
  wifiFlag = WifiState::TRYING;
  clearAttemptResult();

  if (revertToSoftAP) WiFi.softAPdisconnect(true);

  if (routerPassword.length() == 0) WiFi.begin(routerSSID.c_str());
  else WiFi.begin(routerSSID.c_str(), routerPassword.c_str());

  unsigned long start = millis();
  while (wifiFlag == WifiState::TRYING && !authFailSeen &&
         millis() - start < TRY_CONNECT_WINDOW) {
    delay(100);
  }

  if (wifiFlag == WifiState::CONNECTED) {
    Serial.println("Connected! IP: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("Failed to connect.");
  WiFi.disconnect(false);

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

void restoreOfflineChannel() {
  esp_wifi_set_channel(OFFLINE_CHANNEL, WIFI_SECOND_CHAN_NONE);
  active_channel = OFFLINE_CHANNEL;
}

void stopSoftAP() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  restoreOfflineChannel();
}

// Dipanggil sekali saat wifiFlag berubah menjadi CONNECTED di loop()
void onRuntimeConnected() {
  active_channel = WiFi.channel();
  noApFailCount = 0;
  hardWifiLost = false;
  WiFi.setAutoReconnect(true);              // samakan dengan jalur boot online
  saveCredentials(routerSSID, routerPassword);  // no-op jika sama dengan yang tersimpan
  Serial.printf("WiFi connected at runtime. Channel: %d\n", active_channel);
}

void handleWifiRetry() {
  switch (wifiFlag) {

    // --- FAILED: retry ke kredensial tersimpan ---
    case WifiState::FAILED:
      if (millis() - wifiRetryLast < WIFI_RET_INTERVAL) break;

      Serial.printf("WiFi retry to %s (NO_AP_FOUND %d/%d)\n",
                    routerSSID.c_str(), noApFailCount, WIFI_RET_LIMIT);
      clearAttemptResult();
      wifiFlag = WifiState::TRYING;
      wifiRetryStart = millis();
      if (routerPassword.length() == 0) WiFi.begin(routerSSID.c_str());
      else WiFi.begin(routerSSID.c_str(), routerPassword.c_str());
      break;

    // --- TRYING: menunggu event GOT_IP atau timeout ---
    case WifiState::TRYING:
      // percobaan berakhir jika ada alasan putus atau timeout
      if (lastDisconnectReason == 0 &&
          millis() - wifiRetryStart < WIFI_RET_TRY_WINDOW) break;
      if (wifiFlag != WifiState::TRYING) break;   // event GOT_IP masuk di sela-sela

      evaluateSavedCredFailure();   // set flag dulu, supaya event putus berikutnya diabaikan
      WiFi.disconnect(false);
      restoreOfflineChannel();
      break;

    // --- NO_REGISTERED: SoftAP sedang OFF ---
    case WifiState::NO_REGISTERED:
      if (millis() - softApPhaseStart < SOFTAP_RET_WINDOW) break;

      if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed. Skipping SoftAP phase.");
        softApPhaseStart = millis();
        break;
      }
      startSoftAP();
      if (!softApServerStarted) {
        server.begin();
        softApServerStarted = true;
      }
      wifiFlag = WifiState::RUNNING_SOFTAP;
      softApPhaseStart = millis();
      Serial.println("SoftAP phase: ON");
      break;

    // --- RUNNING_SOFTAP: SoftAP sedang ON ---
    case WifiState::RUNNING_SOFTAP:
      runSoftAP();
      if (wifiFlag != WifiState::RUNNING_SOFTAP) break;   // runSoftAP berhasil connect

      if (WiFi.softAPgetStationNum() > 0) {
        softApPhaseStart = millis();                      // perpanjang selama ada klien
      } else if (millis() - softApPhaseStart >= SOFTAP_RET_WINDOW) {
        stopSoftAP();
        wifiFlag = WifiState::NO_REGISTERED;
        softApPhaseStart = millis();
        Serial.println("SoftAP phase: OFF");
      }
      break;

    default:
      break;
  }

  if (wifiFlag == WifiState::CONNECTED && lastHandledFlag != WifiState::CONNECTED) {
    onRuntimeConnected();
  }
  lastHandledFlag = wifiFlag;
}

void setupWifi() {
  WiFi.onEvent(onWiFiEvent);

  // 1. Ada kredensial tersimpan -> coba connect
  String savedSSID, savedPass;
  if (loadCredentials(savedSSID, savedPass)) {
    routerSSID = savedSSID;
    routerPassword = savedPass;
    connectToTargetWiFi(false);   // hasil: CONNECTED atau FAILED
    if (wifiFlag == WifiState::FAILED) {   // kegagalan saat boot dihitung sebagai percobaan ke-1
      evaluateSavedCredFailure();
    }
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
  softApServerStarted = true;
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

void onRecvReading(RxReading &rItem) {
  int regID = findSlaveID(rItem.srcAddr);
  if (regID < 0) {
    Serial.println("Reading from unregistered MAC. Dropped.");
    return;
  }

  // jangan percaya terminator dari pengirim
  rItem.pkt.slaveType[sizeof(rItem.pkt.slaveType) - 1] = '\0';
  rItem.pkt.reportBuffer[sizeof(rItem.pkt.reportBuffer) - 1] = '\0';

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, rItem.pkt.reportBuffer);
  if (err) {
    Serial.printf("Invalid report JSON: %s\n", err.c_str());
    return;
  }
  JsonObject obj = doc.as<JsonObject>();

  Serial.println("\n==============================");
  Serial.println("SLAVE READING RECEIVED");
  Serial.println("==============================");
  Serial.printf("Slave type: %s\n", rItem.pkt.slaveType);
  Serial.printf("Slave ID: %d\n", rItem.pkt.slaveID);

  for (JsonPair kv : obj) {
    Serial.print(kv.key().c_str());
    Serial.print(": ");
    serializeJson(kv.value(), Serial);
    Serial.println();
  }

  collectReading(regID, obj);

  char line[17];
  snprintf(line, sizeof(line), "Slave #%d", rItem.pkt.slaveID);
  printDoubleLog("Received from:", line);   // tanpa delay
}

void processReadingQueue() {
  RxReading item;
  while (xQueueReceive(readingQueue, &item, 0) == pdTRUE) {
    onRecvReading(item);
  }
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

    case MSG_SLAVE_READING:
      enqueueReading(info->src_addr, incomingData, len);
      break;

    default:
      Serial.println("Unknown message type.");
      break;
  }
}

// =====================================================
// CLOUD (RTDB lewat REST)
// =====================================================

bool loadCloudConfig() {
  cloudCfg = CloudConfig();

  File f = LittleFS.open(ENV_PATH, "r");
  if (!f) {
    Serial.println("Cloud: " ENV_PATH " not found. Upload disabled.");
    return false;
  }

  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (first) {                          // buang BOM UTF-8 jika ada
      if (line.startsWith("\xEF\xBB\xBF")) line.remove(0, 3);
      first = false;
    }
    line.trim();                          // buang \r (CRLF Windows) dan spasi
    if (line.length() == 0 || line[0] == '#') continue;

    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq);   key.trim();
    String val = line.substring(eq + 1);  val.trim();
    if (val.length() >= 2 && val[0] == '"' && val[val.length() - 1] == '"') {
      val = val.substring(1, val.length() - 1);
    }

    if (key == "FIREBASE_API_KEY")          cloudCfg.apiKey = val;
    else if (key == "FIREBASE_DATABASE_URL") cloudCfg.dbUrl = val;
    else if (key == "DEVICE_EMAIL")          cloudCfg.email = val;
    else if (key == "DEVICE_PASSWORD")       cloudCfg.password = val;
  }
  f.close();

  while (cloudCfg.dbUrl.endsWith("/")) cloudCfg.dbUrl.remove(cloudCfg.dbUrl.length() - 1);

  cloudCfg.valid = cloudCfg.apiKey.length() > 0 &&
                   cloudCfg.dbUrl.startsWith("https://") &&
                   cloudCfg.email.length() > 0 &&
                   cloudCfg.password.length() > 0;

  Serial.printf("Cloud: config %s\n", cloudCfg.valid ? "loaded" : "INCOMPLETE. Upload disabled.");
  return cloudCfg.valid;
}

// Login email/password -> idToken. Backoff mencegah login berulang saat kredensial salah.
bool cloudSignIn() {
  if (cloudRetryDelay != 0 && millis() - cloudLastFail < cloudRetryDelay) return false;

  WiFiClientSecure client;
  client.setInsecure();   // TIDAK memverifikasi sertifikat server (keputusan sementara)
  HTTPClient http;
  http.setConnectTimeout(CLOUD_HTTP_TIMEOUT);
  http.setTimeout(CLOUD_HTTP_TIMEOUT);

  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=")
               + cloudCfg.apiKey;
  if (!http.begin(client, url)) {
    cloudLastFail = millis();
    cloudRetryDelay = SIGNIN_RETRY_MS;
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  JsonDocument req;
  req["email"] = cloudCfg.email;
  req["password"] = cloudCfg.password;
  req["returnSecureToken"] = true;
  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : String();
  http.end();

  JsonDocument res;
  if (code == 200 && !deserializeJson(res, resp)) {
    const char *tok = res["idToken"] | "";
    long expiresIn = String(res["expiresIn"] | "3600").toInt();
    if (tok[0] != '\0') {
      cloudToken = tok;
      long ttl = expiresIn - TOKEN_MARGIN_S;
      if (ttl < 60) ttl = 60;
      cloudTokenTtlMs = (unsigned long)ttl * 1000UL;
      cloudTokenAt = millis();
      cloudRetryDelay = 0;
      Serial.println("Cloud: signed in.");
      return true;
    }
  }

  res.clear();
  deserializeJson(res, resp);
  Serial.printf("Cloud: sign-in failed (HTTP %d): %s\n", code, (const char *)(res["error"]["message"] | "-"));
  cloudLastFail = millis();
  cloudRetryDelay = (code == 400) ? SIGNIN_BAD_CRED_MS : SIGNIN_RETRY_MS;
  return false;
}

// PATCH /latest.json. true jika berhasil.
bool cloudPatchLatest(const char *payload) {
  bool tokenOk = cloudToken.length() > 0 && (millis() - cloudTokenAt) < cloudTokenTtlMs;
  if (!tokenOk && !cloudSignIn()) return false;

  HTTPClient http;
  http.setConnectTimeout(CLOUD_HTTP_TIMEOUT);
  http.setTimeout(CLOUD_HTTP_TIMEOUT);

  String url = cloudCfg.dbUrl + "/latest.json?print=silent&auth=" + cloudToken;
  if (!http.begin(dbClient, url)) {
    dbClient.stop();
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  int code = http.PATCH((uint8_t *)payload, strlen(payload));
  if (code == 200 || code == 204) {
    http.end();
    Serial.printf("Cloud: PATCH latest succeeded");
    return true;
  }

  String err = (code > 0) ? http.getString() : String();
  http.end();
  Serial.printf("Cloud: PATCH latest failed (HTTP %d) %s\n", code, err.c_str());

  if (code < 0) dbClient.stop();   // koneksi rusak -> dibuka ulang di request berikut
  if (code == 401) {
    // 401 bisa berarti token kedaluwarsa ATAU ditolak rules; jangan login tiap siklus
    cloudToken = "";
    cloudLastFail = millis();
    cloudRetryDelay = SIGNIN_RETRY_MS;
  }
  return false;
}

void cloudTask(void *) {
  dbClient.setInsecure();   // TIDAK memverifikasi sertifikat server (keputusan sementara)
  bool reported = false;

  for (;;) {
    char *payload = nullptr;
    if (xQueueReceive(cloudQueue, &payload, portMAX_DELAY) != pdTRUE || payload == nullptr) continue;

    if (wifiFlag == WifiState::CONNECTED && cloudPatchLatest(payload) && !reported) {
      reported = true;
      Serial.printf("Cloud: first upload OK. Task stack free: %u bytes\n",
                    (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    free(payload);
  }
}

// =====================================================
// AGREGATE ONE POLLING CYCLE
// =====================================================

// slaveID registered for this mac
int findSlaveID(const uint8_t *mac) {
  for (const auto &s : slaves) {
    if (memcmp(s.mac, mac, 6) == 0) return s.slaveID;
  }
  return -1;
}

void startCycle(uint32_t reqTime) {
  cycleReqTime = reqTime;
  cycleExpected = slaves.size();
  aggregate_rep.clear();
  aggregate_rep["timestamp_req"] = reqTime;
  aggregate_rep["slaves"].to<JsonObject>();
}

// called from onRecvReading() (context loop)
void collectReading(int id, JsonObject obj) {
  if (cycleState != CycleState::COLLECTING) {
    Serial.println("Reading outside collect window. Ignored.");
    return;
  }
  if ((uint32_t)(obj["reqTime"] | 0u) != cycleReqTime) {
    Serial.println("Reading from an older cycle. Ignored.");
    return;
  }

  String key = "slave" + String(id);
  JsonObject dst = aggregate_rep["slaves"][key].to<JsonObject>();
  for (JsonPair kv : obj) {
    const char *k = kv.key().c_str();
    if (strcmp(k, "reqTime") == 0 || strcmp(k, "slaveID") == 0) continue;   // already in the agregated report
    dst[kv.key()] = kv.value();
  }
}

// Transform aggregate_rep into PATCH body for /latest, then hand it over to the upload task
void queueLatestUpload() {
  JsonDocument patch;
  JsonObject slavesObj = aggregate_rep["slaves"].as<JsonObject>();

  for (JsonPair kv : slavesObj) {
    String key = kv.key().c_str();
    JsonObject s = kv.value().as<JsonObject>();

    if (s["unreachable"] | false) {
      // only touch the flag; last value and old updatedAt remain unchanged
      patch[key + "/unreachable"] = true;
    } else {
      // change this slave node as a whole
      patch[key] = s;
      patch[key]["unreachable"] = false;
      patch[key]["updatedAt"][".sv"] = "timestamp";   // server timestamp (ms epoch)
    }
  }

  size_t n = measureJson(patch);
  char *buf = (char *)malloc(n + 1);
  if (buf == nullptr) {
    Serial.println("Cloud: out of memory. Upload skipped.");
    return;
  }
  serializeJson(patch, buf, n + 1);

  // long queue 1: if the old content not yet taken, remove then overwrite it
  for (int attempt = 0; attempt < 2; attempt++) {
    if (xQueueSend(cloudQueue, &buf, 0) == pdTRUE) return;
    char *old = nullptr;
    if (xQueueReceive(cloudQueue, &old, 0) == pdTRUE) free(old);
  }
  free(buf);
}

void finalizeCycle() {
  JsonObject slavesObj = aggregate_rep["slaves"].as<JsonObject>();

  // registered slave that didnt reply -> flag it as unreachable
  for (size_t i = 0; i < cycleExpected && i < slaves.size(); i++) {
    String key = "slave" + String(slaves[i].slaveID);
    if (!slavesObj[key].is<JsonObject>()) {
      slavesObj[key]["unreachable"] = true;
      Serial.printf("Slave #%d did not reply: unreachable.\n", slaves[i].slaveID);
    }
  }

  cycleState = CycleState::IDLE;

  if (cloudQueue == NULL) return;   // cloud inactive (incomplete config)
  if (wifiFlag == WifiState::CONNECTED) {
    queueLatestUpload();
  } else {
    Serial.println("Offline: aggregate not uploaded.");
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

  // false = dont automatically format if mount failed
  if (LittleFS.begin(false)) {
    loadCloudConfig();
  } else {
    Serial.println("LittleFS mount failed. Cloud config not loaded.");
  }

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
  lastHandledFlag = wifiFlag;

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

  // Creating slave reading queue
  readingQueue = xQueueCreate(READING_QUEUE_DEPTH, sizeof(RxReading));
  if (readingQueue == NULL) {
    Serial.println("Failed to create reading queue.");
    while (true) delay(1000);
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

  if (cloudCfg.valid) {
    cloudQueue = xQueueCreate(1, sizeof(char *));
    if (cloudQueue == NULL ||
        xTaskCreate(cloudTask, "cloud", CLOUD_TASK_STACK, NULL, 1, NULL) != pdPASS) {
      Serial.println("Failed to start cloud task. Upload disabled.");
      cloudQueue = NULL;
    }
  }

  printSingleLog("Master ready", 2000);

  Serial.println("Setup complete.");
}

/* =====================================================
   LOOP
   ===================================================== */

void loop() {
  processReadingQueue();

  static unsigned long lastPolling = 0;
  static unsigned long lastStatus = 0;

  /*
     Print status each interval of STAT_INTERVAL secs
  */

  if (millis() - lastStatus >= STAT_INTERVAL) {
    lastStatus = millis();

    Serial.println("\n=== MASTER STATUS ===");
    if (wifiFlag == WifiState::CONNECTED && getLocalTime(&timeinfo, 0)) {
      Serial.println(&timeinfo, "Timestamp: %A, %B %d %Y %H:%M:%S");
    }

    Serial.printf("Registered slaves: %d\n", slaves.size());

    Serial.printf("Mode: %s\n", wifiFlag == WifiState::CONNECTED ? "ONLINE" : "OFFLINE");
    
    Serial.printf("WiFi state: %s\n", wifiStateName(wifiFlag));

    Serial.printf("WiFi retry: %d/%d, hardWifiLost: %s\n",
              noApFailCount, WIFI_RET_LIMIT, hardWifiLost ? "YES" : "NO");

    Serial.printf("Active channel: %d\n", active_channel);

    Serial.printf("Free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());
  }

  /*
     Start a new cycle only if the previous cycle has finished
  */

  if (cycleState == CycleState::IDLE && slaves.size() > 0 &&
      millis() - lastPolling >= POLL_INTERVAL) {
    lastPolling = millis();

    Serial.println("\n=== POLLING SLAVES ===");
    if (wifiFlag == WifiState::CONNECTED && getLocalTime(&timeinfo, 0)) {
      Serial.println(&timeinfo, "Timestamp: %A, %B %d %Y %H:%M:%S");
    }

    startCycle((uint32_t)lastPolling);

    // Poll to each slave
    for (auto &slave : slaves) {
      // create message
      master_request requestMessage = {};
      WiFi.macAddress(requestMessage.srcAddr);
      requestMessage.msgType = MSG_MASTER_REQUEST;
      memcpy(requestMessage.dstAddr, slave.mac, 6);
      requestMessage.slaveID = slave.slaveID;
      strncpy(requestMessage.slaveType, slave.slaveType.c_str(), sizeof(requestMessage.slaveType) - 1);
      requestMessage.reqTime = (uint32_t)lastPolling;   // satu timestamp per siklus polling

      Serial.printf("Requesting Slave #%d (", slave.slaveID);
      printMac(slave.mac);
      Serial.println(")");

      // try to send unicast request
      esp_err_t result = esp_now_send(slave.mac, (uint8_t *)&requestMessage, sizeof(requestMessage));

      Serial.printf("Request status: %s\n", esp_err_to_name(result));

      delay(100);
    }

    cycleSentAt = millis();
    cycleState = CycleState::COLLECTING;
  }

  /*
     Finish cycle if all slave replied or timeout reached
  */
  
  if (cycleState == CycleState::COLLECTING) {
    JsonObject so = aggregate_rep["slaves"].as<JsonObject>();
    if (so.size() >= cycleExpected || millis() - cycleSentAt >= COLLECT_TIMEOUT) {
      finalizeCycle();
    }
  }

  handleWifiRetry();

  delay(10);
}