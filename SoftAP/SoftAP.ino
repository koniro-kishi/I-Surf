#include <WiFi.h>
#include <LittleFS.h>

const char* apSSID = "I-Surf Setup";
const char* apPassword = "saya-berselancar";

WiFiServer server(80);

// =====================================================
// WIFI CREDENTIALS
// =====================================================

String targetSSID = "";
String targetPassword = "";


// =====================================================
// SCAN WIFI
// =====================================================

int scanWiFi() {
  Serial.println("Starting WiFi scan...");

  WiFi.scanDelete();

  int n_networks = WiFi.scanNetworks();

  Serial.println("Scan done");

  if (n_networks == 0) {
    Serial.println("No networks found");
  } else {
    Serial.printf("%d networks found\n", n_networks);

    for (int i = 0; i < n_networks; i++) {
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.print(" dBm)");

      Serial.print(
        WiFi.encryptionType(i) == WIFI_AUTH_OPEN
          ? " OPEN"
          : " LOCKED"
      );

      Serial.println();
    }
  }

  return n_networks;
}


// =====================================================
// WIFI ICON BASED ON RSSI
// =====================================================

String getWiFiIcon(int rssi) {
  if (rssi > -50) {
    return "/wifi-strong.png";
  } else if (rssi > -60) {
    return "/wifi-good.png";
  } else if (rssi > -70) {
    return "/wifi-fair.png";
  } else {
    return "/wifi-weak.png";
  }
}


// =====================================================
// URL DECODE
// =====================================================

String urlDecode(const String& input) {
  String decoded;

  for (int i = 0; i < input.length(); i++) {
    if (input[i] == '%') {
      if (i + 2 < input.length()) {
        String hex = input.substring(i + 1, i + 3);

        char c = (char)strtol(
          hex.c_str(),
          NULL,
          16
        );

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


// =====================================================
// GET QUERY PARAMETER FROM URL
// =====================================================

String getQueryParam(const String& url, const String& param) {
  String search = param + "=";
  int start = url.indexOf(search);
  if (start < 0) return "";
  
  start += search.length();
  int end = url.indexOf('&', start);
  if (end < 0) {
    end = url.indexOf(' ', start); // Request line ends with space
  }
  if (end < 0) {
    end = url.length();
  }
  
  return urlDecode(url.substring(start, end));
}


// =====================================================
// GET VALUE FROM POST BODY
// =====================================================

String getPostValue(
  const String& body,
  const String& key
) {
  String searchKey = key + "=";

  int start = body.indexOf(searchKey);

  if (start < 0) {
    return "";
  }

  start += searchKey.length();

  int end = body.indexOf('&', start);

  if (end < 0) {
    end = body.length();
  }

  return urlDecode(
    body.substring(start, end)
  );
}


// =====================================================
// HTML ESCAPE
// =====================================================

String htmlEscape(const String& input) {
  String output;
  output.reserve(input.length() + 16);

  for (int i = 0; i < input.length(); i++) {
    switch (input[i]) {
      case '&':
        output += "&amp;";
        break;

      case '<':
        output += "&lt;";
        break;

      case '>':
        output += "&gt;";
        break;

      case '"':
        output += "&quot;";
        break;

      case '\'':
        output += "&#39;";
        break;

      default:
        output += input[i];
        break;
    }
  }

  return output;
}


// =====================================================
// READ EXACT HTTP BODY
// =====================================================

bool readRequestBody(
  WiFiClient& client,
  int contentLength,
  String& body
) {
  body = "";

  if (contentLength <= 0) {
    return true;
  }

  body.reserve(contentLength);

  unsigned long startTime = millis();

  while (
    body.length() < contentLength &&
    millis() - startTime < 3000
  ) {
    while (
      client.available() &&
      body.length() < contentLength
    ) {
      body += (char)client.read();
    }

    if (body.length() < contentLength) {
      delay(1);
    }
  }

  return body.length() == contentLength;
}


// =====================================================
// SEND LITTLEFS FILE
// =====================================================

void sendFile(
  WiFiClient& client,
  const char* path,
  const char* contentType
) {
  File file = LittleFS.open(path, "r");

  if (!file) {
    Serial.print("File not found: ");
    Serial.println(path);

    client.println("HTTP/1.1 404 Not Found");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.println("404 Not Found");
    return;
  }

  client.println("HTTP/1.1 200 OK");

  client.print("Content-Type: ");
  client.println(contentType);

  client.print("Content-Length: ");
  client.println(file.size());

  client.println("Cache-Control: public, max-age=3600");
  client.println("Connection: close");
  client.println();

  uint8_t buffer[1024];

  while (file.available()) {
    size_t bytesRead = file.read(
      buffer,
      sizeof(buffer)
    );

    if (bytesRead > 0) {
      client.write(buffer, bytesRead);
    }
  }

  file.close();
}


// =====================================================
// SEND MAIN WEB PAGE (LIST OF NETWORKS)
// =====================================================

void sendWebPage(
  WiFiClient& client,
  int n_networks
) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Cache-Control: no-cache, no-store, must-revalidate");
  client.println("Pragma: no-cache");
  client.println("Expires: 0");
  client.println("Connection: close");
  client.println();

  client.println("<!DOCTYPE html>");
  client.println("<html>");
  client.println("<head>");
  client.println("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
  client.println("<meta charset=\"UTF-8\">");
  client.println("<meta name=\"color-scheme\" content=\"light\">");
  client.println("<title>I-Surf Setup</title>");

  client.println("<style>");
  client.println("html,body{margin:0;padding:0;font-family:Arial,Helvetica,sans-serif;background:#fff;color:#111;}");
  client.println("body{padding:30px 24px;}");
  client.println(".header{display:flex;align-items:center;justify-space-between;margin-bottom:25px;}");
  client.println(".title{font-size:34px;font-weight:600;color:#666;margin:0;}");
  client.println(".refresh-button{display:flex;align-items:center;justify-content:center;width:42px;height:42px;border:none;background:transparent;padding:0;cursor:pointer;}");
  client.println(".refresh-button img{width:36px;height:36px;}");
  client.println(".network-list{width:100%;}");
  client.println(".network-item{margin-bottom:4px;}");
  client.println(".network-link{position:relative;display:block;width:100%;height:72px;padding:0 75px;border:none;border-radius:12px;background:#fff;text-align:left;font-size:27px;color:#111;text-decoration:none;box-sizing:border-box;line-height:72px;}");
  client.println(".network-link:hover{background:#f5f5f5;}");
  client.println(".network-link:active{background:#eee;}");
  client.println(".wifi-icon{position:absolute;left:18px;top:50%;transform:translateY(-50%);width:40px;height:40px;object-fit:contain;pointer-events:none;}");
  client.println(".ssid{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;text-align:left;}");
  client.println(".lock-icon{position:absolute;right:18px;top:50%;transform:translateY(-50%);width:30px;height:30px;object-fit:contain;pointer-events:none;}");
  client.println("</style>");

  client.println("</head>");
  client.println("<body>");

  client.println("<div class=\"header\">");
  client.println("<h1 class=\"title\">Available networks</h1>");
  client.println("<a href=\"/scan\" class=\"refresh-button\"><img src=\"/refresh.png\" alt=\"Refresh\"></a>");
  client.println("</div>");

  client.println("<div class=\"network-list\">");

  if (n_networks == 0) {
    client.println("<div style=\"text-align:center;\">No networks found</div>");
  } else {
    for (int i = 0; i < n_networks; i++) {
      String networkSSID = WiFi.SSID(i);
      String safeSSID = htmlEscape(networkSSID);
      int rssi = WiFi.RSSI(i);
      bool isOpen = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
      String wifiIcon = getWiFiIcon(rssi);

      client.println("<div class=\"network-item\">");
      client.print("<a class=\"network-link\" href=\"/join?ssid=");
      client.print(networkSSID);
      client.print("&open=");
      client.print(isOpen ? "true" : "false");
      client.println("\">");

      client.print("<img class=\"wifi-icon\" src=\"");
      client.print(wifiIcon);
      client.println("\" alt=\"WiFi\">");

      client.print("<span class=\"ssid\">");
      client.print(safeSSID);
      client.println("</span>");

      if (!isOpen) {
        client.println("<img class=\"lock-icon\" src=\"/locked.png\" alt=\"Locked\">");
      }

      client.println("</a>");
      client.println("</div>");
    }
  }

  client.println("</div>");
  client.println("</body>");
  client.println("</html>");
}


// =====================================================
// SEND DETAIL / CONNECT PAGE ("WIFI NETWORK DETAIL")
// =====================================================

void sendDetailPage(
  WiFiClient& client,
  const String& ssid,
  bool isOpen
) {
  String safeSSID = htmlEscape(ssid);

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Cache-Control: no-cache, no-store, must-revalidate");
  client.println("Connection: close");
  client.println();

  client.println("<!DOCTYPE html>");
  client.println("<html>");
  client.println("<head>");
  client.println("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
  client.println("<meta charset=\"UTF-8\">");
  client.println("<title>WIFI NETWORK DETAIL</title>");

  client.println("<style>");
  client.println("html,body{margin:0;padding:0;font-family:Arial,Helvetica,sans-serif;background:#fff;color:#111;}");
  client.println("body{padding:30px 24px;max-width:480px;margin:0 auto;}");
  client.println("h1{font-size:24px;color:#666;text-align:center;margin-bottom:20px;text-transform:uppercase;}");
  client.println("h3{font-size:28px;text-align:center;margin:0 0 25px 0;word-break:break-word;color:#111;}");
  client.println(".password-label{display:block;font-size:16px;margin-bottom:8px;color:#555;}");
  client.println(".password-container{position:relative;width:100%;margin-bottom:25px;}");
  client.println(".password-input{width:100%;height:48px;padding:0 55px 0 14px;border:1px solid #ccc;border-radius:8px;font-size:18px;box-sizing:border-box;}");
  client.println(".visibility-button{position:absolute;right:8px;top:50%;transform:translateY(-50%);width:36px;height:36px;padding:4px;border:none;background:transparent;cursor:pointer;}");
  client.println(".visibility-button img{width:28px;height:28px;}");
  client.println(".connect-button{display:block;width:100%;height:50px;border:none;border-radius:8px;background:#4CAF50;color:#fff;font-size:18px;font-weight:600;cursor:pointer;}");
  client.println(".connect-button:disabled{background:#aaa;cursor:default;}");
  client.println(".back-button{display:flex;align-items:center;justify-content:center;width:100%;height:45px;margin-top:12px;border:none;background:transparent;font-size:16px;color:#777;text-decoration:none;}");
  client.println("</style>");

  client.println("<script>");
  client.println("function togglePassword(){");
  client.println("  const input=document.getElementById('password');");
  client.println("  const icon=document.getElementById('visibilityIcon');");
  client.println("  if(input.type==='password'){ input.type='text'; icon.src='/invisible.png'; }");
  client.println("  else{ input.type='password'; icon.src='/visible.png'; }");
  client.println("}");

  client.println("async function connectWiFi(){");
  client.println("  const button=document.getElementById('connectButton');");
  client.println("  const passwordInput=document.getElementById('password');");
  client.println("  const password = passwordInput ? passwordInput.value : '';");
  client.println("  button.disabled=true;");
  client.println("  button.textContent='Connecting...';");
  client.println("  const body='ssid='+encodeURIComponent('" + ssid + "')+'&password='+encodeURIComponent(password);");
  client.println("  try{");
  client.println("    await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});");
  client.println("  }catch(e){}");
  client.println("  setTimeout(function(){ window.location.href='http://192.168.4.1/'; },18000);");
  client.println("}");
  client.println("</script>");

  client.println("</head>");
  client.println("<body>");

  client.println("<h1>WIFI NETWORK DETAIL</h1>");
  client.println("<h3>" + safeSSID + "</h3>");

  client.println("<form onsubmit=\"event.preventDefault(); connectWiFi();\">");

  if (!isOpen) {
    client.println("<label class=\"password-label\" for=\"password\">Password</label>");
    client.println("<div class=\"password-container\">");
    client.println("<input id=\"password\" class=\"password-input\" type=\"password\" autocomplete=\"off\" required>");
    client.println("<button class=\"visibility-button\" type=\"button\" onclick=\"togglePassword()\">");
    client.println("<img id=\"visibilityIcon\" src=\"/visible.png\" alt=\"Show\">");
    client.println("</button>");
    client.println("</div>");
  }

  client.println("<button id=\"connectButton\" class=\"connect-button\" type=\"submit\">Connect</button>");
  client.println("<a href=\"/\" class=\"back-button\">Back</a>");

  client.println("</form>");

  client.println("</body>");
  client.println("</html>");
}


// =====================================================
// START SOFT AP
// =====================================================

void startSoftAP() {
  Serial.println("Starting SoftAP...");

  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP(
    apSSID,
    apPassword
  );

  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
}


// =====================================================
// CONNECT TO TARGET WIFI
// =====================================================

bool connectToTargetWiFi() {
  Serial.println();
  Serial.println("==============================");
  Serial.println("Trying to connect to:");
  Serial.println(targetSSID);

  Serial.println("Disabling SoftAP...");

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);

  if (targetPassword.length() == 0) {
    WiFi.begin(targetSSID.c_str());
  } else {
    WiFi.begin(
      targetSSID.c_str(),
      targetPassword.c_str()
    );
  }

  const unsigned long timeout = 15000;
  unsigned long startTime = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startTime < timeout
  ) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.println("==============================");
    return true;
  }

  Serial.println("WiFi connection failed.");
  WiFi.disconnect(true);

  startSoftAP();

  Serial.println("Returned to setup mode.");
  Serial.println("==============================");

  return false;
}


// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("LittleFS mounted.");

  startSoftAP();

  server.begin();

  Serial.println("Web server started.");
}


// =====================================================
// LOOP
// =====================================================

void loop() {
  WiFiClient client = server.available();

  if (!client) {
    return;
  }

  Serial.println("New Client.");

  unsigned long requestTimeout = millis() + 2000;

  while (
    !client.available() &&
    millis() < requestTimeout
  ) {
    delay(1);
  }

  if (!client.available()) {
    Serial.println("Client connected but sent no data.");
    client.stop();
    return;
  }

  String requestLine = client.readStringUntil('\n');
  requestLine.trim();

  if (requestLine.length() == 0) {
    Serial.println("Empty request.");
    client.stop();
    return;
  }

  Serial.print("Request: ");
  Serial.println(requestLine);

  int contentLength = 0;

  while (client.connected()) {
    String line = client.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) {
      break;
    }

    if (line.startsWith("Content-Length:")) {
      contentLength = line.substring(15).toInt();
    }
  }

  String body;

  if (contentLength > 0) {
    if (!readRequestBody(client, contentLength, body)) {
      Serial.println("Failed to read complete request body.");

      client.println("HTTP/1.1 400 Bad Request");
      client.println("Content-Type: text/plain");
      client.println("Connection: close");
      client.println();
      client.println("Incomplete request body.");

      client.stop();
      return;
    }
  }

  // ===================================================
  // ROUTING
  // ===================================================

  if (requestLine.startsWith("GET /wifi-strong.png")) {
    sendFile(client, "/wifi-strong.png", "image/png");
  }
  else if (requestLine.startsWith("GET /wifi-good.png")) {
    sendFile(client, "/wifi-good.png", "image/png");
  }
  else if (requestLine.startsWith("GET /wifi-fair.png")) {
    sendFile(client, "/wifi-fair.png", "image/png");
  }
  else if (requestLine.startsWith("GET /wifi-weak.png")) {
    sendFile(client, "/wifi-weak.png", "image/png");
  }
  else if (requestLine.startsWith("GET /locked.png")) {
    sendFile(client, "/locked.png", "image/png");
  }
  else if (requestLine.startsWith("GET /refresh.png")) {
    sendFile(client, "/refresh.png", "image/png");
  }
  else if (requestLine.startsWith("GET /visible.png")) {
    sendFile(client, "/visible.png", "image/png");
  }
  else if (requestLine.startsWith("GET /invisible.png")) {
    sendFile(client, "/invisible.png", "image/png");
  }
  else if (requestLine.startsWith("GET /favicon.ico")) {
    client.println("HTTP/1.1 204 No Content");
    client.println("Connection: close");
    client.println();
  }

  // ---------------------------------------------------
  // DETAIL PAGE ("WIFI NETWORK DETAIL")
  // ---------------------------------------------------
  else if (requestLine.startsWith("GET /join")) {
    String ssid = getQueryParam(requestLine, "ssid");
    String openParam = getQueryParam(requestLine, "open");
    bool isOpen = (openParam == "true");

    sendDetailPage(client, ssid, isOpen);
  }

  // ---------------------------------------------------
  // CONNECT
  // ---------------------------------------------------
  else if (requestLine.startsWith("POST /connect")) {
    Serial.println("Received WiFi credentials.");

    targetSSID = getPostValue(body, "ssid");
    targetPassword = getPostValue(body, "password");

    Serial.print("Target SSID: ");
    Serial.println(targetSSID);

    Serial.print("Password length: ");
    Serial.println(targetPassword.length());

    if (targetSSID.length() == 0) {
      client.println("HTTP/1.1 400 Bad Request");
      client.println("Content-Type: text/plain");
      client.println("Connection: close");
      client.println();
      client.println("SSID is empty.");

      client.stop();
      return;
    }

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println();
    client.println("Connecting...");
    client.flush();
    delay(100);

    client.stop();

    connectToTargetWiFi();
  }

  // ---------------------------------------------------
  // SCAN
  // ---------------------------------------------------
  else if (requestLine.startsWith("GET /scan ")) {
    int n_networks = scanWiFi();
    sendWebPage(client, n_networks);
  }

  // ---------------------------------------------------
  // ROOT
  // ---------------------------------------------------
  else if (requestLine.startsWith("GET / ")) {
    int n_networks = scanWiFi();
    sendWebPage(client, n_networks);
  }

  // ---------------------------------------------------
  // 404
  // ---------------------------------------------------
  else {
    Serial.print("404 for request: ");
    Serial.println(requestLine);

    client.println("HTTP/1.1 404 Not Found");
    client.println("Content-Type: text/plain");
    client.println("Connection: close");
    client.println();
    client.println("404 Not Found");
  }

  if (client.connected()) {
    client.stop();
  }

  Serial.println("Client disconnected.");
}