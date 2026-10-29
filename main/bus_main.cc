// NOTE4 bus display: Quote/0-style live arrival card for a configurable
// DSAT stop + hero route on a 4.2" 400x300 e-paper panel, refreshed over
// Wi-Fi every 5 minutes.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "esp_console.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "hearth_canvas.h"
#include "zectrix_epd.h"

#include "bus_api.h"
#include "bus_config.h"
#include "bus_display.h"
#include "bus_power.h"
#include "bus_stops_data.h"
#include "bus_ui.h"
#include "bus_web.h"
#include "bus_wifi.h"
#include "u8g2_font.h"
#include "u8g2_font_macau.h"

namespace {

constexpr const char* kTag = "bus.main";
constexpr const char* kVersion = "2.9.2";
constexpr TickType_t kRefreshInterval = pdMS_TO_TICKS(5 * 60 * 1000);
constexpr TickType_t kWifiRetryInterval = pdMS_TO_TICKS(30 * 1000);
constexpr int kRefreshBit = BIT0;

HearthCanvas& g_canvas = BusDisplayCanvas();
zectrix_epd_handle_t g_epd = nullptr;
EventGroupHandle_t g_loop_events = nullptr;
BusConfig g_config;

char g_status[40] = "starting";
char g_updated[16] = "--:--";

void NowString(char* out, size_t cap) {
    time_t now = 0;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    std::snprintf(out, cap, "%02d:%02d", local.tm_hour, local.tm_min);
}

void NotifyRefresh() {
    if (g_loop_events != nullptr) {
        xEventGroupSetBits(g_loop_events, kRefreshBit);
    }
}

// Set before a card paint that should use the slow OTP waveform even in fast
// mode: the background data refresh is the periodic ghost-cleaner.
bool g_force_otp_paint = false;

void Paint() {
    BusDisplayPaint(!g_force_otp_paint);
    g_force_otp_paint = false;
}

// Largest text scale (<= max_scale) whose width fits max_w.
int FitScale(const char* s, int max_scale, int max_w) {
    int sc = max_scale;
    while (sc > 1 && g_canvas.TextWidth(s, sc) > max_w) {
        sc--;
    }
    return sc;
}

void RenderSplash(const char* line2) {
    g_canvas.Clear();
    g_canvas.TextCentered(100, "NOTE4 BUS BOARD",
                          FitScale("NOTE4 BUS BOARD", 2,
                                   HearthCanvas::kWidth - 24), false);
    g_canvas.TextCentered(160, line2, FitScale(line2, 2,
                                               HearthCanvas::kWidth - 24),
                          false);
    g_canvas.TextCentered(220, kVersion, 1, false);
    Paint();
}

void RenderNoWifi(const char* detail) {
    g_canvas.Clear();
    g_canvas.FillRect(0, 0, HearthCanvas::kWidth, 40, true);
    g_canvas.Text(12, 12, "SET UP OVER USB", 2, true);
    g_canvas.Text(12, 90, "Connect USB, open serial monitor,", 1, false);
    g_canvas.Text(12, 115, "then run:", 1, false);
    g_canvas.Text(12, 150, "  bus-set ssid <your-wifi>", 2, false);
    g_canvas.Text(12, 185, "  bus-set pass <password>", 2, false);
    g_canvas.Text(12, 220, "  bus-set stop T394  bus-set route 51A", 1, false);
    g_canvas.Text(12, 245, "  bus-save  bus-reboot", 2, false);
    char line[48];
    std::snprintf(line, sizeof(line), "status: %s", detail);
    g_canvas.Text(12, 278, line, 1, false);
    Paint();
}

// Quote/0-style card, scaled up for the 400x300 panel.

constexpr const uint8_t* kSmallFont = u8g2_font_wqy12_t_macau;

int DrawSmall(int x, int y_base, const char* s, int max_px, bool inverted) {
    return u8g2font::DrawText(g_canvas, kSmallFont, x, y_base, s, max_px,
                              inverted);
}

int SmallWidth(const char* s) {
    return u8g2font::TextWidth(kSmallFont, s);
}

// Thin status strip across the very top: IP + signal, battery, date/time.
void RenderStatusBar() {
    const int w = HearthCanvas::kWidth;
    char ip[24] = "";
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != nullptr) {
        esp_netif_ip_info_t info = {};
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK &&
            info.ip.addr != 0) {
            std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }
    wifi_ap_record_t ap = {};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    BusPowerState pwr;
    const bool batt_ok = BusPowerRead(&pwr);
    char batt[16] = "";
    if (batt_ok) {
        if (pwr.full) {
            std::snprintf(batt, sizeof(batt), "%u%% FULL", pwr.percent);
        } else if (pwr.charging) {
            std::snprintf(batt, sizeof(batt), "%u%% CHG", pwr.percent);
        } else {
            std::snprintf(batt, sizeof(batt), "%u%%", pwr.percent);
        }
    }

    time_t now = 0;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    static const char* const kDays[] = {"SUN", "MON", "TUE", "WED",
                                        "THU", "FRI", "SAT"};
    static const char* const kMonths[] = {"JAN", "FEB", "MAR", "APR",
                                          "MAY", "JUN", "JUL", "AUG",
                                          "SEP", "OCT", "NOV", "DEC"};
    char clock_str[24];
    std::snprintf(clock_str, sizeof(clock_str), "%s %d %s %02d:%02d",
                  kDays[local.tm_wday], local.tm_mday,
                  kMonths[local.tm_mon], local.tm_hour, local.tm_min);

    ESP_LOGI(kTag, "statusbar: ip=\"%s\" rssi=%d batt=\"%s\" (%s) clock=\"%s\"",
             ip, rssi, batt, batt_ok ? "ok" : "invalid", clock_str);

    const int y_base = 15;
    int x = 8;
    x = DrawSmall(x, y_base, ip[0] ? ip : "NO WIFI", 110, false);
    if (rssi != 0) {
        char sig[12];
        std::snprintf(sig, sizeof(sig), " %ddB", rssi);
        x = DrawSmall(x, y_base, sig, 44, false);
    }
    if (batt[0]) {
        const int bw = SmallWidth(batt);
        DrawSmall((w - bw) / 2, y_base, batt, bw + 1, false);
    }
    const int cw = SmallWidth(clock_str);
    DrawSmall(w - 8 - cw, y_base, clock_str, cw + 1, false);
    g_canvas.HLine(6, 20, w - 12, true);
}

void RenderCard(const BusView& view, const Weather* wx, const char* stop,
                const char* also_routes) {
    g_canvas.Clear();
    const int w = HearthCanvas::kWidth;

    RenderStatusBar();

    // --- Header bar: solid black, white text ---
    g_canvas.FillRect(0, 22, w, 62, true);
    g_canvas.Text(14, 28, view.route, 3, true);
    const int stop_x = 14 + g_canvas.TextWidth(view.route, 3) + 14;
    g_canvas.Text(stop_x, 46, stop, 1, true);
    const int time_x = w - 14 - g_canvas.TextWidth(g_updated, 2);
    g_canvas.Text(time_x, 38, g_updated, 2, true);

    // --- Hero ETA: minutes if known, else stops away ---
    char num[12] = "--";
    char unit[8] = "";
    char sub[64] = "no live data for this route";
    if (view.has_bus) {
        if (view.stops_away == 0) {
            std::snprintf(num, sizeof(num), "NOW");
            sub[0] = '\0';
            std::snprintf(sub, sizeof(sub), "bus at stop");
            unit[0] = '\0';
        } else if (view.eta_min >= 0) {
            std::snprintf(num, sizeof(num), "%d", view.eta_min);
            std::snprintf(unit, sizeof(unit), "MIN");
            if (view.km >= 0) {
                std::snprintf(sub, sizeof(sub), "%d stops - %.1f km",
                              view.stops_away, view.km);
            } else {
                std::snprintf(sub, sizeof(sub), "%d stops", view.stops_away);
            }
        } else {
            std::snprintf(num, sizeof(num), "%d", view.stops_away);
            std::snprintf(unit, sizeof(unit), "STOPS");
            if (view.km >= 0) {
                std::snprintf(sub, sizeof(sub), "%.1f km", view.km);
            } else {
                std::snprintf(sub, sizeof(sub), "ETA UNAVAILABLE");
            }
        }
    }
    // Scale the hero number up, then clamp so it always fits the panel.
    int hero_scale = 6;
    while (hero_scale > 2 &&
           g_canvas.TextWidth(num, hero_scale) > (w - 180)) {
        hero_scale--;
    }
    g_canvas.Text(16, 100, num, hero_scale, false);
    if (unit[0]) {
        const int ux = 16 + g_canvas.TextWidth(num, hero_scale) + 10;
        g_canvas.Text(ux, 100 + 16 * hero_scale - 32, unit, 2, false);
    }
    g_canvas.Text(16, 202, sub, 2, false);

    // --- Weather block (right of hero area) ---
    if (wx != nullptr && wx->valid) {
        char temp[16];
        std::snprintf(temp, sizeof(temp), "%dC", wx->temp_c);
        int tw = g_canvas.TextWidth(temp, 3);
        g_canvas.Text(w - 16 - tw, 104, temp, 3, false);
        tw = g_canvas.TextWidth(wx->text, 1);
        g_canvas.Text(w - 16 - tw, 162, wx->text, 1, false);
        char rh[16];
        std::snprintf(rh, sizeof(rh), "RH %d%%", wx->humidity);
        tw = g_canvas.TextWidth(rh, 1);
        g_canvas.Text(w - 16 - tw, 182, rh, 1, false);
    }

    // --- Divider ---
    g_canvas.HLine(10, 240, w - 20, true);
    g_canvas.HLine(10, 241, w - 20, true);

    // --- Footer: following bus + other routes ---
    char nextline[64];
    if (view.has_next) {
        if (view.next_eta_min >= 0) {
            std::snprintf(nextline, sizeof(nextline),
                          "NEXT %d STOPS - %.1f KM - %dM",
                          view.next_stops_away, view.next_km,
                          view.next_eta_min);
        } else if (view.next_km >= 0) {
            std::snprintf(nextline, sizeof(nextline), "NEXT %d STOPS - %.1f KM",
                          view.next_stops_away, view.next_km);
        } else {
            std::snprintf(nextline, sizeof(nextline), "NEXT %d STOPS",
                          view.next_stops_away);
        }
    } else if (view.has_stop) {
        std::snprintf(nextline, sizeof(nextline), "NEXT: NO FOLLOWING BUS");
    } else {
        std::snprintf(nextline, sizeof(nextline), "STOP NOT ON ROUTE %s",
                      view.route);
    }
    // Auto-shrink so the line can never overflow the panel width.
    g_canvas.Text(12, 248, nextline,
                  FitScale(nextline, 2, w - 24), false);

    char statline[64];
    std::snprintf(statline, sizeof(statline), "%s", g_status);
    const int stat_w = SmallWidth(statline);
    const int sx = w - 12 - stat_w;

    // Trim the ALSO list so it can never collide with the status text.
    char alson[96];
    std::snprintf(alson, sizeof(alson), "ALSO: %s", also_routes);
    int alson_max = sx - 24;
    while (alson[0] != '\0' && SmallWidth(alson) > alson_max) {
        alson[std::strlen(alson) - 1] = '\0';
    }
    DrawSmall(12, 296, alson, alson_max, false);
    DrawSmall(sx, 296, statline, stat_w + 1, false);

    // --- Border ---
    g_canvas.Rect(0, 0, w, HearthCanvas::kHeight, true);
    Paint();
}

// Other routes serving the stop (from the static stop database).
void AlsoRoutes(const char* stop, const char* hero, char* out, size_t cap) {
    out[0] = '\0';
    const bus_stops::Stop* s = nullptr;
    for (size_t i = 0; i < bus_stops::kStopCount; i++) {
        if (std::strcmp(bus_stops::kStops[i].code, stop) == 0) {
            s = &bus_stops::kStops[i];
            break;
        }
    }
    if (s == nullptr) {
        std::snprintf(out, cap, "UNKNOWN STOP");
        return;
    }
    // s->routes is a comma-separated list; skip the hero route.
    size_t pos = 0;
    const char* p = s->routes;
    bool first = true;
    bool hero_found = false;
    while (*p && pos + 8 < cap) {
        const char* comma = std::strchr(p, ',');
        const size_t len = comma ? static_cast<size_t>(comma - p)
                                 : std::strlen(p);
        if (len > 0 && len < 8 &&
            std::strncmp(p, hero, len) != 0) {
            pos += std::snprintf(out + pos, cap - pos, "%s%.*s", first ? "" : " ",
                                 static_cast<int>(len), p);
            first = false;
        } else if (len > 0) {
            hero_found = true;
        }
        p = comma ? comma + 1 : p + std::strlen(p);
    }
    if (pos == 0) {
        std::snprintf(out, cap, hero_found ? "ONLY ROUTE AT STOP" : "-");
    }
}

// Fetch the hero route; if it does not serve the stop, walk the stop's other
// routes until one does. Returns the route actually displayed via out->route.
void UpdateOnce(bool background = false) {
    ESP_LOGI(kTag, "update start");
    // The 5-min background refresh doubles as the ghost-cleaning pass: keep
    // it on the slow OTP waveform even when fast mode is enabled.
    g_force_otp_paint = background;
    if (!BusWifiConnected()) {
        std::snprintf(g_status, sizeof(g_status), "wifi reconnecting");
        (void)BusWifiReconnect(pdMS_TO_TICKS(15000));
    }
    if (!BusWifiConnected()) {
        std::snprintf(g_status, sizeof(g_status), "wifi down");
        return;
    }

    BusView view = {};
    const esp_err_t err = BusApiFetchView(g_config.route, g_config.stop, &view);
    if (err == ESP_OK && !view.has_stop) {
        // Configured route does not serve this stop: try the stop's routes.
        ESP_LOGW(kTag, "%s does not serve %s; probing stop routes",
                 g_config.route, g_config.stop);
        for (size_t i = 0; i < bus_stops::kStopCount && !view.has_stop; i++) {
            const bus_stops::Stop& s = bus_stops::kStops[i];
            if (std::strcmp(s.code, g_config.stop) != 0) {
                continue;
            }
            const char* p = s.routes;
            while (*p && !view.has_stop) {
                const char* comma = std::strchr(p, ',');
                const size_t len = comma ? static_cast<size_t>(comma - p)
                                         : std::strlen(p);
                if (len > 0 && len < sizeof(g_config.route)) {
                    char candidate[8];
                    std::snprintf(candidate, sizeof(candidate), "%.*s",
                                  static_cast<int>(len), p);
                    if (std::strcmp(candidate, g_config.route) != 0) {
                        const esp_err_t e2 =
                            BusApiFetchView(candidate, g_config.stop, &view);
                        if (e2 == ESP_OK && view.has_stop) {
                            ESP_LOGI(kTag, "fallback route: %s", candidate);
                        }
                    }
                }
                p = comma ? comma + 1 : p + std::strlen(p);
            }
        }
    }
    if (err != ESP_OK && !view.has_stop) {
        std::snprintf(g_status, sizeof(g_status), "fetch failed");
    }

    Weather wx = {};
    if (BusWeatherFetch(&wx) != ESP_OK || !wx.valid) {
        ESP_LOGW(kTag, "weather unavailable (non-fatal)");
    }

    NowString(g_updated, sizeof(g_updated));
    if (view.has_stop) {
        std::snprintf(g_status, sizeof(g_status), "OK %s", g_updated);
    }
    ESP_LOGI(kTag,
             "view: route=%s stop=%s found=%d bus=%d stops=%d km=%.1f "
             "eta=%d next=%d wall=%s",
             view.route, g_config.stop, (int)view.has_stop, (int)view.has_bus,
             view.stops_away, view.km, view.eta_min, view.next_stops_away,
             g_updated);
    ESP_LOGI(kTag, "fetched view (stop found: %d)", view.has_stop);

    char also[96];
    AlsoRoutes(g_config.stop, view.route, also, sizeof(also));
    RenderCard(view, wx.valid ? &wx : nullptr, g_config.stop, also);
}

void SntpInit() {
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    // Aliyun/NTP.org reach China networks reliably; Google is a fallback.
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_setservername(2, "time.google.com");
    esp_sntp_init();
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "NOTE4 bus board %s", kVersion);
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_ERROR_CHECK(BusConfigLoad(&g_config));
    BusConsoleHooks hooks = {};
    hooks.refresh_now = NotifyRefresh;
    BusConfigRegisterConsole(&g_config, hooks);

    zectrix_epd_config_t epd_cfg = {};
    zectrix_epd_get_default_config(&epd_cfg);
    ESP_ERROR_CHECK(zectrix_epd_new(&epd_cfg, &g_epd));
    BusDisplayInit(g_epd);

    g_loop_events = xEventGroupCreate();
    ESP_ERROR_CHECK(g_loop_events ? ESP_OK : ESP_ERR_NO_MEM);

    // Three-key UI: settings menu is always available, even before Wi-Fi is
    // configured (it reboots the device after saving).
    BusUiStart(&g_config, NotifyRefresh);

    // Console escape hatch to open the settings menu (handy for testing).
    static const esp_console_cmd_t ui_cmd = {
        "bus-ui", "Open the on-device settings menu", nullptr,
        +[](int, char**) -> int {
            BusUiOpenMenu();
            return 0;
        },
        nullptr, nullptr, nullptr};
    esp_console_cmd_register(&ui_cmd);

    // Synthetic key events so the UI can be driven over the console.
    static const esp_console_cmd_t key_cmd = {
        "bus-key", "Inject a key: up|down|ok [long]", nullptr,
        +[](int argc, char** argv) -> int {
            if (argc < 2) {
                printf("usage: bus-key up|down|ok [long]\n");
                return 1;
            }
            int key = -1;
            if (strcmp(argv[1], "up") == 0) {
                key = 0;
            } else if (strcmp(argv[1], "down") == 0) {
                key = 1;
            } else if (strcmp(argv[1], "ok") == 0) {
                key = 2;
            }
            if (key < 0) {
                printf("unknown key '%s'\n", argv[1]);
                return 1;
            }
            const bool long_press =
                argc > 2 && strcmp(argv[2], "long") == 0;
            BusUiInjectKey(key, long_press);
            printf("injected %s%s\n", argv[1],
                   long_press ? " (long)" : "");
            return 0;
        },
        nullptr, nullptr, nullptr};
    esp_console_cmd_register(&key_cmd);

    static esp_console_cmd_t page_cmd = {
            "bus-page", "Print current UI page + selection", nullptr,
            +[](int argc, char** argv) -> int {
                char state[96];
                BusUiState(state, sizeof(state));
                printf("page: %s (active=%d)\n", state,
                       BusUiActive() ? 1 : 0);
                return 0;
            },
            nullptr, nullptr, nullptr};
    esp_console_cmd_register(&page_cmd);

    RenderSplash("starting...");
    BusDisplaySetFlip(g_config.flip != 0);
    BusDisplaySetFast(g_config.fast != 0);
    if (g_config.flip) {
        // Re-draw the splash the right way up for a flipped mount.
        RenderSplash("starting...");
    }

    BusPowerInit();

    bool configured = BusConfigComplete(g_config);
    if (!configured) {
        ESP_LOGW(kTag, "no wifi configured; waiting for UI or USB bus-set");
        RenderNoWifi("waiting for config");
    }

    if (configured) {
        const esp_err_t wifi = BusWifiStart(g_config.ssid, g_config.password,
                                            pdMS_TO_TICKS(20000));
        BusWebStart(&g_config);
        if (wifi != ESP_OK) {
            std::snprintf(g_status, sizeof(g_status), "wifi timeout");
            RenderNoWifi("wifi connect failed");
        } else {
            SntpInit();
            // Wait briefly for SNTP so the first paint has a clock.
            int sntp_i = 0;
            for (; sntp_i < 50 && !esp_sntp_get_sync_status(); sntp_i++) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            ESP_LOGI(kTag, "sntp wait done after %d rounds (status %d)",
                     sntp_i, esp_sntp_get_sync_status());
            UpdateOnce();
        }
    }

    for (;;) {
        const EventBits_t bits = xEventGroupWaitBits(
            g_loop_events, kRefreshBit, pdTRUE, pdFALSE,
            BusWifiConnected() ? kRefreshInterval : kWifiRetryInterval);

        // Freshly saved config from USB: reboot into the normal boot path.
        if (!configured && BusConfigComplete(g_config)) {
            BusConfigLoad(&g_config);
            esp_restart();
        }
        configured = BusConfigComplete(g_config);
        if (!configured) {
            continue;
        }
        // Never paint the card while a settings page is open.
        if (BusUiActive()) {
            continue;
        }
        if (!BusWifiConnected()) {
            std::snprintf(g_status, sizeof(g_status), "wifi down, retrying");
            (void)BusWifiReconnect(kWifiRetryInterval);
            continue;
        }
        UpdateOnce(true);
    }
}
