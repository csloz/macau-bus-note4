#include "bus_config.h"

#include <cstdio>
#include <cstring>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace {

constexpr const char* kTag = "bus.cfg";
constexpr const char* kNamespace = "bus";

BusConfig* g_config = nullptr;
BusConsoleHooks g_hooks;

void GetStr(nvs_handle_t handle, const char* key, char* out, size_t cap) {
    size_t length = cap;
    if (nvs_get_str(handle, key, out, &length) != ESP_OK) {
        out[0] = '\0';
    }
}

void Copy(char* out, size_t cap, const char* src) {
    std::snprintf(out, cap, "%s", src);
}

struct {
    struct arg_str* key;
    struct arg_str* value;
    struct arg_end* end;
} g_set_args;

int CmdShow(int, char**) {
    const BusConfig& c = *g_config;
    printf("ssid  %s\n", c.ssid);
    printf("pass  %s\n", c.password[0] ? "(set)" : "(empty)");
    printf("stop  %s\n", c.stop);
    printf("route %s\n", c.route);
    printf("flip  %s\n", c.flip ? "on" : "off");
    printf("fast  %s\n", c.fast ? "on" : "off");
    return 0;
}

int CmdSet(int argc, char** argv) {
    const int errors = arg_parse(argc, argv, (void**)&g_set_args);
    if (errors != 0) {
        arg_print_errors(stderr, g_set_args.end, argv[0]);
        return 1;
    }
    const char* key = g_set_args.key->sval[0];
    const char* value = g_set_args.value->count ? g_set_args.value->sval[0] : "";
    BusConfig& c = *g_config;
    if (std::strcmp(key, "ssid") == 0) {
        Copy(c.ssid, sizeof(c.ssid), value);
    } else if (std::strcmp(key, "pass") == 0) {
        Copy(c.password, sizeof(c.password), value);
    } else if (std::strcmp(key, "stop") == 0) {
        Copy(c.stop, sizeof(c.stop), value);
    } else if (std::strcmp(key, "route") == 0) {
        Copy(c.route, sizeof(c.route), value);
    } else if (std::strcmp(key, "flip") == 0) {
        c.flip = (std::strcmp(value, "on") == 0 ||
                  std::strcmp(value, "1") == 0) ? 1 : 0;
    } else if (std::strcmp(key, "fast") == 0) {
        c.fast = (std::strcmp(value, "on") == 0 ||
                  std::strcmp(value, "1") == 0) ? 1 : 0;
    } else {
        printf("unknown key '%s' (ssid pass stop route flip fast)\n", key);
        return 1;
    }
    printf("set %s; run 'bus-save' then 'bus-reboot'\n", key);
    return 0;
}

int CmdSave(int, char**) {
    const esp_err_t err = BusConfigSave(*g_config);
    printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

int CmdRefresh(int, char**) {
    if (g_hooks.refresh_now) {
        g_hooks.refresh_now();
        printf("refresh triggered\n");
    }
    return 0;
}

int CmdReboot(int, char**) {
    printf("rebooting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

}  // namespace

esp_err_t BusConfigLoad(BusConfig* out) {
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    Copy(out->ssid, sizeof(out->ssid), CONFIG_BUS_WIFI_SSID);
    Copy(out->password, sizeof(out->password), CONFIG_BUS_WIFI_PASSWORD);
    Copy(out->stop, sizeof(out->stop), CONFIG_BUS_STOP_CODE);
    Copy(out->route, sizeof(out->route), CONFIG_BUS_HERO_ROUTE);

    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_OK;
    }
    char scratch[96];
    GetStr(handle, "ssid", scratch, sizeof(scratch));
    if (scratch[0]) {
        Copy(out->ssid, sizeof(out->ssid), scratch);
    }
    GetStr(handle, "pass", scratch, sizeof(scratch));
    if (scratch[0]) {
        Copy(out->password, sizeof(out->password), scratch);
    }
    GetStr(handle, "stop", scratch, sizeof(scratch));
    if (scratch[0]) {
        Copy(out->stop, sizeof(out->stop), scratch);
    }
    GetStr(handle, "route", scratch, sizeof(scratch));
    if (scratch[0]) {
        Copy(out->route, sizeof(out->route), scratch);
    }
    uint8_t flip = 0;
    if (nvs_get_u8(handle, "flip", &flip) == ESP_OK) {
        out->flip = flip ? 1 : 0;
    }
    uint8_t fast = 0;
    if (nvs_get_u8(handle, "fast", &fast) == ESP_OK) {
        out->fast = fast ? 1 : 0;
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t BusConfigSave(const BusConfig& config) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(handle, "ssid", config.ssid);
    nvs_set_str(handle, "pass", config.password);
    nvs_set_str(handle, "stop", config.stop);
    nvs_set_str(handle, "route", config.route);
    nvs_set_u8(handle, "flip", config.flip ? 1 : 0);
    nvs_set_u8(handle, "fast", config.fast ? 1 : 0);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool BusConfigComplete(const BusConfig& config) {
    return config.ssid[0] != '\0';
}

void BusConfigRegisterConsole(BusConfig* config, BusConsoleHooks hooks) {
    g_config = config;
    g_hooks = hooks;
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "bus>";
    repl_config.max_cmdline_length = 160;
    if (esp_console_new_repl_stdio(&repl_config, &repl) != ESP_OK) {
        ESP_LOGW(kTag, "console unavailable; using stored config only");
        return;
    }
    g_set_args.key = arg_str1(nullptr, nullptr, "<key>", "ssid|pass");
    g_set_args.value = arg_str0(nullptr, nullptr, "<value>", "new value");
    g_set_args.end = arg_end(2);

    const esp_console_cmd_t commands[] = {
        {"bus-show", "Print Wi-Fi config", nullptr, CmdShow, nullptr, nullptr,
         nullptr},
        {"bus-set", "Set ssid or pass", nullptr, CmdSet, &g_set_args, nullptr,
         nullptr},
        {"bus-save", "Write config to NVS", nullptr, CmdSave, nullptr, nullptr,
         nullptr},
        {"bus-now", "Refresh the display immediately", nullptr, CmdRefresh,
         nullptr, nullptr, nullptr},
        {"bus-reboot", "Restart", nullptr, CmdReboot, nullptr, nullptr,
         nullptr},
    };
    for (const auto& command : commands) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&command));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(kTag, "console ready: bus-set / bus-save / bus-now");
}
