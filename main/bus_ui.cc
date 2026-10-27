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

// ---------- on-screen keyboard geometry ----------
constexpr int kKbCols = 10;
constexpr int kKbRows = 4;          // 3 char rows + nav row
constexpr int kKbCellW = 32;
constexpr int kKbCellH = 34;
constexpr int kKbPitchY = 36;
constexpr int kKbX0 = (kW - kKbCols * kKbCellW) / 2;  // 40
constexpr int kKbY0 = 84;

// Cell actions.
constexpr uint8_t kActChar = 0;    // type cell->ch
constexpr uint8_t kActMode = 1;    // cycle charset mode
constexpr uint8_t kActDel = 2;     // backspace
constexpr uint8_t kActSpace = 3;
constexpr uint8_t kActOk = 4;      // submit
constexpr uint8_t kActCancel = 5;  // back out
constexpr uint8_t kActNone = 6;    // filler, skipped by the cursor

struct KbCell {
    const char* label;
    char ch;
    uint8_t action;
};

constexpr KbCell None() { return {"", 0, kActNone}; }
constexpr KbCell Key(const char* label, char ch) { return {label, ch, kActChar}; }
constexpr KbCell Mode(const char* label) { return {label, 0, kActMode}; }
constexpr KbCell Del() { return {"DEL", 0, kActDel}; }
constexpr KbCell Act(uint8_t action, const char* label) { return {label, 0, action}; }

// Keyboard modes. The stop editor uses UPPER + DIGITS only.
enum KbMode { KB_LOWER = 0, KB_UPPER, KB_SYM1, KB_SYM2, KB_DIGITS, KB_MODE_COUNT };

// 3 content rows x 10 cols per mode. The cursor skips kActNone cells.
constexpr KbCell kLowerRows[3][kKbCols] = {
    {Key("q",'q'), Key("w",'w'), Key("e",'e'), Key("r",'r'), Key("t",'t'),
     Key("y",'y'), Key("u",'u'), Key("i",'i'), Key("o",'o'), Key("p",'p')},
    {Key("a",'a'), Key("s",'s'), Key("d",'d'), Key("f",'f'), Key("g",'g'),
     Key("h",'h'), Key("j",'j'), Key("k",'k'), Key("l",'l'), None()},
    {Mode("ABC"), Key("z",'z'), Key("x",'x'), Key("c",'c'), Key("v",'v'),
     Key("b",'b'), Key("n",'n'), Key("m",'m'), Del(), None()},
};
constexpr KbCell kUpperRows[3][kKbCols] = {
    {Key("Q",'Q'), Key("W",'W'), Key("E",'E'), Key("R",'R'), Key("T",'T'),
     Key("Y",'Y'), Key("U",'U'), Key("I",'I'), Key("O",'O'), Key("P",'P')},
    {Key("A",'A'), Key("S",'S'), Key("D",'D'), Key("F",'F'), Key("G",'G'),
     Key("H",'H'), Key("J",'J'), Key("K",'K'), Key("L",'L'), None()},
    {Mode("abc"), Key("Z",'Z'), Key("X",'X'), Key("C",'C'), Key("V",'V'),
     Key("B",'B'), Key("N",'N'), Key("M",'M'), Del(), None()},
};
constexpr KbCell kSym1Rows[3][kKbCols] = {
    {Key("1",'1'), Key("2",'2'), Key("3",'3'), Key("4",'4'), Key("5",'5'),
     Key("6",'6'), Key("7",'7'), Key("8",'8'), Key("9",'9'), Key("0",'0')},
    {Key("!",'!'), Key("@",'@'), Key("#",'#'), Key("$",'$'), Key("%",'%'),
     Key("^",'^'), Key("&",'&'), Key("*",'*'), Key("(",'('), Key(")",')')},
    {Mode("S2"), Key("+",'+'), Key("-",'-'), Key("_",'_'), Key(":",':'),
     Key(";",';'), Key(".",','), Key("/",'/'), Del(), None()},
};
constexpr KbCell kSym2Rows[3][kKbCols] = {
    {Key("=",'='), Key("<",'<'), Key(">",'>'), Key("[",'['), Key("]",']'),
     Key("{",'{'), Key("}",'}'), None(), None(), None()},
    {Key("\\",'\\'), Key("|",'|'), Key("`",'`'), Key("~",'~'), Key("'",'\''),
     Key("\"",'\"'), None(), None(), None(), None()},
    {Mode("abc"), None(), None(), None(), None(), None(), None(), None(),
     Del(), None()},
};
constexpr KbCell kDigitsRows[3][kKbCols] = {
    {Key("1",'1'), Key("2",'2'), Key("3",'3'), Key("4",'4'), Key("5",'5'),
     Key("6",'6'), Key("7",'7'), Key("8",'8'), Key("9",'9'), Key("0",'0')},
    {Key("-",'-'), Key("_",'_'), Key("/",'/'), Key(".",','), Key(":",':'),
     None(), None(), None(), None(), None()},
    {Mode("ABC"), None(), None(), None(), None(), None(), None(), None(),
     Del(), None()},
};

// Nav row (same for every mode): [SPC x4] [OK x2] [CAN x2], centered.
constexpr KbCell kNavRow[kKbCols] = {
    None(), Act(kActSpace, "SPC"), Act(kActSpace, "SPC"), Act(kActSpace, "SPC"),
    Act(kActSpace, "SPC"), Act(kActOk, "OK"), Act(kActOk, "OK"),
    Act(kActCancel, "CAN"), Act(kActCancel, "CAN"), None(),
};

// Editor state.
KbMode g_kb_mode = KB_LOWER;
bool g_kb_stop = false;   // uppercase/digits editor for stop codes
int g_kb_sel = 0;         // linear index over selectable cells (row-major)
char g_kb_text[65] = {};
int g_kb_len = 0;
char g_kb_title[24] = {};
char g_kb_sub[48] = {};

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

// Route list
char g_routes[12][8];
int g_route_count = 0;
int g_route_sel = 0;

bool g_active = false;
QueueHandle_t g_events = nullptr;

// ---------- keyboard helpers ----------

const KbCell* ModeRows(KbMode mode) {
    switch (mode) {
        case KB_LOWER: return &kLowerRows[0][0];
        case KB_UPPER: return &kUpperRows[0][0];
        case KB_SYM1: return &kSym1Rows[0][0];
        case KB_SYM2: return &kSym2Rows[0][0];
        case KB_DIGITS: return &kDigitsRows[0][0];
        default: return &kLowerRows[0][0];
    }
}

const KbCell* CellAt(int row, int col) {
    if (row < 3) {
        return &ModeRows(g_kb_mode)[row * kKbCols + col];
    }
    return &kNavRow[col];
}

bool CellSelectable(const KbCell* cell) {
    return cell->action != kActNone;
}

int SelectableCount() {
    int n = 0;
    for (int r = 0; r < kKbRows; r++) {
        for (int c = 0; c < kKbCols; c++) {
            if (CellSelectable(CellAt(r, c))) {
                n++;
            }
        }
    }
    return n;
}

void SelToCell(int sel, int* row, int* col) {
    int n = 0;
    for (int r = 0; r < kKbRows; r++) {
        for (int c = 0; c < kKbCols; c++) {
            if (CellSelectable(CellAt(r, c))) {
                if (n == sel) {
                    *row = r;
                    *col = c;
                    return;
                }
                n++;
            }
        }
    }
    *row = 0;
    *col = 0;
}

KbMode NextMode(KbMode mode) {
    if (g_kb_stop) {
        // Stop codes: letters <-> digits only.
        return mode == KB_UPPER ? KB_DIGITS : KB_UPPER;
    }
    return static_cast<KbMode>((mode + 1) % KB_MODE_COUNT);
}

void CycleMode() {
    g_kb_mode = NextMode(g_kb_mode);
    // Park the cursor on the mode key (row 2, col 0) of the new layout.
    int n = 0;
    for (int r = 0; r < kKbRows; r++) {
        for (int c = 0; c < kKbCols; c++) {
            if (!CellSelectable(CellAt(r, c))) continue;
            if (r == 2 && c == 0) {
                g_kb_sel = n;
            }
            n++;
        }
    }
}

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

void DrawKbCell(int row, int col, bool selected) {
    auto& c = BusDisplayCanvas();
    const KbCell* cell = CellAt(row, col);
    const int x = kKbX0 + col * kKbCellW;
    const int y = kKbY0 + row * kKbPitchY;
    if (cell->action == kActNone) {
        return;
    }
    if (selected) {
        c.FillRect(x, y, kKbCellW - 2, kKbCellH, true);
        c.Text(x + 8, y + 9, cell->label, 2, true);
    } else {
        c.Rect(x, y, kKbCellW - 2, kKbCellH, true);
        c.Text(x + 8, y + 9, cell->label, 2, false);
    }
}

void PaintKbPage(bool full) {
    auto& c = BusDisplayCanvas();
    Header(g_kb_title);
    c.Text(12, 48, g_kb_sub, 1, false);

    int sr, sc;
    SelToCell(g_kb_sel, &sr, &sc);
    for (int r = 0; r < kKbRows; r++) {
        for (int col = 0; col < kKbCols; col++) {
            if (CellSelectable(CellAt(r, col))) {
                DrawKbCell(r, col, r == sr && col == sc);
            }
        }
    }

    // Typed text (plain, so it can be checked) with a cursor underscore.
    char line[32];
    const int show = 24;
    const int start = g_kb_len > show ? g_kb_len - show : 0;
    std::snprintf(line, sizeof(line), "%.*s_", g_kb_len - start,
                  g_kb_text + start);
    c.Text(12, 246, line, 2, false);
    Hint("UP/DOWN=move  OK=pick  HOLD OK=done");
    if (full) BusDisplayPaint(); else BusDisplayPaintRows(kHeaderH, 300);
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

void StartKb(bool for_stop) {
    g_kb_stop = for_stop;
    g_kb_len = 0;
    g_kb_text[0] = '\0';
    g_kb_mode = for_stop ? KB_UPPER : KB_LOWER;
    g_kb_sel = 0;
    if (for_stop) {
        std::snprintf(g_kb_title, sizeof(g_kb_title), "BUS STOP");
        std::snprintf(g_kb_sub, sizeof(g_kb_sub), "CODE (E.G. T394)");
    } else {
        std::snprintf(g_kb_title, sizeof(g_kb_title), "WIFI PASSWORD");
        std::snprintf(g_kb_sub, sizeof(g_kb_sub), "FOR %s", g_chosen_ssid);
    }
    g_page = for_stop ? Page::StopEdit : Page::PassEdit;
    PaintKbPage(true);
}

void ExitToCard() {
    g_page = Page::Card;
    g_active = false;
    ESP_LOGI(kTag, "UI closed");
    if (g_on_exit != nullptr) {
        g_on_exit();
    }
}

// ---------- keyboard events ----------

// Submit the typed text (on-screen OK or physical HOLD OK).
void KbSubmit() {
    if (g_kb_len == 0) {
        ShowMessage("EMPTY - TYPE SOMETHING", g_page, 1500);
        return;
    }
    g_kb_text[g_kb_len] = '\0';
    if (g_kb_stop) {
        std::snprintf(g_config->stop, sizeof(g_config->stop), "%.7s",
                      g_kb_text);
        ConfirmStop();
    } else {
        std::snprintf(g_config->ssid, sizeof(g_config->ssid), "%.32s",
                      g_chosen_ssid);
        std::snprintf(g_config->password, sizeof(g_config->password), "%.64s",
                      g_kb_text);
        SaveAndReboot("wifi");
    }
}

void KbCancel() {
    if (g_kb_stop) {
        g_page = Page::Menu;
        PaintMenu(true);
    } else {
        g_page = Page::WifiList;
        PaintWifiList(true);
    }
}

void KbActivate() {
    int row, col;
    SelToCell(g_kb_sel, &row, &col);
    const KbCell* cell = CellAt(row, col);
    switch (cell->action) {
        case kActChar:
            if (g_kb_len < static_cast<int>(sizeof(g_kb_text) - 1)) {
                g_kb_text[g_kb_len++] = cell->ch;
                g_kb_text[g_kb_len] = '\0';
            }
            break;
        case kActMode:
            CycleMode();
            break;
        case kActDel:
            if (g_kb_len > 0) {
                g_kb_len--;
                g_kb_text[g_kb_len] = '\0';
            }
            break;
        case kActSpace:
            if (g_kb_len < static_cast<int>(sizeof(g_kb_text) - 1)) {
                g_kb_text[g_kb_len++] = ' ';
                g_kb_text[g_kb_len] = '\0';
            }
            break;
        case kActOk:
            KbSubmit();
            return;  // submit may have changed the page
        case kActCancel:
            KbCancel();
            return;
        default:
            break;
    }
    PaintKbPage(false);
}

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
                        StartKb(true);
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
                    StartKb(false);
                }
            }
            break;

        case Page::PassEdit:
        case Page::StopEdit:
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                KbSubmit();
                break;
            }
            if (ev.type == kPressClick) {
                if (ev.key == kKeyUp) {
                    g_kb_sel = (g_kb_sel + SelectableCount() - 1) %
                               SelectableCount();
                } else if (ev.key == kKeyDown) {
                    g_kb_sel = (g_kb_sel + 1) % SelectableCount();
                } else if (ev.key == kKeyOk) {
                    KbActivate();
                    break;  // KbActivate already repainted (or changed page)
                }
                PaintKbPage(false);
            }
            break;

        case Page::RouteList:
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                StartKb(true);
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
            } else if (g_page == Page::RouteList) {
                PaintRouteList(false);
            } else if (g_page == Page::PassEdit || g_page == Page::StopEdit) {
                PaintKbPage(false);
            }
            break;
    }
}

void RestoreAfterMessage() {
    g_page = g_msg_back;
    if (g_page == Page::Menu) {
        PaintMenu(true);
    } else if (g_page == Page::RouteList) {
        PaintRouteList(false);
    } else if (g_page == Page::PassEdit || g_page == Page::StopEdit) {
        PaintKbPage(false);
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
            RestoreAfterMessage();
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
