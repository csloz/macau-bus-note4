#include "bus_wifi.h"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace {

constexpr const char* kTag = "bus.wifi";
constexpr int kConnectedBit = BIT0;
constexpr int kGotIpBit = BIT1;

EventGroupHandle_t g_events = nullptr;
char g_ssid[33] = {};
char g_pass[65] = {};

void EventHandler(void* arg, esp_event_base_t base, int32_t id,
                  void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_events, kConnectedBit | kGotIpBit);
        ESP_LOGW(kTag, "disconnected; will retry on next BusWifiReconnect");
        if (esp_wifi_connect() != ESP_OK) {
            ESP_LOGW(kTag, "reconnect attempt failed");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(kTag, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(g_events, kConnectedBit | kGotIpBit);
    }
}

esp_err_t Connect(uint32_t timeout_ms) {
    if (g_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), g_ssid,
                 sizeof(cfg.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), g_pass,
                 sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    xEventGroupClearBits(g_events, kConnectedBit | kGotIpBit);
    // ESP_ERR_WIFI_CONN here usually means a connect is already in flight
    // (the STA_START handler issues one); the bits will still arrive.
    if (esp_wifi_connect() != ESP_OK) {
        ESP_LOGD(kTag, "esp_wifi_connect returned in-flight/error; waiting");
    }
    const EventBits_t bits = xEventGroupWaitBits(
        g_events, kGotIpBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & kGotIpBit) ? ESP_OK : ESP_ERR_TIMEOUT;
}

}  // namespace

esp_err_t BusWifiStart(const char* ssid, const char* password,
                       uint32_t timeout_ms) {
    std::snprintf(g_ssid, sizeof(g_ssid), "%s", ssid);
    std::snprintf(g_pass, sizeof(g_pass), "%s", password);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    g_events = xEventGroupCreate();
    ESP_ERROR_CHECK(g_events ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               EventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               EventHandler, nullptr));

    wifi_storage_t storage = WIFI_STORAGE_RAM;
    ESP_ERROR_CHECK(esp_wifi_set_storage(storage));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return Connect(timeout_ms);
}

esp_err_t BusWifiReconnect(uint32_t timeout_ms) {
    if (!BusWifiConnected()) {
        return Connect(timeout_ms);
    }
    return ESP_OK;
}

bool BusWifiConnected() {
    if (g_events == nullptr) {
        return false;
    }
    return (xEventGroupGetBits(g_events) & kConnectedBit) != 0;
}
