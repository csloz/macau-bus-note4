// DSAT live bus API: fetch route state for a configurable stop + hero route,
// plus Cotai weather from Open-Meteo.
#pragma once

#include "esp_err.h"

#include <cstddef>

struct BusView {
    char route[8];        // route actually displayed (may differ from config
                          // if the configured route does not serve the stop)
    bool has_stop;        // stop found on this route
    bool has_bus;         // at least one bus en route
    int stops_away;       // 0 = at stop (-1 when !has_bus)
    float km;             // straight-line km from stop to nearest bus
    int eta_min;          // minutes, -1 when unknown
    bool has_next;        // a following bus exists
    int next_stops_away;
    float next_km;
    int next_eta_min;
};

struct Weather {
    bool valid;
    int temp_c;
    int humidity;
    char text[12];        // "CLEAR", "RAIN", ...
};

// Fetch live state of `route` at `stop` (tries both directions). Returns
// ESP_OK with out->has_stop=false when the route does not serve the stop.
esp_err_t BusApiFetchView(const char* route, const char* stop, BusView* out);

// Current weather for the Cotai area. Returns ESP_OK or an error; on error
// wx->valid is false (weather is decorative, never blocks bus data).
esp_err_t BusWeatherFetch(Weather* wx);
