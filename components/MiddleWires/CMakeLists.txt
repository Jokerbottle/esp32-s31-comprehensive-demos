idf_component_register(
    SRCS "wifi_provision/wifi_provision.c"
    INCLUDE_DIRS "wifi_provision"
    REQUIRES esp_wifi wpa_supplicant nvs_flash esp_netif esp_event freertos
)
