// On-device settings UI for the NOTE4 bus board.
//
// Pages: card (main loop) -> menu -> wifi list -> password keyboard,
// and stop form (STOP + ROUTE dropdown fields, OK/CANCEL buttons).
// Chinese text renders via the embedded 16x16 CJK bitmap font.
#include "bus_ui.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "hearth_canvas.h"

#include "bus_api.h"
#include "bus_buttons.h"
#include "bus_config.h"
#include "bus_display.h"
#include "bus_stops_data.h"
#include "bus_wifi.h"
#include "cjk_font_16x16.h"

namespace {

constexpr const char* kTag = "bus.ui";
constexpr int kW = HearthCanvas::kWidth;
constexpr int kHeaderH = 40;

// List pages: up to 8 rows of scale-2 text below the header.
constexpr int kRowH = 32;
constexpr int kListY0 = kHeaderH;
constexpr int kListY1 = kListY0 + 8 * kRowH + 8;
constexpr int kRowsVisible = 8;

// ---------- UTF-8 / CJK text drawing ----------
//
// ASCII renders with the built-in 8x16 font; CJK codepoints (U+2E80..U+9FFF)
// render from the embedded 16x16 bitmap font, vertically centered on the
// scale-2 ASCII line (32 px). Everything else draws as a hollow box.

int DecodeCp(const char* s, int* len) {
    const uint8_t b = static_cast<uint8_t>(s[0]);
    if (b < 0x80) {
        *len = 1;
        return b;
    }
    if ((b & 0xE0) == 0xC0 && (static_cast<uint8_t>(s[1]) & 0xC0) == 0x80) {
        *len = 2;
        return ((b & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((b & 0xF0) == 0xE0 && (static_cast<uint8_t>(s[1]) & 0xC0) == 0x80 &&
        (static_cast<uint8_t>(s[2]) & 0xC0) == 0x80) {
        *len = 3;
        return ((b & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    *len = 1;
    return 0xFFFD;  // replacement character
}

void DrawCjkGlyph(int cp, int x, int y, bool inverted) {
    auto& c = BusDisplayCanvas();
    if (cp < kCjkFirst || cp > kCjkLast) {
        // Unknown codepoint: hollow box placeholder.
        c.Rect(x + 2, y + 8, 12, 12, !inverted);
        return;
    }
    const uint8_t* g =
        &kCjkGlyphs[(cp - kCjkFirst) * kCjkGlyphBytes];
    const int gy = y + 8;  // center the 16px glyph on the 32px text line
    for (int row = 0; row < 16; row++) {
        if (gy + row < 0 || gy + row >= HearthCanvas::kHeight) continue;
        const uint8_t hi = g[row * 2];
        const uint8_t lo = g[row * 2 + 1];
        if (hi == 0 && lo == 0) continue;
        for (int bit = 0; bit < 16; bit++) {
            const int px = x + bit;
            if (px < 0 || px >= HearthCanvas::kWidth) continue;
            const bool ink =
                (bit < 8) ? ((hi >> (7 - bit)) & 1) : ((lo >> (15 - bit)) & 1);
            if (ink) {
                c.Pixel(px, gy + row, !inverted);
            }
        }
    }
}

// Draw a UTF-8 string (ASCII scale-2 + CJK 16x16) starting at (x, y).
// Truncates after max_px pixels. Returns the end x.
int TextUtf8(int x, int y, const char* s, int max_px, bool inverted) {
    auto& c = BusDisplayCanvas();
    int i = 0;
    while (s[i] != '\0') {
        int len = 0;
        const int cp = DecodeCp(s + i, &len);
        if (x + 16 > max_px) break;
        if (cp < 0x80) {
            char buf[2] = {static_cast<char>(cp), '\0'};
            c.Text(x, y, buf, 2, inverted);
        } else {
            DrawCjkGlyph(cp, x, y, inverted);
        }
        x += 16;
        i += len;
    }
    return x;
}

// Scale-1 variant: ASCII 8px wide, CJK 16px wide (16px tall, no centering).
int TextUtf8Small(int x, int y, const char* s, int max_px) {
    auto& c = BusDisplayCanvas();
    int i = 0;
    while (s[i] != '\0') {
        int len = 0;
        const int cp = DecodeCp(s + i, &len);
        if (cp < 0x80) {
            if (x + 8 > max_px) break;
            char buf[2] = {static_cast<char>(cp), '\0'};
            c.Text(x, y, buf, 1, false);
            x += 8;
        } else {
            if (x + 16 > max_px) break;
            const uint8_t* g = (cp >= kCjkFirst && cp <= kCjkLast)
                                   ? &kCjkGlyphs[(cp - kCjkFirst) * kCjkGlyphBytes]
                                   : nullptr;
            if (g != nullptr) {
                for (int row = 0; row < 16; row++) {
                    if (y + row >= HearthCanvas::kHeight) break;
                    const uint8_t hi = g[row * 2];
                    const uint8_t lo = g[row * 2 + 1];
                    for (int bit = 0; bit < 16; bit++) {
                        const int px = x + bit;
                        if (px >= HearthCanvas::kWidth) break;
                        const bool ink = (bit < 8) ? ((hi >> (7 - bit)) & 1)
                                                   : ((lo >> (15 - bit)) & 1);
                        if (ink) {
                            c.Pixel(px, y + row, true);
                        }
                    }
                }
            } else {
                c.Rect(x + 1, y + 2, 14, 12, true);
            }
            x += 16;
        }
        i += len;
    }
    return x;
}

// ---------- keyboard cell model ----------
constexpr int kKbCols = 10;
constexpr int kKbRows = 4;          // 3 char rows + nav row
constexpr int kKbCellW = 32;
constexpr int kKbCellH = 34;
constexpr int kKbPitchY = 36;
constexpr int kKbX0 = (kW - kKbCols * kKbCellW) / 2;  // 40
constexpr int kKbY0 = 84;

constexpr uint8_t kActChar = 0;
constexpr uint8_t kActMode = 1;
constexpr uint8_t kActDel = 2;
constexpr uint8_t kActSpace = 3;
constexpr uint8_t kActOk = 4;
constexpr uint8_t kActCancel = 5;
constexpr uint8_t kActNone = 6;

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

enum KbMode { KB_LOWER = 0, KB_UPPER, KB_SYM1, KB_SYM2, KB_MODE_COUNT };

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

// Nav row: [SPC x4] [OK x2] [CAN x2] centered, fillers at the edges.
constexpr KbCell kNavRow[kKbCols] = {
    None(), Act(kActSpace, "SPC"), Act(kActSpace, "SPC"), Act(kActSpace, "SPC"),
    Act(kActSpace, "SPC"), Act(kActOk, "OK"), Act(kActOk, "OK"),
    Act(kActCancel, "CAN"), Act(kActCancel, "CAN"), None(),
};

// Editor state (keyboard is used for the Wi-Fi password only).
KbMode g_kb_mode = KB_LOWER;
int g_kb_sel = 0;        // linear index over selectable cells (row-major)
char g_kb_text[65] = {};
int g_kb_len = 0;

enum class Page {
    Card,        // normal display (owned by the main loop)
    Menu,
    WifiList,
    PassEdit,    // on-screen keyboard
    StopForm,    // STOP + ROUTE dropdown fields with OK / CANCEL buttons
    StopList,    // dropdown: all stops
    RouteList,   // dropdown: routes at the chosen stop
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

// Stop form: item order = STOP field, ROUTE field, OK, CANCEL.
int g_form_sel = 0;
constexpr int kFormItemStop = 0;
constexpr int kFormItemRoute = 1;
constexpr int kFormItemOk = 2;
constexpr int kFormItemCancel = 3;
constexpr int kFormItemCount = 4;
char g_form_route[8] = {};  // route chosen in the dropdown, shown in the field

// Stop list (dropdown over all stops, sorted by code).
int g_stop_order[512];
int g_stop_count = 0;
int g_stop_sel = 0;   // 0 = <BACK>, 1.. = g_stop_order[sel-1]

// Route list (dropdown over routes at the chosen stop).
char g_routes[12][8];
int g_route_count = 0;
int g_route_sel = 0;  // 0 = <BACK>, 1.. = g_routes[sel-1]

bool g_active = false;
QueueHandle_t g_events = nullptr;

// ---------- keyboard helpers ----------

const KbCell* ModeRows(KbMode mode) {
    switch (mode) {
        case KB_LOWER: return &kLowerRows[0][0];
        case KB_UPPER: return &kUpperRows[0][0];
        case KB_SYM1: return &kSym1Rows[0][0];
        case KB_SYM2: return &kSym2Rows[0][0];
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

void CycleMode() {
    g_kb_mode = static_cast<KbMode>((g_kb_mode + 1) % KB_MODE_COUNT);
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
    BusDisplayCanvas().Text(12, 284, text, 1, false);
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
    char rssi[8];
    for (int row = 0; row < kRowsVisible && first + row < g_ap_count; row++) {
        const int idx = first + row;
        const BusWifiAp& ap = g_aps[idx];
        const int y = kListY0 + row * kRowH;
        const bool inv = idx == g_wifi_sel;
        if (inv) {
            c.FillRect(0, y, kW, kRowH, true);
        }
        // RSSI pinned to the right edge; SSID gets the rest, truncated
        // with a ".." marker when it does not fit.
        std::snprintf(rssi, sizeof(rssi), "%d", ap.rssi);
        const int rx = kW - 12 - c.TextWidth(rssi, 2);
        const int max_w = rx - 44;
        const int end = TextUtf8(12, y, ap.ssid, max_w, inv);
        if (ap.ssid[0] != '\0' && end >= max_w) {
            c.Text(rx - 40, y, "..", 2, inv);
        }
        c.Text(rx, y, rssi, 2, inv);
    }
    Hint("OK=select  HOLD OK=back");
    if (full) BusDisplayPaint(); else BusDisplayPaintRows(kListY0, kListY1);
}

void DrawKbCell(int row, int col, bool selected) {
    auto& c = BusDisplayCanvas();
    const KbCell* cell = CellAt(row, col);
    if (cell->action == kActNone) {
        return;
    }
    const int x = kKbX0 + col * kKbCellW;
    const int y = kKbY0 + row * kKbPitchY;
    const int bw = kKbCellW - 2;
    if (selected) {
        c.FillRect(x, y, bw, kKbCellH, true);
    } else {
        c.Rect(x, y, bw, kKbCellH, true);
    }
    // Center the label. Multi-char labels render at scale 1 so "CAN",
    // "SPC", "DEL" and mode keys fit inside a 30px cell with breathing
    // room; single characters stay at scale 2.
    const int scale = c.TextWidth(cell->label, 2) > bw - 6 ? 1 : 2;
    const int lw = c.TextWidth(cell->label, scale);
    const int tx = x + (bw - lw) / 2;
    const int ty = y + (kKbCellH - (scale == 2 ? 16 : 8)) / 2;
    c.Text(tx, ty, cell->label, scale, selected);
}

void PaintKbPage(bool full) {
    auto& c = BusDisplayCanvas();
    Header("WIFI PASSWORD");

    // Full network name (ASCII + CJK), wrapped under the header.
    c.Text(12, 44, "FOR:", 1, false);
    int nx = 44;
    int ny = 44;
    const char* s = g_chosen_ssid;
    int i = 0;
    while (s[i] != '\0') {
        int len = 0;
        const int cp = DecodeCp(s + i, &len);
        const int w = cp < 0x80 ? 8 : 16;
        if (nx + w > kW - 12) {
            nx = 44;
            ny += 14;
            if (ny > 70) break;  // two lines is enough; the rest is clipped
        }
        if (cp < 0x80) {
            char buf[2] = {s[i], '\0'};
            c.Text(nx, ny, buf, 1, false);
            nx += 8;
        } else {
            TextUtf8Small(nx, ny, s + i, nx + 16);
            nx += 16;
        }
        i += len;
    }

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

// ---------- stop form (dropdowns + OK / CANCEL) ----------

void PaintStopForm(bool full) {
    auto& c = BusDisplayCanvas();
    Header("BUS STOP");

    const bool stop_sel = g_form_sel == kFormItemStop;
    const bool route_sel = g_form_sel == kFormItemRoute;

    // STOP field
    c.Text(16, 54, "STOP", 2, false);
    if (stop_sel) {
        c.FillRect(130, 48, 140, 36, true);
    } else {
        c.Rect(130, 48, 140, 36, true);
    }
    c.Text(142, 54, g_config->stop[0] ? g_config->stop : "--", 2, stop_sel);
    c.Text(278, 54, "v", 2, false);

    // ROUTE field
    c.Text(16, 102, "ROUTE", 2, false);
    if (route_sel) {
        c.FillRect(130, 96, 140, 36, true);
    } else {
        c.Rect(130, 96, 140, 36, true);
    }
    c.Text(142, 102, g_form_route[0] ? g_form_route : "--", 2, route_sel);
    c.Text(278, 102, "v", 2, false);

    // Chinese name of the chosen stop.
    for (int i = 0; i < g_stop_count; i++) {
        const auto& st = bus_stops::kStops[g_stop_order[i]];
        if (std::strcmp(st.code, g_config->stop) == 0) {
            c.Text(16, 146, "NAME:", 1, false);
            TextUtf8Small(60, 142, st.name, kW - 16);
            break;
        }
    }

    // OK / CANCEL buttons
    const bool ok_sel = g_form_sel == kFormItemOk;
    const bool can_sel = g_form_sel == kFormItemCancel;
    if (ok_sel) {
        c.FillRect(60, 196, 110, 44, true);
    } else {
        c.Rect(60, 196, 110, 44, true);
    }
    c.Text(60 + (110 - c.TextWidth("OK", 2)) / 2, 202, "OK", 2, ok_sel);
    if (can_sel) {
        c.FillRect(200, 196, 150, 44, true);
    } else {
        c.Rect(200, 196, 150, 44, true);
    }
    c.Text(200 + (150 - c.TextWidth("CANCEL", 1)) / 2, 214, "CANCEL", 1,
           can_sel);

    Hint("UP/DOWN=field  OK=open/act");
    if (full) BusDisplayPaint(); else BusDisplayPaintRows(kHeaderH, 300);
}

void PaintStopList(bool full) {
    auto& c = BusDisplayCanvas();
    Header("PICK STOP");
    const int total = g_stop_count + 1;  // + <BACK>
    int first = g_stop_sel - (kRowsVisible - 1);
    if (first < 0) first = 0;
    if (total > kRowsVisible && first > total - kRowsVisible) {
        first = total - kRowsVisible;
    }
    for (int row = 0; row < kRowsVisible && first + row < total; row++) {
        const int idx = first + row;
        const int y = kListY0 + row * kRowH;
        const bool sel = idx == g_stop_sel;
        if (sel) {
            c.FillRect(0, y, kW, kRowH, true);
        }
        if (idx == 0) {
            c.Text(12, y, "<BACK>", 2, sel);
            continue;
        }
        const auto& st = bus_stops::kStops[g_stop_order[idx - 1]];
        c.Text(12, y, st.code, 2, sel);
        TextUtf8(92, y, st.name, kW - 12, sel);
    }
    Hint("OK=pick  HOLD OK=back");
    if (full) BusDisplayPaint(); else BusDisplayPaintRows(kListY0, 300);
}

void PaintRouteList(bool full) {
    auto& c = BusDisplayCanvas();
    Header("PICK ROUTE");
    char sub[48];
    std::snprintf(sub, sizeof(sub), "ROUTES AT %s", g_config->stop);
    c.Text(12, kHeaderH + 4, sub, 1, false);
    const int total = g_route_count + 1;  // + <BACK>
    int first = g_route_sel - (kRowsVisible - 1);
    if (first < 0) first = 0;
    if (total > kRowsVisible && first > total - kRowsVisible) {
        first = total - kRowsVisible;
    }
    for (int i = first; i < total; i++) {
        const int row = i - first;
        if (row >= kRowsVisible) break;
        const int y = kListY0 + 24 + row * kRowH;
        const bool sel = i == g_route_sel;
        if (i == 0) {
            c.Text(12, y, "<BACK>", 2, sel);
        } else {
            c.Text(24, y, g_routes[i - 1], 2, sel);
        }
    }
    Hint("OK=pick  HOLD OK=back");
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

void RestoreAfterMessage() {
    g_page = g_msg_back;
    switch (g_page) {
        case Page::Menu: PaintMenu(true); break;
        case Page::WifiList: PaintWifiList(false); break;
        case Page::PassEdit: PaintKbPage(false); break;
        case Page::StopForm: PaintStopForm(false); break;
        case Page::StopList: PaintStopList(false); break;
        case Page::RouteList: PaintRouteList(false); break;
        default: PaintMenu(true); break;
    }
}

// ---------- actions ----------

void SaveAndReboot(const char* what) {
    BusConfigSave(*g_config);
    ESP_LOGI(kTag, "%s saved from UI; rebooting", what);
    ShowMessage("SAVED - REBOOTING...", Page::Menu, 10000);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

// Load the routes serving g_config->stop; default the form's route to the
// configured route when it serves the stop, else the first route.
void LoadRoutesForStop() {
    g_route_count = BusApiStopRoutes(g_config->stop, g_routes, 12);
    if (g_route_count < 0) {
        g_route_count = 0;
    }
    g_form_route[0] = '\0';
    if (g_route_count == 0) {
        return;
    }
    std::snprintf(g_form_route, sizeof(g_form_route), "%.7s", g_routes[0]);
    for (int i = 0; i < g_route_count; i++) {
        if (std::strcmp(g_routes[i], g_config->route) == 0) {
            std::snprintf(g_form_route, sizeof(g_form_route), "%.7s",
                          g_config->route);
            break;
        }
    }
}

void SortStopOrder() {
    g_stop_count = static_cast<int>(bus_stops::kStopCount);
    if (g_stop_count >
        static_cast<int>(sizeof(g_stop_order) / sizeof(g_stop_order[0]))) {
        g_stop_count =
            static_cast<int>(sizeof(g_stop_order) / sizeof(g_stop_order[0]));
    }
    for (int i = 0; i < g_stop_count; i++) {
        g_stop_order[i] = i;
    }
    // Insertion sort by stop code (n=451, done once at boot).
    for (int i = 1; i < g_stop_count; i++) {
        const int key = g_stop_order[i];
        int j = i - 1;
        while (j >= 0 &&
               std::strcmp(bus_stops::kStops[g_stop_order[j]].code,
                           bus_stops::kStops[key].code) > 0) {
            g_stop_order[j + 1] = g_stop_order[j];
            j--;
        }
        g_stop_order[j + 1] = key;
    }
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
    std::snprintf(g_chosen_ssid, sizeof(g_chosen_ssid), "%.32s",
                  g_aps[g_wifi_sel].ssid);
    g_kb_len = 0;
    g_kb_text[0] = '\0';
    g_kb_mode = KB_LOWER;
    g_kb_sel = 0;
    g_page = Page::PassEdit;
    PaintKbPage(true);
}

void StartStopForm() {
    g_form_sel = kFormItemStop;
    std::snprintf(g_form_route, sizeof(g_form_route), "%.7s", g_config->route);
    g_page = Page::StopForm;
    PaintStopForm(true);
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

void KbSubmit() {
    // Require at least one non-space character before saving.
    bool has_text = false;
    for (int i = 0; i < g_kb_len; i++) {
        if (g_kb_text[i] != ' ') {
            has_text = true;
            break;
        }
    }
    if (g_kb_len == 0 || !has_text) {
        ShowMessage("EMPTY - TYPE SOMETHING", Page::PassEdit, 1500);
        return;
    }
    g_kb_text[g_kb_len] = '\0';
    std::snprintf(g_config->ssid, sizeof(g_config->ssid), "%.32s",
                  g_chosen_ssid);
    std::snprintf(g_config->password, sizeof(g_config->password), "%.64s",
                  g_kb_text);
    SaveAndReboot("wifi");
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
            g_page = Page::WifiList;
            PaintWifiList(true);
            return;
        default:
            break;
    }
    PaintKbPage(false);
}

void OpenStopList() {
    g_stop_sel = 0;
    // Preselect the currently configured stop.
    for (int i = 0; i < g_stop_count; i++) {
        if (std::strcmp(bus_stops::kStops[g_stop_order[i]].code,
                        g_config->stop) == 0) {
            g_stop_sel = i + 1;
            break;
        }
    }
    g_page = Page::StopList;
    PaintStopList(true);
}

void OpenRouteList() {
    if (g_route_count <= 0) {
        ShowMessage("NO ROUTES (OFFLINE?)", Page::StopForm, 1800);
        return;
    }
    g_route_sel = 1;  // first route (index 0 is <BACK>)
    for (int i = 0; i < g_route_count; i++) {
        if (std::strcmp(g_routes[i], g_form_route) == 0) {
            g_route_sel = i + 1;
            break;
        }
    }
    g_page = Page::RouteList;
    PaintRouteList(true);
}

void StopFormActivate() {
    switch (g_form_sel) {
        case kFormItemStop:
            OpenStopList();
            break;
        case kFormItemRoute:
            LoadRoutesForStop();
            if (g_route_count > 0) {
                OpenRouteList();
            } else {
                ShowMessage("NO ROUTES (OFFLINE?)", Page::StopForm, 1800);
            }
            break;
        case kFormItemOk:
            if (g_config->stop[0] == '\0') {
                ShowMessage("PICK A STOP FIRST", Page::StopForm, 1500);
                break;
            }
            if (g_form_route[0] == '\0') {
                ShowMessage("PICK A ROUTE FIRST", Page::StopForm, 1500);
                break;
            }
            std::snprintf(g_config->route, sizeof(g_config->route), "%.7s",
                          g_form_route);
            SaveAndReboot("stop/route");
            break;
        case kFormItemCancel:
            g_page = Page::Menu;
            PaintMenu(true);
            break;
        default:
            break;
    }
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
                        StartStopForm();
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

        case Page::PassEdit:
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

        case Page::StopForm:
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                g_page = Page::Menu;
                PaintMenu(true);
                break;
            }
            if (ev.type == kPressClick) {
                if (ev.key == kKeyUp) {
                    g_form_sel = (g_form_sel + kFormItemCount - 1) %
                                 kFormItemCount;
                } else if (ev.key == kKeyDown) {
                    g_form_sel = (g_form_sel + 1) % kFormItemCount;
                } else if (ev.key == kKeyOk) {
                    StopFormActivate();
                    break;
                }
                PaintStopForm(false);
            }
            break;

        case Page::StopList: {
            const int total = g_stop_count + 1;
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                g_page = Page::StopForm;
                PaintStopForm(true);
                break;
            }
            if (ev.type == kPressClick) {
                if (ev.key == kKeyUp) {
                    g_stop_sel = (g_stop_sel + total - 1) % total;
                } else if (ev.key == kKeyDown) {
                    g_stop_sel = (g_stop_sel + 1) % total;
                } else if (ev.key == kKeyOk) {
                    if (g_stop_sel == 0) {
                        g_page = Page::StopForm;
                        PaintStopForm(true);
                    } else {
                        std::snprintf(
                            g_config->stop, sizeof(g_config->stop), "%.7s",
                            bus_stops::kStops[g_stop_order[g_stop_sel - 1]].code);
                        g_form_sel = kFormItemRoute;
                        LoadRoutesForStop();
                        g_page = Page::StopForm;
                        PaintStopForm(true);
                    }
                    break;
                }
                PaintStopList(false);
            }
            break;
        }

        case Page::RouteList: {
            const int total = g_route_count + 1;
            if (ev.key == kKeyOk && ev.type == kPressLong) {
                g_page = Page::StopForm;
                PaintStopForm(true);
                break;
            }
            if (ev.type == kPressClick) {
                if (ev.key == kKeyUp) {
                    g_route_sel = (g_route_sel + total - 1) % total;
                } else if (ev.key == kKeyDown) {
                    g_route_sel = (g_route_sel + 1) % total;
                } else if (ev.key == kKeyOk) {
                    if (g_route_sel == 0) {
                        g_page = Page::StopForm;
                        PaintStopForm(true);
                    } else {
                        std::snprintf(g_form_route, sizeof(g_form_route),
                                      "%.7s", g_routes[g_route_sel - 1]);
                        g_form_sel = kFormItemOk;
                        g_page = Page::StopForm;
                        PaintStopForm(true);
                    }
                    break;
                }
                PaintRouteList(false);
            }
            break;
        }

        case Page::Message:
            RestoreAfterMessage();
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

void BusUiOpenMenu() {
    if (g_active || g_events == nullptr) {
        return;
    }
    g_active = true;
    g_menu_sel = 0;
    g_page = Page::Menu;
    PaintMenu(true);
    ESP_LOGI(kTag, "UI opened via console");
}

void BusUiInjectKey(int key, bool long_press) {
    if (g_events == nullptr) {
        return;
    }
    BusKeyEvent ev = {};
    switch (key) {
        case 0: ev.key = kKeyUp; break;
        case 1: ev.key = kKeyDown; break;
        case 2: ev.key = kKeyOk; break;
        default: return;
    }
    ev.type = long_press ? kPressLong : kPressClick;
    Emit(ev);
}

void BusUiStart(BusConfig* config, void (*on_exit)()) {
    g_config = config;
    g_on_exit = on_exit;
    g_events = xQueueCreate(8, sizeof(BusKeyEvent));
    if (g_events == nullptr) {
        ESP_LOGE(kTag, "queue alloc failed");
        return;
    }
    SortStopOrder();
    BusButtonsStart(Emit);
    if (xTaskCreate(UiTask, "bus_ui", 6144, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "failed to start UI task");
    }
}

bool BusUiActive() {
    return g_active;
}
