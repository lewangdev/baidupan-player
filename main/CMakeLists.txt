idf_component_register(
    SRCS "main.c"
         "bp_ui.c"
         "bp_baidu.c"
         "bp_player.c"
         "bp_media.c"
         "bp_console.c"
         "bp_wifi.cc"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash esp_wifi esp-wifi-connect esp_http_client json mbedtls
    PRIV_REQUIRES esp_netif esp_event esp_timer esp-tls lwip lvgl
)
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/bp_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/bp_font_24.c"
)
# 上游 demo_*.c / ui_pixel*.c 保留在磁盘上供主机测试(tools/validate.sh 直接编译),
# 不参与固件构建:本应用是全新界面(强制 UI 重新设计规则)。
