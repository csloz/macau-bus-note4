// Battery voltage + charge state for the ZECTRIX NOTE4 board.
#pragma once

#include <cstdint>

struct BusPowerState {
    bool valid = false;      // ADC read succeeded
    bool charging = false;   // charger detect pin active
    bool full = false;       // charge-full pin active
    uint16_t mv = 0;         // battery voltage, millivolts
    uint8_t percent = 0;     // estimated charge 0-100
};

// Configure the battery ADC (ADC1_CH3, GPIO4, voltage divided by 2 on board)
// and the charge status inputs (GPIO2 detect active-low, GPIO1 full).
void BusPowerInit();

// Read battery state. Returns false when the ADC is unavailable or reads 0.
bool BusPowerRead(BusPowerState* out);
