// DSAT live bus API: fetch route state for a configurable stop + hero route,
// plus Cotai weather from Open-Meteo.
#include "bus_api.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bus_stops_data.h"

namespace {

constexpr const char* kTag = "bus.api";
constexpr int kBodyCap = 24 * 1024;
constexpr const char* kBaseUrl =
    "https://bis.dsat.gov.mo/macauweb/routestation/bus";
constexpr const char* kWeatherUrl =
    "https://api.open-meteo.com/v1/forecast"
    "?latitude=22.15&longitude=113.56"
    "&current=temperature_2m,relative_humidity_2m,weather_code"
    "&timezone=Asia%2FMacau";
constexpr float kFallbackSpeedKmh = 25.0f;  // same assumption as Quote/0

struct Sink {
    char buffer[kBodyCap] = {};
    int length = 0;
    bool overflow = false;
};

// One fetch at a time from the update task.
Sink g_sink;

esp_err_t OnHttp(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    auto* sink = static_cast<Sink*>(event->user_data);
    const int room = kBodyCap - 1 - sink->length;
    if (room <= 0) {
        sink->overflow = true;
        return ESP_OK;
    }
    const int n = event->data_len < room ? event->data_len : room;
    std::memcpy(sink->buffer + sink->length, event->data, n);
    sink->length += n;
    if (n < event->data_len) sink->overflow = true;
    sink->buffer[sink->length] = '\0';
    return ESP_OK;
}

esp_err_t HttpGet(const char* url, char* out, size_t cap) {
    if (cap < kBodyCap) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(kTag, "GET %s", url);
    const int64_t t0 = esp_timer_get_time();
    g_sink.length = 0;
    g_sink.overflow = false;
    g_sink.buffer[0] = '\0';

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 12000;
    cfg.event_handler = OnHttp;
    cfg.user_data = &g_sink;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Referer",
                               "https://bis.dsat.gov.mo/macauweb/");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(kTag, "GET done in %lld ms: %s (http %d, %d bytes%s)",
             (long long)((esp_timer_get_time() - t0) / 1000),
             esp_err_to_name(err), status, g_sink.length,
             g_sink.overflow ? ", truncated" : "");
    if (err != ESP_OK) {
        return err;
    }
    std::memcpy(out, g_sink.buffer, g_sink.length + 1);
    if (g_sink.overflow) {
        ESP_LOGW(kTag, "response truncated");
    }
    return ESP_OK;
}

// Find the live routeInfo station index whose staCode base matches `code`.
int FindStationIndex(cJSON* route_info, const char* code) {
    const cJSON* station = nullptr;
    int index = 0;
    cJSON_ArrayForEach(station, route_info) {
        const cJSON* sta = cJSON_GetObjectItem(station, "staCode");
        if (cJSON_IsString(sta) && sta->valuestring != nullptr) {
            const char* slash = std::strchr(sta->valuestring, '/');
            const size_t base_len =
                slash ? static_cast<size_t>(slash - sta->valuestring)
                      : std::strlen(sta->valuestring);
            if (base_len == std::strlen(code) &&
                std::strncmp(sta->valuestring, code, base_len) == 0) {
                return index;
            }
        }
        index++;
    }
    return -1;
}

const bus_stops::Stop* FindStopData(const char* code) {
    for (size_t i = 0; i < bus_stops::kStopCount; i++) {
        if (std::strcmp(bus_stops::kStops[i].code, code) == 0) {
            return &bus_stops::kStops[i];
        }
    }
    return nullptr;
}

// Straight-line distance in km (same metric the Quote/0 display uses).
float HaversineKm(double lat1, double lon1, double lat2, double lon2) {
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    constexpr double kEarthKm = 6371.0;
    const double p1 = lat1 * kRad;
    const double p2 = lat2 * kRad;
    const double dp = (lat2 - lat1) * kRad;
    const double dl = (lon2 - lon1) * kRad;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) +
                     std::cos(p1) * std::cos(p2) * std::sin(dl / 2) *
                         std::sin(dl / 2);
    return static_cast<float>(
        kEarthKm * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a)));
}

int EtaMinutes(float km, int speed_kmh) {
    // Match the Quote/0 display exactly: flat 25 km/h incl. stops.
    (void)speed_kmh;
    if (km < 0.15f) {
        return 0;  // practically at the stop
    }
    return static_cast<int>(std::lround(km / kFallbackSpeedKmh * 60.0f));
}

// One entry per physical bus (matching the macau-bus host pipeline), sorted
// by stops_away after collection.
struct BusSpot {
    int stops_away;
    float km;
};

constexpr int kMaxBuses = 12;

// Collect every bus currently at or before stop_index (buses past the stop
// are ignored), one entry per bus, then sort nearest-first.
void CollectBuses(cJSON* route_info, int stop_index,
                  const bus_stops::Stop* stop_data, BusSpot* out, int* count) {
    const cJSON* station = nullptr;
    int index = 0;
    cJSON_ArrayForEach(station, route_info) {
        if (index > stop_index) break;
        const cJSON* buses = cJSON_GetObjectItem(station, "busInfo");
        if (cJSON_IsArray(buses)) {
            const cJSON* bus = nullptr;
            cJSON_ArrayForEach(bus, buses) {
                if (*count >= kMaxBuses) return;
                // Straight-line km from the monitored stop to this station.
                float km = -1.0f;
                const cJSON* sta = cJSON_GetObjectItem(station, "staCode");
                if (cJSON_IsString(sta) && sta->valuestring) {
                    char base[16] = {};
                    const char* slash = std::strchr(sta->valuestring, '/');
                    std::snprintf(base, sizeof(base), "%.*s",
                                  slash ? (int)(slash - sta->valuestring)
                                        : (int)std::strlen(sta->valuestring),
                                  sta->valuestring);
                    const bus_stops::Stop* bus_stop = FindStopData(base);
                    if (bus_stop && stop_data) {
                        km = HaversineKm(stop_data->lat_e6 / 1e6,
                                         stop_data->lng_e6 / 1e6,
                                         bus_stop->lat_e6 / 1e6,
                                         bus_stop->lng_e6 / 1e6);
                    }
                }
                out[*count].stops_away = stop_index - index;
                out[*count].km = km;
                (*count)++;
            }
        }
        index++;
    }
    // Insertion sort by stops_away (n is tiny).
    for (int i = 1; i < *count; i++) {
        const BusSpot key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].stops_away > key.stops_away) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
}

void FillViewFromBuses(BusView* view, const BusSpot* buses, int count) {
    if (count <= 0) {
        return;
    }
    view->has_bus = true;
    view->stops_away = buses[0].stops_away;
    view->km = buses[0].km;
    view->eta_min = buses[0].km >= 0 ? EtaMinutes(buses[0].km, 0) : -1;
    if (count > 1) {
        view->has_next = true;
        view->next_stops_away = buses[1].stops_away;
        view->next_km = buses[1].km;
        view->next_eta_min = buses[1].km >= 0 ? EtaMinutes(buses[1].km, 0) : -1;
    }
}

}  // namespace

esp_err_t BusApiFetchView(const char* route, const char* stop, BusView* out) {
    std::memset(out, 0, sizeof(*out));
    std::snprintf(out->route, sizeof(out->route), "%s", route);
    out->stops_away = -1;
    out->eta_min = -1;

    const bus_stops::Stop* stop_data = FindStopData(stop);
    if (stop_data == nullptr) {
        ESP_LOGW(kTag, "stop %s not in stop database", stop);
        return ESP_ERR_NOT_FOUND;
    }

    // Fetch both directions, then pick the one where the stop appears
    // EARLIEST in the sequence (same rule as the macau-bus host pipeline) —
    // the earliest stop index is the direction with the most buses still
    // to come.
    char url[160];
    char* body = static_cast<char*>(std::malloc(kBodyCap));
    if (body == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    bool found = false;
    int best_dir = -1;
    int best_stop_index = 0;
    BusSpot best_buses[kMaxBuses] = {};
    int best_count = 0;
    esp_err_t last_err = ESP_FAIL;

    for (int dir = 0; dir <= 1; dir++) {
        std::snprintf(url, sizeof(url), "%s?routeName=%s&dir=%d", kBaseUrl,
                      route, dir);
        const esp_err_t http = HttpGet(url, body, kBodyCap);
        if (http != ESP_OK) {
            last_err = http;
            continue;
        }
        cJSON* doc = cJSON_Parse(body);
        if (doc == nullptr) {
            last_err = ESP_ERR_INVALID_RESPONSE;
            continue;
        }
        cJSON* data = cJSON_GetObjectItem(doc, "data");
        cJSON* route_info = cJSON_GetObjectItem(data, "routeInfo");
        if (cJSON_IsArray(route_info)) {
            const int stop_index = FindStationIndex(route_info, stop);
            if (stop_index >= 0 && (!found || stop_index < best_stop_index)) {
                BusSpot buses[kMaxBuses] = {};
                int count = 0;
                CollectBuses(route_info, stop_index, stop_data, buses, &count);
                found = true;
                best_dir = dir;
                best_stop_index = stop_index;
                std::memcpy(best_buses, buses, sizeof(buses));
                best_count = count;
            }
        }
        cJSON_Delete(doc);
    }
    std::free(body);

    if (found) {
        out->has_stop = true;
        FillViewFromBuses(out, best_buses, best_count);
        ESP_LOGI(kTag, "%s dir %d: stop idx %d, %d buses en route", route,
                 best_dir, best_stop_index, best_count);
    } else if (last_err != ESP_OK && last_err != ESP_FAIL) {
        return last_err;
    }
    return ESP_OK;
}

namespace {

const char* WeatherText(int code) {
    struct Entry {
        int code;
        const char* text;
    };
    static constexpr Entry kMap[] = {
        {0, "CLEAR"},   {1, "CLEAR"},   {2, "P.CLOUDY"}, {3, "OVERCAST"},
        {45, "FOG"},    {48, "FOG"},    {51, "DRIZZLE"}, {53, "DRIZZLE"},
        {55, "DRIZZLE"},{56, "DRIZZLE"},{57, "DRIZZLE"}, {61, "RAIN"},
        {63, "RAIN"},   {65, "RAIN"},   {66, "RAIN"},    {67, "RAIN"},
        {71, "SNOW"},   {73, "SNOW"},   {75, "SNOW"},    {77, "SNOW"},
        {80, "SHOWERS"},{81, "SHOWERS"},{82, "SHOWERS"}, {85, "SNOW"},
        {86, "SNOW"},   {95, "STORM"},  {96, "STORM"},   {99, "STORM"},
    };
    for (const auto& e : kMap) {
        if (e.code == code) {
            return e.text;
        }
    }
    return "--";
}

}  // namespace

esp_err_t BusWeatherFetch(Weather* wx) {
    std::memset(wx, 0, sizeof(*wx));
    char* body = static_cast<char*>(std::malloc(kBodyCap));
    if (body == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = HttpGet(kWeatherUrl, body, kBodyCap);
    if (err != ESP_OK) {
        std::free(body);
        return err;
    }
    cJSON* doc = cJSON_Parse(body);
    std::free(body);
    if (doc == nullptr) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON* cur = cJSON_GetObjectItem(doc, "current");
    const cJSON* temp = cJSON_GetObjectItem(cur, "temperature_2m");
    const cJSON* rh = cJSON_GetObjectItem(cur, "relative_humidity_2m");
    const cJSON* code = cJSON_GetObjectItem(cur, "weather_code");
    if (cJSON_IsNumber(temp) && cJSON_IsNumber(rh) && cJSON_IsNumber(code)) {
        wx->valid = true;
        wx->temp_c = static_cast<int>(std::lround(temp->valuedouble));
        wx->humidity = static_cast<int>(std::lround(rh->valuedouble));
        std::snprintf(wx->text, sizeof(wx->text), "%s",
                      WeatherText(static_cast<int>(code->valuedouble)));
    }
    cJSON_Delete(doc);
    return ESP_OK;
}
