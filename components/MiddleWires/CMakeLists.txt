idf_component_register(
    SRCS "wifi_provision/wifi_provision.c"
          "audio_player/audio_player.c"
          "music_playlist/music_playlist.c"
    INCLUDE_DIRS "wifi_provision" "audio_player" "music_playlist"
    REQUIRES esp_wifi wpa_supplicant nvs_flash esp_netif esp_event freertos
              BSP esp_player esp_audio_render esp_codec_dev esp_extractor
              esp_audio_codec gmf_core gmf_audio gmf_io media_lib_sal
              esp_http_client
)
