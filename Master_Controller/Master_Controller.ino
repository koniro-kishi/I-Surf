#include <Wire.h>
#include "ESP32_NOW.h"
#include "WiFi.h"
#include <esp_mac.h>  // For the MAC2STR and MACSTR macros
#include <LiquidCrystal_I2C.h>

/* === DEFINITIONS === */

#define DEFAULT_WIFI_CHANNEL 6 // check using "netsh wlan show interfaces" in cmd
#define WIFI_SSID "Sapiq"
#define WIFI_PASSWORD "123581321"


/* === CLASSES === */

class ESP_NOW_Broadcast_Peer : public ESP_NOW_Peer {
public:
  // Constructor of the class using the broadcast address
  ESP_NOW_Broadcast_Peer(uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) : ESP_NOW_Peer(ESP_NOW.BROADCAST_ADDR, channel, iface, lmk) {}

  // Destructor of the class
  ~ESP_NOW_Broadcast_Peer() {
    remove();
  }

  // Function to properly initialize the ESP-NOW and register the broadcast peer
  bool begin() {
    if (!ESP_NOW.begin() || !add()) {
      log_e("Failed to initialize ESP-NOW or register the broadcast peer");
      return false;
    }
    return true;
  }

  // Function to send a message to all devices within the network
  bool send_message(const uint8_t *data, size_t len) {
    
    if (!send(data, len)) {
      log_e("Failed to broadcast message");
      return false;
    }
    return true;
  }
};


/* === GLOBAL VARIABLES === */

// Peer communication
uint32_t msg_count = 0;


/* === INSTANCES === */

// Broadcast peer object
ESP_NOW_Broadcast_Peer *broadcast_peer = nullptr;

// LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);


/* === FUNCTIONS === */

// Print to both lcd and serial
void printSingleLog(const char* string, int duration = 0){
  lcd.clear();

  lcd.setCursor(0, 0);
  Serial.println(string);
  lcd.print(string);

  if (duration != 0){
    delay(duration);
    lcd.clear();
  }
}

void printDoubleLog(const char* string0, const char* string1, int duration = 0){
  lcd.clear();

  lcd.setCursor(0, 0);
  Serial.println(string0);
  lcd.print(string0);

  lcd.setCursor(0, 1);
  Serial.println(string1);
  lcd.print(string1);

  if (duration != 0){
    delay(duration);
    lcd.clear();
  }
}

/* Main */

void setup() {
  Serial.begin(115200);

  // LCD I2C
  lcd.init();         // inisialisasi LCD
  lcd.backlight();    // nyalakan lampu
  printSingleLog("Initializing...", 2000);



  // === WIFI MODULE ===

  // Initialize the Wi-Fi module
  WiFi.mode(WIFI_AP_STA);           // configure wifi mode
  WiFi.setChannel(DEFAULT_WIFI_CHANNEL);    // set wifi channel

  // Starting subsystem station (STA) for peer connection
  printSingleLog("Starting STA...", 0);
  while (!WiFi.STA.started()) {
    Serial.println("Starting STA...");
    delay(100);
  }
  delay(2000);
  printSingleLog("STA started", 2000);

  // Initiate WIFI connection
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  printSingleLog("Connecting...", 0);
  while(WiFi.status() != WL_CONNECTED) {  // shows that the module is still trying to connect to the WIFI connection
    delay(500);
    Serial.print(".");
  }
  printDoubleLog("Connected WIFI:", WIFI_SSID, 2000);

  // Report WiFi parameters
  Serial.println("Saya-Berselancar: Master");
  Serial.println("Wi-Fi parameters:");
  Serial.println("  Mode: STA");
  Serial.println("  MAC Address: " + WiFi.macAddress());
  Serial.println(String("  SSID: ") + WIFI_SSID);
  Serial.println("  IP Address: " + WiFi.localIP().toString());
  Serial.printf("  Channel: %d\n", WiFi.channel());

  // Dynamically create the peer using the actual connected channel
  uint8_t active_channel = WiFi.channel();
  broadcast_peer = new ESP_NOW_Broadcast_Peer(active_channel, WIFI_IF_STA, nullptr);

  // === ESP-NOW PROTOCOL ===

  // Register the broadcast peer
  if (!broadcast_peer->begin()) {
    Serial.println("Failed to initialize broadcast peer");
    Serial.println("Rebooting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  // Report ESP-NOW successful
  String espNowVerStr = "ESP-NOW v" + String(ESP_NOW.getVersion());
  printDoubleLog("Master online", espNowVerStr.c_str(), 2000);



  // === SETUP COMPLETE === 
  printSingleLog("Setup complete!", 2000);
}

void loop() {
  // Broadcast a message to all devices within the network
  char data[32];
  snprintf(data, sizeof(data), "Command #%lu", msg_count++);

  printDoubleLog("Broadcast Msg:", data);

  if (!broadcast_peer->send_message((uint8_t *)data, sizeof(data))) {
    Serial.println("Failed to broadcast message");
  }

  delay(5000);
}