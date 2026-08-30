idf_component_register(
    SRCS "ES8311/es8311_audio.c"
    INCLUDE_DIRS "." "ES8311"
    REQUIRES driver esp_codec_dev freertos esp_driver_gpio esp_driver_i2c esp_driver_i2s
)
