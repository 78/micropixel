idf_component_register(
    SRCS "uart_eth_modem.cc"
    INCLUDE_DIRS "include"
    REQUIRES uart-uhci espressif__iot_eth esp_netif esp_event esp_driver_gpio esp_driver_uart esp_timer)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
