// NVS-backed Wi-Fi credentials + serial console provisioning.
#pragma once

#include "esp_err.h"

#include <cstdint>

struct BusConfig {
    char ssid[33] = {};
    char password[65] = {};
    char stop[8] = {};    // DSAT stop code to monitor (e.g. "T394")
    char route[8] = {};   // hero route shown in the header (e.g. "51A")
    int flip = 0;         // 1 = display rotated 180deg, UP/DOWN swapped
    int fast = 0;         // 1 = fast e-paper waveform (snappier, more ghosting)
};

esp_err_t BusConfigLoad(BusConfig* out);
esp_err_t BusConfigSave(const BusConfig& config);
bool BusConfigComplete(const BusConfig& config);

// Starts a console REPL task (non-blocking): bus-set / bus-save / bus-show /
// bus-now / bus-reboot. Pass callbacks so the console can trigger actions.
struct BusConsoleHooks {
    void (*refresh_now)();
};

void BusConfigRegisterConsole(BusConfig* config, BusConsoleHooks hooks);
