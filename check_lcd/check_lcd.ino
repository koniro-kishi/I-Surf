#include <Wire.h>
#include "ESP32_NOW.h"
#include "WiFi.h"
#include <esp_mac.h>  // For the MAC2STR and MACSTR macros
#include <LiquidCrystal_I2C.h>

// LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {
  lcd.init();        // inisialisasi LCD
  lcd.backlight();   // nyalakan lampu
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Hello World");
  lcd.setCursor(0, 1);
  lcd.print("Ohayou sekai");
}

void loop() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Hello World");
  delay(1000);
  lcd.setCursor(0, 1);
  lcd.print("Ohayou sekai");
  delay(1000);
}
