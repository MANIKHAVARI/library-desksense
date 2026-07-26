# This file tells ESP-IDF which C file to compile.
idf_component_register(
    SRCS "application_code.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_adc
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        nvs_flash
        mqtt
        coap
)