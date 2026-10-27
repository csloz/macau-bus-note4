#include "bus_display.h"

#include "esp_log.h"

namespace {

constexpr const char* kTag = "bus.disp";

HearthCanvas g_canvas;
zectrix_epd_handle_t g_epd = nullptr;

}  // namespace

void BusDisplayInit(zectrix_epd_handle_t epd) {
    g_epd = epd;
}

HearthCanvas& BusDisplayCanvas() {
    return g_canvas;
}

void BusDisplayPaint() {
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
    const esp_err_t err = zectrix_epd_refresh_full_1bpp(
        g_epd, g_canvas.data(), g_canvas.size());
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
    rect.y = y0;
    rect.width = HearthCanvas::kWidth;
    rect.height = y1 - y0;
    const uint8_t* pixels = g_canvas.data() + static_cast<size_t>(y0) * kStride;
    const esp_err_t err = zectrix_epd_refresh_partial_1bpp(
        g_epd, &rect, pixels, static_cast<size_t>(rect.height) * kStride);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "partial refresh failed: %s", esp_err_to_name(err));
    }
}
