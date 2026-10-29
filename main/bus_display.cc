#include "bus_display.h"

#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr const char* kTag = "bus.disp";

HearthCanvas g_canvas;
zectrix_epd_handle_t g_epd = nullptr;

bool g_flip = false;
bool g_fast = false;

// Scratch buffer for the 180-degree-rotated image (full frame or band).
constexpr size_t kCanvasBytes = HearthCanvas::kWidth / 8 * HearthCanvas::kHeight;
uint8_t g_rotated[kCanvasBytes];

uint8_t RevBits(uint8_t b) {
    b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

}  // namespace

void BusDisplaySetFlip(bool flip) {
    g_flip = flip;
}

bool BusDisplayGetFlip() {
    return g_flip;
}

void BusDisplaySetFast(bool fast) {
    g_fast = fast;
}

bool BusDisplayGetFast() {
    return g_fast;
}

void BusDisplayInit(zectrix_epd_handle_t epd) {
    g_epd = epd;
}

HearthCanvas& BusDisplayCanvas() {
    return g_canvas;
}

void BusDisplayPaint(bool fast_ok) {
    if (g_epd == nullptr) {
        return;
    }
    if (!zectrix_epd_is_powered(g_epd)) {
        const esp_err_t on = zectrix_epd_power_on(g_epd);
        if (on != ESP_OK) {
            ESP_LOGE(kTag, "epd power on failed: %s", esp_err_to_name(on));
            return;
        }
    }
    const uint8_t* in = g_canvas.data();
    const size_t n = g_canvas.size();
    const uint8_t* frame = in;
    if (g_flip) {
        // 180 degrees = reverse the byte stream and reverse bits per byte:
        // pixel (x, y) ends up at (W-1-x, H-1-y).
        for (size_t i = 0; i < n; i++) {
            g_rotated[n - 1 - i] = RevBits(in[i]);
        }
        frame = g_rotated;
    }
    const int64_t t0 = esp_timer_get_time();
    const bool use_fast = g_fast && fast_ok;
    const esp_err_t err =
        use_fast ? zectrix_epd_refresh_full_fast_1bpp(g_epd, frame, n)
                 : zectrix_epd_refresh_full_1bpp(g_epd, frame, n);
    ESP_LOGI(kTag, "%s full refresh: %lld ms (%s)", use_fast ? "fast" : "otp",
             static_cast<long long>((esp_timer_get_time() - t0) / 1000),
             err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "refresh failed: %s", esp_err_to_name(err));
    }
}

void BusDisplayPaintRows(int y0, int y1) {
    if (g_epd == nullptr || y1 <= y0) {
        return;
    }
    if (!zectrix_epd_is_powered(g_epd)) {
        BusDisplayPaint();
        return;
    }
    constexpr int kStride = HearthCanvas::kWidth / 8;
    if (y0 < 0) y0 = 0;
    if (y1 > HearthCanvas::kHeight) y1 = HearthCanvas::kHeight;
    zectrix_epd_rect_t rect = {};
    rect.x = 0;
    rect.width = HearthCanvas::kWidth;
    rect.height = y1 - y0;
    const uint8_t* pixels =
        g_canvas.data() + static_cast<size_t>(y0) * kStride;
    if (g_flip) {
        // The canvas band [y0, y1) lands on panel rows [H-y1, H-y0), with
        // row order reversed and each row's bytes bit-reversed.
        rect.y = HearthCanvas::kHeight - y1;
        for (int r = 0; r < rect.height; r++) {
            const uint8_t* src =
                g_canvas.data() + static_cast<size_t>(y1 - 1 - r) * kStride;
            uint8_t* dst = g_rotated + static_cast<size_t>(r) * kStride;
            for (int b = 0; b < kStride; b++) {
                dst[b] = RevBits(src[kStride - 1 - b]);
            }
        }
        pixels = g_rotated;
    } else {
        rect.y = y0;
    }
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err =
        g_fast ? zectrix_epd_refresh_partial_fast_1bpp(
                     g_epd, &rect, pixels,
                     static_cast<size_t>(rect.height) * kStride)
               : zectrix_epd_refresh_partial_1bpp(
                     g_epd, &rect, pixels,
                     static_cast<size_t>(rect.height) * kStride);
    if (g_fast) {
        ESP_LOGI(kTag, "fast partial %d rows: %lld ms (%s)", rect.height,
                 static_cast<long long>((esp_timer_get_time() - t0) / 1000),
                 err == ESP_OK ? "ok" : esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "partial refresh failed: %s", esp_err_to_name(err));
    }
}
