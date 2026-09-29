#include <WiFi.h>
#include <LittleFS.h>

const char* apSSID = "I-Surf Setup";
const char* apPassword = "saya-berselancar";

WiFiServer server(80);

String targetSSID = "";
String targetPassword = "";

// =====================================================
// UTILITY FUNCTIONS
// =====================================================

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

// =====================================================
// FILE RESPONSE HELPERS
// =====================================================

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
  Serial.println("\nConnecting to: " + targetSSID);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  if (targetPassword.length() == 0) WiFi.begin(targetSSID.c_str());
  else WiFi.begin(targetSSID.c_str(), targetPassword.c_str());

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

// =====================================================
// SETUP & LOOP
// =====================================================

void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    return;
  }

  startSoftAP();
  server.begin();
  Serial.println("Web server ready.");
}

void loop() {
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
    targetSSID = getPostValue(body, "ssid");
    targetPassword = getPostValue(body, "password");

    client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\nConnecting...");
    client.flush();
    delay(100);
    client.stop();

    connectToTargetWiFi();
  } 
  else if (requestLine.startsWith("GET / ") || requestLine.startsWith("GET /scan")) {
    renderIndexPage(client);
  } 
  else {
    client.println("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n404");
  }

  if (client.connected()) client.stop();
}