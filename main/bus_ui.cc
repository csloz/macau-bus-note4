#include "bus_ui.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bus_api.h"
#include "bus_buttons.h"
#include "bus_config.h"
#include "bus_display.h"
#include "bus_wifi.h"

namespace {

constexpr const char* kTag = "bus.ui";
constexpr int kW = HearthCanvas::kWidth;
constexpr int kHeaderH = 40;

// List pages: up to 8 rows of scale-2 text below the header.
constexpr int kRowH = 32;
constexpr int kListY0 = kHeaderH;
constexpr int kListY1 = kListY0 + 8 * kRowH + 8;
constexpr int kRowsVisible = 8;
constexpr int kEditorWrap = 24;  // scale-2 chars per line

// Character sets for the editors.
constexpr char kPassChars[] =
    " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
constexpr char kStopChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

enum class Page {
    Card,        // normal display (owned by the main loop)
    Menu,
    WifiList,
    PassEdit,
    StopEdit,
    RouteList,
    Message,     // transient status text, then returns to g_msg_back
};

Page g_page = Page::Card;
Page g_msg_back = Page::Menu;
TickType_t g_msg_until = 0;
char g_msg[64] = {};

BusConfig* g_config = nullptr;
void (*g_on_exit)() = nullptr;

// Menu
int g_menu_sel = 0;
constexpr const char* kMenuItems[] = {"WIFI SETUP", "BUS STOP", "EXIT"};
constexpr int kMenuCount = 3;

// Wifi list
BusWifiAp g_aps[15];
int g_ap_count = 0;
int g_wifi_sel = 0;
char g_chosen_ssid[33] = {};

// Password editor
char g_pass[65] = {};
int g_pass_len = 0;
int g_pass_idx = 0;  // index into kPassChars

// Stop editor
char g_stop[8] = {};
int g_stop_len = 0;
int g_stop_idx = 0;  // index into kStopChars

// Route list
char g_routes[12][8];
int g_route_count = 0;
int g_route_sel = 0;

bool g_active = false;
QueueHandle_t g_events = nullptr;

// ---------- rendering ----------

void Header(const char* title) {
    auto& c = BusDisplayCanvas();
    c.Clear();
    c.FillRect(0, 0, kW, kHeaderH, true);
    c.Text(12, 12, title, 2, true);
}

void Hint(const char* text) {
    BusDisplayCanvas().Text(12, 280, text, 1, false);
}

void PaintMenu(bool full) {
    auto& c = BusDisplayCanvas();
    Header("SETTINGS");
    for (int i = 0; i < kMenuCount; i++) {
        const int y = 80 + i * 60;
        c.Text(60, y, kMenuItems[i], 3, i == g_menu_sel);
        if (i == g_menu_sel) {
            c.Text(24, y, ">", 3, false);
        }
    }
    Hint("OK=open  HOLD OK=exit");
    if (full) BusDisplayPaint(); else BusDisplayPaintRows(kHeaderH, 300);
}

void PaintWifiList(bool full) {
    auto& c = BusDisplayCanvas();
    Header("WIFI SETUP");
    if (g_ap_count == 0) {
        c.TextCentered(140, "NO NETWORKS FOUND", 2, false);
        Hint("HOLD OK=back");
        BusDisplayPaint();
        return;
    }
    int first = g_wifi_sel - (kRowsVisible - 1);
    if (first < 0) first = 0;
    if (g_ap_count > kRowsVisible && first > g_ap_count - kRowsVisible) {
        first = g_ap_count - kRowsVisible;
    }
    char line[44];
    for (int row = 0; row < kRowsVisible && first + row < g_ap_count; row++) {
        const int idx = first + row;
        const BusWifiAp& ap = g_aps[idx];
        std::snprintf(line, sizeof(line), "%.*s %d", 30, ap.ssid, ap.rssi);
        c.Text(12, kListY0 + row * kRowH, line, 2, idx == g_wifi_sel);
    }
    Hint("OK=select  HOLD OK=back");
    if (full) BusDisplayPaint(); else BusDisplayPaintRows(kListY0, kListY1);
}

// Current cursor position for the editor text area.
void EditorCursorPos(int typed_len, int& cx, int& cy) {
    cy = 92 + (typed_len / kEditorWrap) * 28;
    cx = 12 + (typed_len % kEditorWrap) * 16;
    if (cy > 244) {
        cy = 244;
    }
}

void PaintEditor(const char* title, const char* sub, const char* typed,
                 bool password, bool full) {
    auto& c = BusDisplayCanvas();
    Header(title);
    c.Text(12, 50, sub, 1, false);

    char shown[80] = {};
    if (password) {
        const int dots = g_pass_len > 30 ? 30 : g_pass_len;
        std::memset(shown, '*', dots);
        shown[dots] = '\0';
    } else {
        std::snprintf(shown, sizeof(shown), "%s", typed);
    }
    int y = 92;
    const int len = static_cast<int>(std::strlen(shown));
    for (int start = 0; start < len; start += kEditorWrap) {
        char chunk[kEditorWrap + 1];
        std::snprintf(chunk, sizeof(chunk), "%.*s", kEditorWrap,
                      shown + start);
        c.Text(12, y, chunk, 2, false);
        y += 28;
    }
    int cx = 0;
    int cy = 0;
    EditorCursorPos(g_pass_len, cx, cy);
    char curbuf[8];
    const char cur = password ? kPassChars[g_pass_idx] : kStopChars[g_stop_idx];
    std::snprintf(curbuf, sizeof(curbuf), "[%c]", cur == ' ' ? '_' : cur);
    c.Text(cx, cy, curbuf, 2, false);

    Hint("OK=next  HOLD OK=done  HOLD DOWN=del");
    if (full) BusDisplayPaint(); else BusDisplayPaintRows(kHeaderH, 300);
}

void PaintPassEdit(bool full) {
    char sub[48];
    std::snprintf(sub, sizeof(sub), "FOR %s", g_chosen_ssid);
    PaintEditor("WIFI PASSWORD", sub, g_pass, true, full);
}

void PaintStopEdit(bool full) {
    PaintEditor("BUS STOP", "ENTER STOP CODE (E.G. T394)", g_stop, false, full);
}

void PaintRouteList(bool full) {
    auto& c = BusDisplayCanvas();
    Header("ROUTE");
    char sub[48];
    std::snprintf(sub, sizeof(sub), "ROUTES AT %s", g_config->stop);
    c.Text(12, kHeaderH + 6, sub, 1, false);
    const int first = g_route_sel < kRowsVisible ? 0 : g_route_sel - kRowsVisible + 1;
    for (int i = first; i < g_route_count; i++) {
        const int row = i - first;
        if (row >= kRowsVisible) break;
        c.Text(24, kListY0 + 16 + row * kRowH, g_routes[i], 2,
               i == g_route_sel);
    }
    Hint("OK=select  HOLD OK=back");
    if (full) BusDisplayPaint(); else BusDisplayPaintRows(kListY0, 300);
}

void ShowMessage(const char* text, Page back, int ms) {
    std::snprintf(g_msg, sizeof(g_msg), "%s", text);
    g_msg_back = back;
    g_msg_until = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    g_page = Page::Message;
    auto& c = BusDisplayCanvas();
    Header("NOTE4 BUS");
    c.TextCentered(150, g_msg, 2, false);
    BusDisplayPaint();
}

// ---------- actions ----------

void SaveAndReboot(const char* what) {
    BusConfigSave(*g_config);
    ESP_LOGI(kTag, "%s saved from UI; rebooting", what);
    ShowMessage("SAVED - REBOOTING...", Page::Menu, 10000);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void ConfirmStop() {
    if (!BusApiFindStop(g_config->stop)) {
        ESP_LOGW(kTag, "unknown stop '%s'", g_config->stop);
        ShowMessage("UNKNOWN STOP", Page::StopEdit, 1800);
        return;
    }
    g_route_count = BusApiStopRoutes(g_config->stop, g_routes, 12);
    if (g_route_count <= 0) {
        ShowMessage("NO ROUTES AT STOP", Page::StopEdit, 1800);
        return;
    }
    g_route_sel = 0;
    for (int i = 0; i < g_route_count; i++) {
        if (std::strcmp(g_routes[i], g_config->route) == 0) {
            g_route_sel = i;
            break;
        }
    }
    g_page = Page::RouteList;
    PaintRouteList(true);
}

void StartWifiList() {
    Header("WIFI SETUP");
    BusDisplayCanvas().TextCentered(140, "SCANNING...", 2, false);
    BusDisplayPaint();
    int count = 0;
    const esp_err_t err = BusWifiScan(g_aps, 15, &count);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "scan failed: %s", esp_err_to_name(err));
        ShowMessage("SCAN FAILED - BUSY", Page::Menu, 1800);
        return;
    }
    g_ap_count = count;
    g_wifi_sel = 0;
    for (int i = 0; i < count; i++) {
        if (std::strcmp(g_aps[i].ssid, g_config->ssid) == 0) {
            g_wifi_sel = i;
            break;
        }
    }
    g_page = Page::WifiList;
    PaintWifiList(true);
}

void StartPassEdit() {
    std::snprintf(g_chosen_ssid, sizeof(g_chosen_ssid), "%s",
                  g_aps[g_wifi_sel].ssid);
    g_pass_len = 0;
    g_pass_idx = 1;  // 'a'
    g_page = Page::PassEdit;
    PaintPassEdit(true);
}

void StartStopEdit() {
    g_stop_len = 0;
    g_stop_idx = 0;  // 'A'
    g_page = Page::StopEdit;
    PaintStopEdit(true);
}

void ExitToCard() {
    g_page = Page::Card;
    g_active = false;
    ESP_LOGI(kTag, "UI closed");
    if (g_on_exit != nullptr) {
        g_on_exit();
    }
}

// ---------- event handling ----------

void HandleEvent(BusKeyEvent ev) {
    switch (g_page) {
        case Page::Card:
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                g_active = true;
                g_menu_sel = 0;
                g_page = Page::Menu;
                PaintMenu(true);
            }
            break;

        case Page::Menu:
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                ExitToCard();
                break;
            }
            if (ev.type != kPressClick) {
                break;
            }
            switch (ev.key) {
                case kKeyUp:
                    g_menu_sel = (g_menu_sel + kMenuCount - 1) % kMenuCount;
                    PaintMenu(false);
                    break;
                case kKeyDown:
                    g_menu_sel = (g_menu_sel + 1) % kMenuCount;
                    PaintMenu(false);
                    break;
                case kKeyOk:
                    if (g_menu_sel == 0) {
                        StartWifiList();
                    } else if (g_menu_sel == 1) {
                        StartStopEdit();
                    } else {
                        ExitToCard();
                    }
                    break;
            }
            break;

        case Page::WifiList:
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                g_page = Page::Menu;
                PaintMenu(true);
                break;
            }
            if (ev.type == kPressClick && g_ap_count > 0) {
                if (ev.key == kKeyUp && g_wifi_sel > 0) {
                    g_wifi_sel--;
                    PaintWifiList(false);
                } else if (ev.key == kKeyDown &&
                           g_wifi_sel < g_ap_count - 1) {
                    g_wifi_sel++;
                    PaintWifiList(false);
                } else if (ev.key == kKeyOk) {
                    StartPassEdit();
                }
            }
            break;

        case Page::PassEdit: {
            const int n = static_cast<int>(sizeof(kPassChars)) - 1;
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                if (g_pass_len == 0) {
                    ShowMessage("EMPTY PASSWORD", Page::PassEdit, 1500);
                } else {
                    g_pass[g_pass_len] = '\0';
                    std::snprintf(g_config->ssid, sizeof(g_config->ssid), "%s",
                                  g_chosen_ssid);
                    std::snprintf(g_config->password,
                                  sizeof(g_config->password), "%s", g_pass);
                    SaveAndReboot("wifi");
                }
                break;
            }
            if (ev.key == kKeyDown && ev.type == kPressLong) {
                if (g_pass_len > 0) {
                    g_pass_len--;
                    g_pass[g_pass_len] = '\0';
                }
                PaintPassEdit(false);
                break;
            }
            if (ev.type == kPressClick) {
                if (ev.key == kKeyUp) {
                    g_pass_idx = (g_pass_idx + 1) % n;
                } else if (ev.key == kKeyDown) {
                    g_pass_idx = (g_pass_idx + n - 1) % n;
                } else if (ev.key == kKeyOk) {
                    if (g_pass_len < static_cast<int>(sizeof(g_pass) - 1)) {
                        g_pass[g_pass_len++] = kPassChars[g_pass_idx];
                    }
                    if (g_pass_len >= static_cast<int>(sizeof(g_pass) - 1)) {
                        g_pass[g_pass_len] = '\0';
                        std::snprintf(g_config->ssid, sizeof(g_config->ssid),
                                      "%s", g_chosen_ssid);
                        std::snprintf(g_config->password,
                                      sizeof(g_config->password), "%s", g_pass);
                        SaveAndReboot("wifi");
                        break;
                    }
                }
                PaintPassEdit(false);
            }
            break;
        }

        case Page::StopEdit: {
            const int n = static_cast<int>(sizeof(kStopChars)) - 1;
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                if (g_stop_len == 0) {
                    ShowMessage("EMPTY CODE", Page::StopEdit, 1500);
                } else {
                    g_stop[g_stop_len] = '\0';
                    std::snprintf(g_config->stop, sizeof(g_config->stop), "%s",
                                  g_stop);
                    ConfirmStop();
                }
                break;
            }
            if (ev.key == kKeyDown && ev.type == kPressLong) {
                if (g_stop_len > 0) {
                    g_stop_len--;
                    g_stop[g_stop_len] = '\0';
                }
                PaintStopEdit(false);
                break;
            }
            if (ev.type == kPressClick) {
                if (ev.key == kKeyUp) {
                    g_stop_idx = (g_stop_idx + 1) % n;
                } else if (ev.key == kKeyDown) {
                    g_stop_idx = (g_stop_idx + n - 1) % n;
                } else if (ev.key == kKeyOk) {
                    if (g_stop_len < static_cast<int>(sizeof(g_stop) - 1)) {
                        g_stop[g_stop_len++] = kStopChars[g_stop_idx];
                    }
                    if (g_stop_len >= 6) {
                        g_stop[g_stop_len] = '\0';
                        std::snprintf(g_config->stop, sizeof(g_config->stop),
                                      "%s", g_stop);
                        ConfirmStop();
                        break;
                    }
                }
                PaintStopEdit(false);
            }
            break;
        }

        case Page::RouteList:
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                StartStopEdit();
                break;
            }
            if (ev.type == kPressClick && g_route_count > 0) {
                if (ev.key == kKeyUp && g_route_sel > 0) {
                    g_route_sel--;
                    PaintRouteList(false);
                } else if (ev.key == kKeyDown &&
                           g_route_sel < g_route_count - 1) {
                    g_route_sel++;
                    PaintRouteList(false);
                } else if (ev.key == kKeyOk) {
                    std::snprintf(g_config->route, sizeof(g_config->route),
                                  "%s", g_routes[g_route_sel]);
                    SaveAndReboot("stop/route");
                }
            }
            break;

        case Page::Message:
            g_page = g_msg_back;
            if (g_page == Page::Menu) {
                PaintMenu(true);
            } else if (g_page == Page::StopEdit) {
                PaintStopEdit(false);
            } else if (g_page == Page::PassEdit) {
                PaintPassEdit(false);
            } else if (g_page == Page::RouteList) {
                PaintRouteList(false);
            }
            break;
    }
}

void UiTask(void*) {
    BusKeyEvent ev;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (g_page == Page::Message) {
            const TickType_t now = xTaskGetTickCount();
            wait = g_msg_until > now ? g_msg_until - now : 0;
        }
        if (xQueueReceive(g_events, &ev, wait) == pdTRUE) {
            HandleEvent(ev);
        } else if (g_page == Page::Message) {
            // Expired: restore the page underneath.
            g_page = g_msg_back;
            if (g_page == Page::Menu) {
                PaintMenu(true);
            } else if (g_page == Page::StopEdit) {
                PaintStopEdit(false);
            } else if (g_page == Page::PassEdit) {
                PaintPassEdit(false);
            } else if (g_page == Page::RouteList) {
                PaintRouteList(false);
            }
        }
    }
}

void Emit(BusKeyEvent event) {
    if (g_events != nullptr) {
        xQueueSend(g_events, &event, 0);
    }
}

}  // namespace

void BusUiStart(BusConfig* config, void (*on_exit)()) {
    g_config = config;
    g_on_exit = on_exit;
    g_events = xQueueCreate(8, sizeof(BusKeyEvent));
    if (g_events == nullptr) {
        ESP_LOGE(kTag, "queue alloc failed");
        return;
    }
    BusButtonsStart(Emit);
    if (xTaskCreate(UiTask, "bus_ui", 6144, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "failed to start UI task");
    }
}

bool BusUiActive() {
    return g_active;
}
