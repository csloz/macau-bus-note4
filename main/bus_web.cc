#include "bus_web.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus_power.h"

#ifndef BUSWEB_VERSION
#define BUSWEB_VERSION "2.9.0"
#endif

namespace {

constexpr const char* kTag = "bus.web";

constexpr const char* kRepoApi =
    "https://api.github.com/repos/csloz/macau-bus-note4/releases?per_page=8";
constexpr const char* kRepoLatest =
    "https://api.github.com/repos/csloz/macau-bus-note4/releases/latest";
constexpr const char* kAssetUrlBase =
    "https://github.com/csloz/macau-bus-note4/releases/download";
constexpr const char* kAssetName = "note4_bus.bin";
constexpr int64_t kReleaseCacheMs = 5LL * 60 * 1000;

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

// ---------- GitHub releases / OTA ----------

struct ReleaseInfo {
    std::string tag;
    std::string name;
    std::string date;
    std::string notes;
};

std::vector<ReleaseInfo> g_releases;
int64_t g_releases_at = -kReleaseCacheMs;
int64_t g_releases_fail_at = -60LL * 1000 * 1000;  // failure backoff anchor

esp_err_t OnHttpData(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data) {
        static_cast<std::string*>(event->user_data)->append(
            static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}

// HTTPS GET into a heap buffer.
esp_err_t HttpsGet(const char* url, std::string* out) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 15000;
    config.buffer_size = 4096;
    config.event_handler = OnHttpData;
    config.user_data = out;
    out->clear();
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "User-Agent", "note4-bus");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        const int status = esp_http_client_get_status_code(client);
        if (status != 200 || out->empty()) {
            ESP_LOGW(kTag, "github http status %d got=%zu", status,
                     out->size());
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(kTag, "github perform failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

// Numeric compare of "v2.10.0"-style tags; >0 when a is newer than b.
int CompareVersions(const std::string& a, const std::string& b) {
    int ai[3] = {0, 0, 0}, bi[3] = {0, 0, 0};
    std::sscanf(a.c_str(), "v%d.%d.%d", &ai[0], &ai[1], &ai[2]);
    std::sscanf(b.c_str(), "v%d.%d.%d", &bi[0], &bi[1], &bi[2]);
    for (int i = 0; i < 3; i++) {
        if (ai[i] != bi[i]) return ai[i] - bi[i];
    }
    return 0;
}

esp_err_t RefreshReleases(bool latest_only) {
    const int64_t now = esp_timer_get_time() / 1000;
    if (now - g_releases_at < kReleaseCacheMs && !g_releases.empty()) {
        return ESP_OK;
    }
    // Back off after a failed fetch so page loads don't hammer the
    // unauthenticated GitHub API rate limit (60/hour per IP).
    if (now - g_releases_fail_at < 60LL * 1000) {
        return g_releases.empty() ? ESP_FAIL : ESP_OK;
    }
    std::string body;
    const esp_err_t err = HttpsGet(latest_only ? kRepoLatest : kRepoApi, &body);
    if (err != ESP_OK) {
        g_releases_fail_at = now;
        ESP_LOGW(kTag, "release fetch failed: %s", esp_err_to_name(err));
        return err;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(kTag, "release JSON parse failed");
        return ESP_FAIL;
    }
    std::vector<ReleaseInfo> parsed;
    const bool is_array = cJSON_IsArray(root);
    cJSON* item = is_array ? root->child : root;
    for (int i = 0; i < 8 && item != nullptr; i++) {
        const cJSON* tag = cJSON_GetObjectItem(item, "tag_name");
        if (tag == nullptr || !cJSON_IsString(tag)) break;
        ReleaseInfo r;
        r.tag = cJSON_GetStringValue(tag);
        const cJSON* name = cJSON_GetObjectItem(item, "name");
        if (name != nullptr && cJSON_IsString(name)) {
            r.name = cJSON_GetStringValue(name);
        }
        const cJSON* published = cJSON_GetObjectItem(item, "published_at");
        if (published != nullptr && cJSON_IsString(published)) {
            // "2026-10-29T12:00:00Z" -> date only
            r.date = std::string(cJSON_GetStringValue(published)).substr(0, 10);
        }
        const cJSON* notes = cJSON_GetObjectItem(item, "body");
        if (notes != nullptr && cJSON_IsString(notes)) {
            r.notes = cJSON_GetStringValue(notes);
            if (r.notes.size() > 1500) r.notes.resize(1500);
        }
        parsed.push_back(std::move(r));
        item = is_array ? item->next : nullptr;
    }
    cJSON_Delete(root);
    if (parsed.empty()) {
        return ESP_FAIL;
    }
    g_releases = std::move(parsed);
    g_releases_at = now;
    return ESP_OK;
}

// True when a release newer than the running version exists.
bool UpdateAvailable(std::string* latest_tag) {
    if (RefreshReleases(true) != ESP_OK || g_releases.empty()) {
        return false;
    }
    *latest_tag = g_releases.front().tag;
    return CompareVersions(g_releases.front().tag, BUSWEB_VERSION) > 0;
}

std::string PageCss() {
    return "<!doctype html><html><head><meta charset=utf-8>"
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
           "button.danger{background:#b91c1c}"
           ".note{font-size:.82rem;color:#5b6878;margin-top:8px}"
           ".ok{background:#e6f4ea;border:1px solid #b7dfc0;color:#1e6b34;"
           "padding:10px 12px;border-radius:8px;margin-bottom:14px;"
           "font-size:.92rem}"
           ".warn{background:#fef3c7;border:1px solid #f59e0b;color:#92400e;"
           "padding:10px 12px;border-radius:8px;margin-bottom:14px;"
           "font-size:.92rem}"
           ".warn a{color:#92400e;font-weight:600}"
           ".rel{border:1px solid #e2e8f0;border-radius:8px;padding:10px 12px;"
           "margin-bottom:10px}"
           ".rel .tag{font-weight:700;font-size:.95rem}"
           ".rel .meta{font-size:.78rem;color:#5b6878;margin-left:6px}"
           ".rel .badge{background:#dbeafe;color:#1e40af;border-radius:6px;"
           "padding:1px 7px;font-size:.72rem;margin-left:6px}"
           ".notes{font-size:.85rem;color:#374151;white-space:pre-wrap;"
           "margin:8px 0 0;font-family:inherit;max-height:220px;"
           "overflow-y:auto;background:#f8fafc;padding:8px;border-radius:6px}"
           "a{color:#2b6cb0}"
           "</style></head><body><div class=wrap>";
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
    html += PageCss();
    if (saved) {
        html += "<div class=ok><b>Saved.</b> The board is rebooting to apply "
                "the new settings.</div>";
    }
    std::string latest;
    if (UpdateAvailable(&latest)) {
        html += "<div class=warn><b>New firmware " + latest +
                " available.</b> <a href=/firmware>Review &amp; install</a>"
                "</div>";
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
            "<tr><td>Updates</td><td><a href=/firmware>Firmware update</a>"
            "</td></tr>"
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
                  "\"flip\":%s,\"fast\":%s,\"version\":\"" BUSWEB_VERSION "\"}",
                  ip, local.tm_hour, local.tm_min,
                  pwr.valid ? pwr.percent : -1,
                  pwr.charging ? "true" : "false",
                  g_cfg->stop, g_cfg->route,
                  g_cfg->flip ? "true" : "false",
                  g_cfg->fast ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// ---------- Firmware update pages ----------

esp_err_t HandlerFirmware(httpd_req_t* req) {
    std::string html = PageCss();
    html += "<div class=card><h1>Firmware</h1>"
            "<p class=note>Running note4-bus " BUSWEB_VERSION
            ". Pick a release to flash from GitHub. The board reboots "
            "automatically after the update (1&ndash;2 min).</p></div>";
    const esp_err_t err = RefreshReleases(false);
    if (err != ESP_OK || g_releases.empty()) {
        html += "<div class=card><div class=warn>Could not reach GitHub "
                "releases. Check the board's internet connection and "
                "reload.</div></div></div></body></html>";
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, html.c_str(),
                               static_cast<ssize_t>(html.size()));
    }
    html += "<div class=card><form method=POST action=/update>"
            "<h2>Releases</h2>";
    for (size_t i = 0; i < g_releases.size(); i++) {
        const ReleaseInfo& r = g_releases[i];
        const bool running = r.tag == "v" BUSWEB_VERSION;
        html += "<div class=rel><label style=all:unset;cursor:pointer>"
                "<input type=radio name=tag value=";
        html += r.tag;
        html += running ? " checked>" : (i == 0 ? " checked>" : ">");
        html += "<span class=tag>";
        HtmlEscape(r.tag.c_str(), &html);
        html += "</span>";
        if (!r.name.empty() && r.name != r.tag) {
            html += "<span class=meta>";
            HtmlEscape(r.name.c_str(), &html);
            html += "</span>";
        }
        if (!r.date.empty()) {
            html += "<span class=meta>" + r.date + "</span>";
        }
        if (running) {
            html += "<span class=badge>running</span>";
        } else if (i == 0) {
            html += "<span class=badge>latest</span>";
        }
        if (!r.notes.empty()) {
            html += "<pre class=notes>";
            HtmlEscape(r.notes.c_str(), &html);
            html += "</pre>";
        }
        html += "</div>";
    }
    html += "<button type=submit class=danger>Flash selected release"
            "</button></form></div></div></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html.c_str(), static_cast<ssize_t>(html.size()));
}

// Runs in its own task: download + flash can take a minute.
void OtaTask(void* arg) {
    auto* url = static_cast<char*>(arg);
    vTaskDelay(pdMS_TO_TICKS(1000));  // let the HTTP response flush
    ESP_LOGI(kTag, "OTA start: %s", url);

    esp_http_client_config_t http = {};
    http.url = url;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 20000;
    // github.com's redirect response carries large headers.
    http.buffer_size = 8192;
    http.buffer_size_tx = 2048;
    esp_https_ota_config_t ota = {};
    ota.http_config = &http;

    esp_err_t err = esp_https_ota(&ota);
    free(url);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "OTA success, rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(kTag, "OTA failed: %s - still running old firmware",
             esp_err_to_name(err));
    vTaskDelete(nullptr);
}

esp_err_t HandlerUpdate(httpd_req_t* req) {
    char body[128] = {};
    const size_t total = std::min(static_cast<size_t>(req->content_len),
                                  sizeof(body) - 1);
    size_t received = 0;
    while (received < total) {
        const int n = httpd_req_recv(req, body + received, total - received);
        if (n <= 0) break;
        received += static_cast<size_t>(n);
    }
    std::string tag;
    if (!FieldValue(body, "tag", &tag) || tag.empty() ||
        tag.size() > 32) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing tag");
        return ESP_OK;
    }
    for (const char c : tag) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
                        c == '_';
        if (!ok) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad tag");
            return ESP_OK;
        }
    }
    // Confirm the release actually exists and has our asset.
    bool found = false;
    if (RefreshReleases(false) == ESP_OK) {
        for (const ReleaseInfo& r : g_releases) {
            if (r.tag == tag) {
                found = true;
                break;
            }
        }
    }
    if (!found) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown release");
        return ESP_OK;
    }

    std::string url = std::string(kAssetUrlBase) + "/" + tag + "/" +
                      kAssetName;
    auto* heap_url = static_cast<char*>(malloc(url.size() + 1));
    if (heap_url == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_OK;
    }
    std::memcpy(heap_url, url.c_str(), url.size() + 1);

    std::string html = PageCss();
    html += "<div class=card><h1>Updating&hellip;</h1>"
            "<p>Flashing " + tag +
            " from GitHub. Do <b>not</b> power off the board.</p>"
            "<p class=note>This page will stop responding while the update "
            "runs; the board reboots into the new firmware when finished "
            "(about 1&ndash;2 minutes).</p></div></div></body></html>";
    httpd_resp_set_type(req, "text/html");
    const esp_err_t send_err = httpd_resp_send(
        req, html.c_str(), static_cast<ssize_t>(html.size()));
    if (send_err == ESP_OK &&
        xTaskCreate(OtaTask, "ota", 16384, heap_url, 5, nullptr) !=
            pdPASS) {
        free(heap_url);
    }
    return send_err;
}

}  // namespace

void BusWebStart(BusConfig* config) {
    g_cfg = config;
    httpd_handle_t server = nullptr;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.max_uri_handlers = 8;
    http.stack_size = 16384;  // OTA + TLS handshake run in handler tasks
    if (httpd_start(&server, &http) != ESP_OK) {
        ESP_LOGE(kTag, "failed to start web server");
        return;
    }
    const httpd_uri_t index_uri = {"/", HTTP_GET, HandlerIndex, nullptr};
    const httpd_uri_t save_uri = {"/save", HTTP_POST, HandlerSave, nullptr};
    const httpd_uri_t status_uri = {"/status", HTTP_GET, HandlerStatus,
                                    nullptr};
    const httpd_uri_t firmware_uri = {"/firmware", HTTP_GET, HandlerFirmware,
                                      nullptr};
    const httpd_uri_t update_uri = {"/update", HTTP_POST, HandlerUpdate,
                                    nullptr};
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &firmware_uri);
    httpd_register_uri_handler(server, &update_uri);
    ESP_LOGI(kTag, "web portal ready on port 80");
}
