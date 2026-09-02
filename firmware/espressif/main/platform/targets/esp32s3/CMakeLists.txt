# ESP32-S3 supports the Null compile gate and several 320x240 preview boards. Early
# component expansion cannot see the board Kconfig choice, so this is the
# narrow union required by both profiles.
list(APPEND micropixel_requires
    bosch_sensorapi
    esp_hw_support
    esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_spi esp_driver_usb_serial_jtag esp_lcd
    esp_event esp_netif esp_timer esp_wifi
    espressif__icm42670
    espressif__esp_lcd_ili9341
    espressif__esp_lcd_touch_ft5x06
    espressif__esp_lcd_touch_gt911 espressif__esp_lcd_touch_tt21100
    lvgl__lvgl
)
