# ESP32-S31 target has native Wi-Fi and USB HS; its display is an SPI/QSPI panel.
# Board-owned drivers are selected later; this list contains only target-wide
# ESP-IDF facilities required by the MicroPixel platform implementation.
list(APPEND micropixel_requires
    bosch_sensorapi
    esp_hw_support
    esp_driver_dma esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_jpeg esp_driver_ledc
    esp_driver_ppa esp_driver_spi esp_driver_uart
    78__esp_lcd_touch_cst92xx button esp_event esp_lcd esp_lvgl_adapter esp_netif esp_timer esp_tinyusb esp_wifi
    lvgl__lvgl
)
