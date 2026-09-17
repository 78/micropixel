#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

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
        ModemReset,
        RegistrationLost,
        PlmnSearchFallback,
        ErrorInitFailed
    };
    struct Config {
        int uart_num, baud_rate, tx_pin, rx_pin, mrdy_pin, srdy_pin;
        size_t rx_buffer_count, rx_buffer_size, tx_queue_depth;
        bool use_psram;
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
    void SetDebug(bool) {}
    bool at_ready = true;
    bool IsAtReady() const { return at_ready; }
    void SetNetworkEventCallback(std::function<void(UartEthModemEvent, const std::string&)> callback) {
        callback_ = callback;
    }
    void SetPdpContext(const std::string& value, const std::string& type) {
        apn = value;
        (void)type;
    }
    inline static std::vector<std::string> commands;
    inline static std::vector<uint32_t> timeouts;
    inline static std::string query_response = "\r\n+ECSIMCFG: \"SimSlot\",0\r\nOK\r\n";
    inline static std::string failed_command;
    inline static std::map<std::string, std::string> responses;
    std::function<void(const std::string&)> on_send;
    esp_err_t SendAt(const std::string& command, std::string& response, uint32_t timeout) {
        commands.push_back(command);
        if (on_send) on_send(command);
        timeouts.push_back(timeout);
        response = command == "AT+ECSIMCFG?"     ? query_response
                   : responses.contains(command) ? responses.at(command)
                                                 : "OK";
        return command == failed_command ? ESP_FAIL : ESP_OK;
    }
    inline static esp_err_t prepare_result = ESP_OK;
    inline static int prepares{};
    esp_err_t PrepareForShutdown(uint32_t = 3000) {
        ++prepares;
        return prepare_result;
    }
    int GetSignalStrength() { return strength; }
    esp_err_t Start() {
        ++starts;
        return start_result;
    }
    inline static uint32_t last_stop_timeout{};
    esp_err_t Stop(uint32_t timeout = 5000) {
        last_stop_timeout = timeout;
        ++stops;
        return stop_result;
    }
    void Emit(UartEthModemEvent event) { callback_(event, ""); }

   private:
    std::function<void(UartEthModemEvent, const std::string&)> callback_;
};
