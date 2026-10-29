#include "bus_power.h"

#include <algorithm>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "driver/gpio.h"

namespace {

constexpr const char* kTag = "bus.pwr";

// Board fact sheet (vendor demo components/zectrix_board): the pack voltage
// goes through a /2 divider into ADC1_CH3 (GPIO4), charger detect is GPIO2
// (active low) and charge-full is GPIO1 (active high).
constexpr adc_channel_t kBatteryAdcChannel = ADC_CHANNEL_3;
constexpr gpio_num_t kChargeDetect = GPIO_NUM_2;
constexpr gpio_num_t kChargeFull = GPIO_NUM_1;

adc_oneshot_unit_handle_t g_adc = nullptr;
adc_cali_handle_t g_cali = nullptr;
bool g_inited = false;

}  // namespace

void BusPowerInit() {
    gpio_config_t cfg = {};
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pin_bit_mask = (1ULL << kChargeDetect) | (1ULL << kChargeFull);
    gpio_config(&cfg);

    adc_oneshot_unit_init_cfg_t unit = {};
    unit.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit, &g_adc) != ESP_OK) {
        g_adc = nullptr;
        ESP_LOGW(kTag, "battery ADC init failed");
        return;
    }
    adc_oneshot_chan_cfg_t chan = {};
    chan.atten = ADC_ATTEN_DB_12;
    chan.bitwidth = ADC_BITWIDTH_12;
    if (adc_oneshot_config_channel(g_adc, kBatteryAdcChannel, &chan) != ESP_OK) {
        ESP_LOGW(kTag, "battery ADC channel config failed");
        return;
    }
    adc_cali_curve_fitting_config_t cali = {};
    cali.unit_id = ADC_UNIT_1;
    cali.atten = ADC_ATTEN_DB_12;
    cali.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cali, &g_cali) != ESP_OK) {
        g_cali = nullptr;
        ESP_LOGW(kTag, "battery ADC calibration unavailable");
        return;
    }
    g_inited = true;
}

bool BusPowerRead(BusPowerState* out) {
    *out = {};
    if (!g_inited || g_adc == nullptr || g_cali == nullptr) {
        return false;
    }
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        int raw = 0;
        int mv = 0;
        if (adc_oneshot_read(g_adc, kBatteryAdcChannel, &raw) != ESP_OK ||
            adc_cali_raw_to_voltage(g_cali, raw, &mv) != ESP_OK) {
            return false;
        }
        sum += mv * 2;  // undo the board's /2 divider
    }
    const int avg = sum / 10;
    if (avg <= 0) {
        return false;
    }
    // Vendor percentage curve (parabola fitted to the pack discharge).
    const int pct = (-avg * avg + 9016 * avg - 19189000) / 10000;
    out->valid = true;
    out->mv = static_cast<uint16_t>(avg);
    out->percent = static_cast<uint8_t>(std::clamp(pct, 0, 100));
    out->charging = gpio_get_level(kChargeDetect) == 0;
    out->full = gpio_get_level(kChargeFull) == 1;
    return true;
}
