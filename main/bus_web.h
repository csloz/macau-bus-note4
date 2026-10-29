// Minimal web settings portal served from the device (port 80).
#pragma once

#include "bus_config.h"

// Start the HTTP server. `config` is the live config owned by the main task;
// saving from the web writes NVS and reboots the device (same path as
// bus-save + bus-reboot).
void BusWebStart(BusConfig* config);
