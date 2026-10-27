#pragma once
#include <Arduino.h>

// --- Pin (ESP32 DevKit) ---
// UV wajib di ADC1 (GPIO 32-39). ADC2 tidak bisa dipakai saat WiFi/ESP-NOW aktif.
const uint8_t PIN_DHT   = 4;
const uint8_t PIN_UV    = 34;
const uint8_t PIN_RELAY = 26;

// Modul relay aktif HIGH
const bool RELAY_ACTIVE_LOW = false;

// --- Pembacaan sensor ---
const uint32_t SENSOR_INTERVAL_MS = 2500; // DHT22 minimal 2 detik antar baca
const uint8_t  SENSOR_FAIL_LIMIT  = 3;    // gagal berturut-turut sebelum dianggap error
const float    EMA_ALPHA          = 0.3f; // 0..1, makin kecil makin halus

// --- Misting (placeholder, kalibrasi di lapangan) ---
const float    DEFAULT_HUM_MIN  = 60.0f;      // % RH, pompa ON jika di bawah ini
const float    DEFAULT_HUM_MAX  = 80.0f;      // % RH, pompa OFF jika mencapai ini
const float    HUM_MIN_GAP      = 5.0f;       // selisih minimal max - min
const uint32_t AUTO_MAX_ON_MS   = 60000UL;    // 60 detik per siklus otomatis
const uint32_t AUTO_REST_MS     = 120000UL;   // jeda minimal antar siklus otomatis
const uint32_t MANUAL_MAX_ON_MS = 600000UL;   // 10 menit untuk manual

// --- Komunikasi ESP-NOW (disamakan dengan slave irigasi) ---
#define SLAVE_TYPE            "Misting"   // maks 15 karakter
#define DEFAULT_SLAVE_CHANNEL 1           // sama dengan OFFLINE_CHANNEL master
const uint32_t SEARCH_INT_MS  = 1000;     // dwell per channel saat mencari master
const uint32_t REQ_TIMEOUT_MS = 15000;    // 3x POLL_INTERVAL master