#pragma once
#include <functional>
#include <string>

#include "esp_err.h"
#include "freertos/task.h"
class UartEthModem {
   public:
    enum class UartEthModemEvent {
        Connecting,
        Connected,
        Disconnected,
        InFlightMode,
        RequestingPdpContext,
        ErrorNoSim,
        ErrorInitFailed
    };
    struct Config {
        int uart_num, baud_rate, tx_pin, rx_pin, mrdy_pin, srdy_pin;
        size_t rx_buffer_count, rx_buffer_size;
    };
    inline static int starts{}, stops{}, strength = 18;
    inline static esp_err_t start_result = ESP_OK, stop_result = ESP_OK;
    inline static UartEthModem* instance{};
    std::string apn;
    explicit UartEthModem(const Config& config) {
        instance = this;
        configured = config;
    }
    Config configured{};
    void SetNetworkEventCallback(std::function<void(UartEthModemEvent)> callback) { callback_ = callback; }
    esp_err_t SetPdpContext(const std::string& value, const std::string& type) {
        apn = value;
        return type == "IP" ? ESP_OK : ESP_FAIL;
    }
    int GetSignalStrength() { return strength; }
    esp_err_t Start() {
        ++starts;
        return start_result;
    }
    esp_err_t Stop() {
        ++stops;
        return stop_result;
    }
    void Emit(UartEthModemEvent event) { callback_(event); }

   private:
    std::function<void(UartEthModemEvent)> callback_;
};
