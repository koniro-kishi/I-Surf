#include <LittleFS.h>

void setup() {
    const char* path = "/join.html"; 

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed. Skipping SoftAP phase.");
    }
    
    if (LittleFS.remove(path)) {
        Serial.println("File deleted successfully");
    } else {
        Serial.println("Delete failed");
    }
}

void loop(){
    
}
