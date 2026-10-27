#include "bus_wifi.h"

#include <cstdio>
#include <cstdlib>
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

namespace {

bool g_wifi_inited = false;

// One-time netif/event-loop/wifi init, shared by BusWifiStart and
// BusWifiScan (the UI may scan before any credentials exist).
void EnsureWifiInited() {
    if (g_wifi_inited) {
        return;
    }
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

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    g_wifi_inited = true;
}

}  // namespace

esp_err_t BusWifiStart(const char* ssid, const char* password,
                       uint32_t timeout_ms) {
    std::snprintf(g_ssid, sizeof(g_ssid), "%s", ssid);
    std::snprintf(g_pass, sizeof(g_pass), "%s", password);

    EnsureWifiInited();
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

esp_err_t BusWifiScan(BusWifiAp* out, int max, int* count) {
    *count = 0;
    if (out == nullptr || max <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    EnsureWifiInited();

    wifi_scan_config_t scan = {};
    scan.show_hidden = false;
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    // ESP_ERR_WIFI_STATE: a connect is in flight; the caller can retry.
    const esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        return ESP_OK;
    }
    // Cap the record buffer; hidden/empty SSIDs are dropped below.
    const uint16_t want = ap_num > 60 ? 60 : ap_num;
    auto* records = static_cast<wifi_ap_record_t*>(
        std::malloc(sizeof(wifi_ap_record_t) * want));
    if (records == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    uint16_t got = want;
    const esp_err_t get = esp_wifi_scan_get_ap_records(&got, records);
    if (get != ESP_OK) {
        std::free(records);
        return get;
    }

    // Sort by RSSI (records are roughly sorted already; make it explicit).
    for (uint16_t i = 1; i < got; i++) {
        const wifi_ap_record_t key = records[i];
        int16_t j = static_cast<int16_t>(i - 1);
        while (j >= 0 && records[j].rssi < key.rssi) {
            records[j + 1] = records[j];
            j--;
        }
        records[j + 1] = key;
    }

    int n = 0;
    for (uint16_t i = 0; i < got && n < max; i++) {
        const char* ssid =
            reinterpret_cast<const char*>(records[i].ssid);
        if (ssid[0] == '\0') {
            continue;  // hidden network
        }
        bool dup = false;
        for (int k = 0; k < n; k++) {
            if (std::strcmp(out[k].ssid, ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        std::snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", ssid);
        out[n].rssi = records[i].rssi;
        n++;
    }
    std::free(records);
    *count = n;
    return ESP_OK;
}
