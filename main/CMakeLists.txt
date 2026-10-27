idf_component_register(
    SRCS "bus_main.cc" "bus_config.cc" "bus_wifi.cc" "bus_api.cc"
         "bus_display.cc" "bus_buttons.cc" "bus_ui.cc" "u8g2_font.cc"
         "u8g2_font_macau.c"
    INCLUDE_DIRS "."
    REQUIRES hearth_gfx zectrix_epd
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_http_client
                  console esp_timer mbedtls driver
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
