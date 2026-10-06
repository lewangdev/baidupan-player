idf_component_register(
    SRCS "wifi_manager.cc" "wifi_station.cc" "ssid_manager.cc"
         "wifi_configuration_ap.cc" "dns_server.cc"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "assets/wifi_configuration.html" "assets/wifi_configuration_done.html"
    REQUIRES esp_timer esp_wifi nvs_flash esp_http_server esp_netif esp_event lwip json
)
