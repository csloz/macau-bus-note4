// Wi-Fi STA bring-up + on-demand scan for the NOTE4 bus display.
#pragma once

#include "esp_err.h"

#include <cstddef>

// Initializes NVS+netif+wifi and connects. Blocks up to timeout_ms waiting
// for an IP. Returns ESP_OK on connect, ESP_ERR_TIMEOUT otherwise (retry
// with BusWifiReconnect).
esp_err_t BusWifiStart(const char* ssid, const char* password,
                       uint32_t timeout_ms);

// Blocking reconnect attempt (uses stored credentials).
esp_err_t BusWifiReconnect(uint32_t timeout_ms);

bool BusWifiConnected();

struct BusWifiAp {
    char ssid[33];
    int rssi;
};

// Initializes Wi-Fi on demand if needed, scans, dedupes by SSID, and fills
// `out` sorted by signal strength. Blocking (~2 s). *count = entries filled,
// negative on failure (e.g. radio busy).
esp_err_t BusWifiScan(BusWifiAp* out, int max, int* count);
