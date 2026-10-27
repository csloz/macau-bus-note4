#include "bus_buttons.h"

#include <cstdint>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zectrix_button_logic.h"

namespace {

constexpr const char* kTag = "bus.btn";

constexpr gpio_num_t kPinUp = GPIO_NUM_39;
constexpr gpio_num_t kPinDown = GPIO_NUM_18;
constexpr gpio_num_t kPinOk = GPIO_NUM_0;

// Active-low keys (pressed = level 0).
inline bool RawPressed(gpio_num_t pin) {
    return gpio_get_level(pin) == 0;
}

// Auto-repeat timing for UP/DOWN.
constexpr uint32_t kRepeatDelayMs = 500;   // hold before the first repeat
constexpr uint32_t kRepeatEveryMs = 130;   // repeat cadence while held

struct RepeatState {
    bool stable_pressed = false;
    bool sample_pressed = false;
    uint32_t sample_started_ms = 0;
    uint32_t press_started_ms = 0;
    uint32_t last_repeat_ms = 0;
    bool repeating = false;
    bool armed = true;
};

// Debounced press state with click + auto-repeat (no long-press event).
void SampleRepeat(RepeatState& st, uint32_t now_ms, bool pressed,
                  BusKey key, void (*emit)(BusKeyEvent)) {
    bool fire = false;
    if (pressed != st.sample_pressed) {
        st.sample_pressed = pressed;
        st.sample_started_ms = now_ms;
    }
    if (pressed != st.stable_pressed &&
        now_ms - st.sample_started_ms >= 40) {  // debounce
        st.stable_pressed = pressed;
        if (pressed) {
            if (st.armed) {
                st.press_started_ms = st.sample_started_ms;
                st.repeating = false;
            }
        } else if (st.armed) {
            if (!st.repeating) {
                fire = true;  // short click
            }
        }
    } else if (pressed && st.stable_pressed && st.armed) {
        if (!st.repeating &&
            now_ms - st.press_started_ms >= kRepeatDelayMs) {
            st.repeating = true;
            st.last_repeat_ms = now_ms;
            fire = true;
        } else if (st.repeating &&
                   now_ms - st.last_repeat_ms >= kRepeatEveryMs) {
            st.last_repeat_ms = now_ms;
            fire = true;
        }
    }
    if (fire) {
        BusKeyEvent e = {};
        e.key = key;
        e.type = kPressClick;
        emit(e);
    }
}

void ButtonTask(void* arg) {
    auto emit = reinterpret_cast<void (*)(BusKeyEvent)>(arg);

    // OK uses the shared Hearth click/long-press tracker.
    zectrix::ButtonTracker ok_tracker;
    zectrix::ButtonTiming ok_timing;
    ok_timing.long_press_ms = 600;
    zectrix::ResetButtonTracker(ok_tracker, 0, RawPressed(kPinOk));

    RepeatState up{};
    RepeatState down{};
    up.sample_pressed = up.stable_pressed = RawPressed(kPinUp);
    down.sample_pressed = down.stable_pressed = RawPressed(kPinDown);
    up.armed = !up.stable_pressed;
    down.armed = !down.stable_pressed;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const uint32_t now_ms =
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        const zectrix::ButtonEvent ok =
            zectrix::TrackButtonPress(ok_tracker, ok_timing, now_ms,
                                      RawPressed(kPinOk));
        if (ok == zectrix::ButtonEvent::kClick) {
            BusKeyEvent e = {kKeyOk, kPressClick};
            emit(e);
        } else if (ok == zectrix::ButtonEvent::kLongPress) {
            BusKeyEvent e = {kKeyOk, kPressLong};
            emit(e);
        }

        SampleRepeat(up, now_ms, RawPressed(kPinUp), kKeyUp, emit);
        SampleRepeat(down, now_ms, RawPressed(kPinDown), kKeyDown, emit);
    }
}

}  // namespace

void BusButtonsStart(void (*on_event)(BusKeyEvent event)) {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << kPinUp) | (1ULL << kPinDown) | (1ULL << kPinOk);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    // GPIO39 (input-only) has no internal pull-up; the board provides one.
    gpio_config(&io);

    if (xTaskCreate(ButtonTask, "bus_btn", 3072,
                    reinterpret_cast<void*>(on_event), 5, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "failed to start button task");
    }
}
