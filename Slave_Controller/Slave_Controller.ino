/*
    ESP-NOW Broadcast Slave
    Lucas Saavedra Vaz - 2024 (Cleaned & Debugged)
*/

#include "ESP32_NOW.h"
#include "WiFi.h"
#include <esp_mac.h>  // For the MAC2STR and MACSTR macros
#include <vector>

/* Definitions */
#define DEFAULT_WIFI_CHANNEL 1

/* Classes */
class ESP_NOW_Peer_Class : public ESP_NOW_Peer {
public:
  // Constructor of the class
  ESP_NOW_Peer_Class(const uint8_t *mac_addr, uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) : ESP_NOW_Peer(mac_addr, channel, iface, lmk) {}

  // Destructor of the class
  ~ESP_NOW_Peer_Class() {}

  // Function to register the master peer
  bool add_peer() {
    if (!add()) {
      log_e("Failed to register the broadcast peer");
      return false;
    }
    return true;
  }

  // Function to print the received messages from the master
  void onReceive(const uint8_t *data, size_t len, bool broadcast) override {
    Serial.printf("Received a message from master " MACSTR " (%s)\n", MAC2STR(addr()), broadcast ? "broadcast" : "unicast");
    Serial.printf("  Message: %s\n", (char *)data);
  }
};

/* Global Variables */
ESP_NOW_Peer_Class *master = nullptr;
bool master_found = false;
uint8_t current_scan_channel = 1;

/* Callbacks */
void register_master(const esp_now_recv_info_t *info, const uint8_t *data, int len, void *arg) {
  if (memcmp(info->des_addr, ESP_NOW.BROADCAST_ADDR, 6) == 0) {
    // // Check if master is already registered to avoid duplicates
    // for (auto master : masters) {
    //   if (memcmp(master->addr(), info->src_addr, 6) == 0) {
    //     return; // Already registered
    //   }
    // }

    Serial.printf("Unknown peer " MACSTR " sent a broadcast message\n", MAC2STR(info->src_addr));
    Serial.println("Registering the peer as a master");

    ESP_NOW_Peer_Class *new_master = new ESP_NOW_Peer_Class(info->src_addr, current_scan_channel, WIFI_IF_STA, nullptr);
    if (!new_master->add_peer()) {
      Serial.println("Failed to register the new master");
      delete new_master;
      return;
    }
    master = new_master;
    Serial.printf("Successfully registered master " MACSTR "\n", MAC2STR(new_master->addr()));
  } else {
    log_v("Received a unicast message from " MACSTR, MAC2STR(info->src_addr));
  }
}

/* Main */
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize the Wi-Fi module
  WiFi.mode(WIFI_STA);
  WiFi.setChannel(DEFAULT_WIFI_CHANNEL);
  
  while (!WiFi.STA.started()) {
    delay(100);
  }

  Serial.println("ESP-NOW Example - Broadcast Slave");
  Serial.println("Wi-Fi parameters:");
  Serial.println("  Mode: STA");
  Serial.println("  MAC Address: " + WiFi.macAddress());
  Serial.printf("  Channel: %d\n", DEFAULT_WIFI_CHANNEL);

  // Initialize the ESP-NOW protocol
  if (!ESP_NOW.begin()) {
    Serial.println("Failed to initialize ESP-NOW");
    Serial.println("Rebooting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  Serial.printf("ESP-NOW version: %d, max data length: %d\n", ESP_NOW.getVersion(), ESP_NOW.getMaxDataLen());

  // Register the new peer callback for broadcasts from unknown senders
  ESP_NOW.onNewPeer(register_master, nullptr);

  Serial.println("Setup complete. Waiting for a master to broadcast a message...");
}

void loop() {
  // If we haven't found a master yet, hop channels to listen for broadcasts
  if (master == nullptr) {
    current_scan_channel = (current_scan_channel % 13) + 1; // Cycle channels 1-13
    WiFi.setChannel(current_scan_channel);
    Serial.printf("Currently scanning channel %d\n", current_scan_channel);
    delay(200); // Listen on this channel for a short moment
    return;     // Skip the rest of the loop until connected
  }
  
  // Print debug information every 10 seconds
  static unsigned long last_debug = 0;
  if (millis() - last_debug > 10000) {
    last_debug = millis();
    Serial.printf("Registered master: " MACSTR "\n", MAC2STR(master->addr()));
    Serial.printf("Communicating in WiFi channel %d\n", current_scan_channel);
  }
  delay(100);

  delay(100);
}