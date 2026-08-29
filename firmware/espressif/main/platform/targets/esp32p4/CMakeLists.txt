# ESP32-P4 target requirements are resolved during IDF's early expansion,
# before board Kconfig values exist. Keep this target union separate from
# ESP32-S31 so a new SoC does not inherit ESP-Hosted or P4-only transports.
list(APPEND micropixel_requires
    esp_hw_support
    esp_driver_dma esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_jpeg esp_driver_ledc
    esp_driver_ppa esp_driver_uart esp_driver_usb_serial_jtag
    button esp_event esp_lcd esp_lvgl_adapter esp_lcd_touch_gt911 esp_netif esp_timer esp_wifi lvgl__lvgl
)
