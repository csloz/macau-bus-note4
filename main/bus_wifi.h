// Wi-Fi STA bring-up for the NOTE4 bus display.
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
