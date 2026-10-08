#include <nvs_flash.h>

void setup() {
  Serial.begin(115200);
  
  nvs_flash_erase(); // Erase the NVS partition
  nvs_flash_init();  // Re-initialize the NVS partition
  
  Serial.println("Entire NVS flash erased!");
}

void loop() {}
