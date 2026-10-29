#include "bus_web.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "bus_power.h"

namespace {

constexpr const char* kTag = "bus.web";

#ifndef BUSWEB_VERSION
#define BUSWEB_VERSION "2.8.0"
#endif

BusConfig* g_cfg = nullptr;

void GetIp(char* out, size_t cap) {
    out[0] = '\0';
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) return;
    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) return;
    if (ip.ip.addr == 0) return;
    std::snprintf(out, cap, IPSTR, IP2STR(&ip.ip));
}

void HtmlEscape(const char* in, std::string* out) {
    for (const char* p = in; *p; ++p) {
        switch (*p) {
            case '&': *out += "&amp;"; break;
            case '<': *out += "&lt;"; break;
            case '>': *out += "&gt;"; break;
            case '"': *out += "&quot;"; break;
            default: *out += *p;
        }
    }
}

void UrlDecode(const char* in, size_t len, std::string* out) {
    out->clear();
    for (size_t i = 0; i < len; ++i) {
        if (in[i] == '+') {
            *out += ' ';
        } else if (in[i] == '%' && i + 2 < len) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            *out += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2;
        } else {
            *out += in[i];
        }
    }
}

bool FieldValue(const char* body, const char* field, std::string* out) {
    const size_t flen = std::strlen(field);
    const char* p = body;
    while ((p = std::strstr(p, field)) != nullptr) {
        if (p[flen] == '=' && (p == body || p[-1] == '&')) {
            const char* start = p + flen + 1;
            const char* end = std::strchr(start, '&');
            UrlDecode(start, end ? static_cast<size_t>(end - start)
                                 : std::strlen(start),
                      out);
            return true;
        }
        p += flen;
    }
    return false;
}

std::string PageHtml(bool saved) {
    BusPowerState pwr;
    BusPowerRead(&pwr);
    char ip[16];
    GetIp(ip, sizeof(ip));

    time_t now = 0;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    char clock_str[40];
    std::strftime(clock_str, sizeof(clock_str), "%a %d %b %H:%M", &local);

    char batt[48] = "no battery reading";
    if (pwr.valid) {
        if (pwr.full) {
            std::snprintf(batt, sizeof(batt), "%u%% (full)", pwr.percent);
        } else if (pwr.charging) {
            std::snprintf(batt, sizeof(batt), "%u%% (charging)", pwr.percent);
        } else {
            std::snprintf(batt, sizeof(batt), "%u%% (%u mV)",
                          pwr.percent, pwr.mv);
        }
    }

    std::string stop_esc, route_esc, ssid_esc;
    HtmlEscape(g_cfg->stop, &stop_esc);
    HtmlEscape(g_cfg->route, &route_esc);
    HtmlEscape(g_cfg->ssid, &ssid_esc);

    std::string html;
    html += "<!doctype html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>NOTE4 Bus Board</title><style>"
            ":root{color-scheme:light}"
            "*{box-sizing:border-box}"
            "body{font-family:system-ui,-apple-system,sans-serif;margin:0;"
            "background:#f2f4f7;color:#1c2430}"
            ".wrap{max-width:560px;margin:0 auto;padding:16px}"
            ".card{background:#fff;border-radius:10px;padding:16px 18px;"
            "margin-bottom:14px;box-shadow:0 1px 3px rgba(16,24,40,.12)}"
            "h1{font-size:1.15rem;margin:0 0 4px}"
            "h2{font-size:.8rem;text-transform:uppercase;letter-spacing:"
            ".08em;color:#5b6878;margin:0 0 10px}"
            "table{width:100%;border-collapse:collapse;font-size:.92rem}"
            "td{padding:5px 0;vertical-align:top}"
            "td:first-child{color:#5b6878;width:38%}"
            "label{display:block;font-size:.8rem;color:#5b6878;"
            "margin:12px 0 4px}"
            "input[type=text],input[type=password]{width:100%;padding:10px;"
            "border:1px solid #cfd6e0;border-radius:8px;font-size:1rem}"
            "input[type=text]:focus,input[type=password]:focus{outline:none;"
            "border-color:#2b6cb0}"
            ".chk{display:flex;align-items:center;gap:8px;margin-top:12px;"
            "font-size:.95rem}"
            "button{width:100%;margin-top:18px;padding:13px;border:0;"
            "border-radius:8px;background:#2b6cb0;color:#fff;font-size:1rem;"
            "font-weight:600}"
            ".note{font-size:.82rem;color:#5b6878;margin-top:8px}"
            ".ok{background:#e6f4ea;border:1px solid #b7dfc0;color:#1e6b34;"
            "padding:10px 12px;border-radius:8px;margin-bottom:14px;"
            "font-size:.92rem}"
            "</style></head><body><div class=wrap>";
    if (saved) {
        html += "<div class=ok><b>Saved.</b> The board is rebooting to apply "
                "the new settings.</div>";
    }
    html += "<div class=card><h1>NOTE4 Bus Board</h1>"
            "<table>"
            "<tr><td>IP address</td><td>";
    html += ip[0] ? ip : "not connected";
    html += "</td></tr><tr><td>Time</td><td>";
    html += clock_str;
    html += "</td></tr><tr><td>Battery</td><td>";
    html += batt;
    html += "</td></tr><tr><td>Stop / route</td><td>";
    html += stop_esc + " / " + route_esc;
    html += "</td></tr></table></div>";

    html += "<div class=card><form method=POST action=/save>"
            "<h2>Settings</h2>"
            "<label for=stop>Stop code (e.g. T394)</label>"
            "<input type=text id=stop name=stop maxlength=7 value=\"";
    html += stop_esc;
    html += "\"><label for=route>Hero route (e.g. 51A)</label>"
            "<input type=text id=route name=route maxlength=7 value=\"";
    html += route_esc;
    html += "\"><label for=ssid>Wi-Fi SSID</label>"
            "<input type=text id=ssid name=ssid maxlength=32 value=\"";
    html += ssid_esc;
    html += "\"><label for=pass>Wi-Fi password (leave blank to keep)"
            "</label><input type=password id=pass name=pass maxlength=64>"
            "<div class=chk><input type=checkbox id=flip name=flip value=on";
    if (g_cfg->flip) html += " checked";
    html += "><label for=flip style=all:unset;cursor:pointer>Flip display "
            "180&deg;</label></div>"
            "<div class=chk><input type=checkbox id=fast name=fast value=on";
    if (g_cfg->fast) html += " checked";
    html += "><label for=fast style=all:unset;cursor:pointer>Fast refresh "
            "(snappier, more ghosting)</label></div>"
            "<button type=submit>Save &amp; reboot</button>"
            "<p class=note>Settings are stored on the device; the board "
            "restarts to apply them.</p></form></div>";

    html += "<div class=card><h2>About</h2><table>"
            "<tr><td>Firmware</td><td>note4-bus " BUSWEB_VERSION "</td></tr>"
            "<tr><td>Data</td><td>DSAT Macau bus arrivals, Open-Meteo "
            "weather; 5-minute cycle</td></tr></table></div>";
    html += "</div></body></html>";
    return html;
}

esp_err_t HandlerIndex(httpd_req_t* req) {
    const std::string html = PageHtml(false);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html.c_str(), static_cast<ssize_t>(html.size()));
}

void RebootTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

esp_err_t HandlerSave(httpd_req_t* req) {
    char body[1024] = {};
    const size_t total = std::min(static_cast<size_t>(req->content_len),
                                  sizeof(body) - 1);
    size_t received = 0;
    while (received < total) {
        const int n = httpd_req_recv(req, body + received, total - received);
        if (n <= 0) break;
        received += static_cast<size_t>(n);
    }

    std::string value;
    BusConfig next = *g_cfg;
    // Empty text fields KEEP the stored value - a blank stop/route/ssid would
    // leave the board unconfigured (and thus without this portal) after the
    // settings-save reboot.
    if (FieldValue(body, "stop", &value) && !value.empty()) {
        std::snprintf(next.stop, sizeof(next.stop), "%.7s", value.c_str());
    }
    if (FieldValue(body, "route", &value) && !value.empty()) {
        std::snprintf(next.route, sizeof(next.route), "%.7s", value.c_str());
    }
    if (FieldValue(body, "ssid", &value) && !value.empty()) {
        std::snprintf(next.ssid, sizeof(next.ssid), "%.31s", value.c_str());
    }
    if (FieldValue(body, "pass", &value)) {
        if (!value.empty()) {  // blank = keep current password
            std::snprintf(next.password, sizeof(next.password), "%.63s",
                          value.c_str());
        }
    }
    next.flip = FieldValue(body, "flip", &value) && value == "on" ? 1 : 0;
    next.fast = FieldValue(body, "fast", &value) && value == "on" ? 1 : 0;

    const esp_err_t err = BusConfigSave(next);
    ESP_LOGI(kTag, "web save: err=%s stop=%s route=%s flip=%d fast=%d",
             esp_err_to_name(err), next.stop, next.route, next.flip, next.fast);

    const std::string html = PageHtml(true);
    httpd_resp_set_type(req, "text/html");
    const esp_err_t send_err = httpd_resp_send(
        req, html.c_str(), static_cast<ssize_t>(html.size()));
    if (err == ESP_OK && send_err == ESP_OK) {
        xTaskCreate(RebootTask, "web_reboot", 3072, nullptr, 5, nullptr);
    }
    return send_err;
}

esp_err_t HandlerStatus(httpd_req_t* req) {
    BusPowerState pwr;
    BusPowerRead(&pwr);
    char ip[16];
    GetIp(ip, sizeof(ip));
    time_t now = 0;
    time(&now);
    struct tm local = {};
    localtime_r(&now, &local);
    char body[256];
    std::snprintf(body, sizeof(body),
                  "{\"ip\":\"%s\",\"time\":\"%02d:%02d\",\"battery_percent\":"
                  "%d,\"charging\":%s,\"stop\":\"%s\",\"route\":\"%s\","
                  "\"flip\":%s,\"fast\":%s}",
                  ip, local.tm_hour, local.tm_min,
                  pwr.valid ? pwr.percent : -1,
                  pwr.charging ? "true" : "false",
                  g_cfg->stop, g_cfg->route,
                  g_cfg->flip ? "true" : "false",
                  g_cfg->fast ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

}  // namespace

void BusWebStart(BusConfig* config) {
    g_cfg = config;
    httpd_handle_t server = nullptr;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.max_uri_handlers = 8;
    http.stack_size = 8192;
    if (httpd_start(&server, &http) != ESP_OK) {
        ESP_LOGE(kTag, "failed to start web server");
        return;
    }
    const httpd_uri_t index_uri = {"/", HTTP_GET, HandlerIndex, nullptr};
    const httpd_uri_t save_uri = {"/save", HTTP_POST, HandlerSave, nullptr};
    const httpd_uri_t status_uri = {"/status", HTTP_GET, HandlerStatus,
                                    nullptr};
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_uri_handler(server, &status_uri);
    ESP_LOGI(kTag, "web portal ready on port 80");
}
