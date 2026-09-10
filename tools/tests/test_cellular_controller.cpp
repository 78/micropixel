#include <cassert>
#include <cstring>

#include "cellular_nvs_declarations.hpp"
#include "platform/boards/metalio-claw4/cellular_controller.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "work/background_executor.hpp"

namespace {
int32_t stored_mode{};
bool mode_present{};
bool fail_commit{};
uint8_t output_port = 0xff;
int restarts{};
int wakeups{};
}  // namespace

esp_err_t nvs_open_from_partition(const char* partition, const char* name, int, nvs_handle_t* handle) {
    assert(std::strcmp(partition, "runtime_nvs") == 0 && std::strcmp(name, "network") == 0);
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_i32(nvs_handle_t, const char* key, int32_t* mode) {
    assert(std::strcmp(key, "type") == 0);
    *mode = stored_mode;
    return mode_present ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_i32(nvs_handle_t, const char*, int32_t mode) {
    stored_mode = mode;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) { return fail_commit ? ESP_FAIL : ESP_OK; }
void esp_restart() { ++restarts; }
esp_err_t i2c_master_transmit_receive(void*, const uint8_t* address, size_t size, uint8_t* data, size_t length, int) {
    assert(size == 1 && *address == 2 && length == 1);
    *data = output_port;
    return ESP_OK;
}
esp_err_t i2c_master_transmit(void*, const uint8_t* bytes, size_t length, int) {
    assert(length == 2 && bytes[0] == 2);
    output_port = bytes[1];
    return ESP_OK;
}

int main() {
    using micropixel::platform::metalio_claw4::CellularController;
    micropixel::platform::buses::I2cExecutor bus;
    micropixel::work::BackgroundExecutor background;
    {
        CellularController controller;
        controller.Configure(&bus, bus);
        controller.BindBackgroundExecutor(background);
        controller.SetStateChangeSink([](void*) { ++wakeups; }, nullptr);
        assert(controller.Initialize());
        assert(controller.Snapshot().available && !controller.Snapshot().enabled);
        assert(output_port == 0x7f);  // Only NT26 changes; other TCA power outputs survive.
        assert(UartEthModem::starts == 0);
        background.accepting = false;
        assert(!controller.SetEnabled(true));
        assert(!controller.Snapshot().switching);
        background.accepting = true;
        fail_commit = true;
        assert(controller.SetEnabled(true));
        assert(controller.Snapshot().switching);
        assert(!controller.SetEnabled(true));
        background.Run();
        assert(restarts == 0 && !controller.Snapshot().switching);
        fail_commit = false;
        assert(controller.SetEnabled(true));
        background.Run();
        assert(restarts == 1 && stored_mode == 1 && wakeups > 0);
    }
    mode_present = true;
    {
        CellularController controller;
        controller.Configure(&bus, bus);
        controller.BindBackgroundExecutor(background);
        assert(controller.Initialize());
        assert(controller.Snapshot().enabled && UartEthModem::starts == 1);
        assert(output_port == 0xff);
        assert(UartEthModem::instance->configured.baud_rate == 2000000);
        assert(UartEthModem::instance->apn == "eapn1.net");
        UartEthModem::instance->Emit(UartEthModem::UartEthModemEvent::Connected);
        assert(controller.Snapshot().connected);
        background.Run();
        assert(controller.Snapshot().signal_bars == 3);
        const int strengths[]{0, 9, 10, 14, 15, 19, 20, 31, 99, -1};
        const int bars[]{1, 1, 2, 2, 3, 3, 4, 4, 0, 0};
        for (size_t i = 0; i < std::size(strengths); ++i) {
            UartEthModem::instance->Emit(UartEthModem::UartEthModemEvent::Disconnected);
            UartEthModem::strength = strengths[i];
            UartEthModem::instance->Emit(UartEthModem::UartEthModemEvent::Connected);
            background.Run();
            assert(controller.Snapshot().signal_bars == bars[i]);
        }
        UartEthModem::instance->Emit(UartEthModem::UartEthModemEvent::Disconnected);
        assert(!controller.Snapshot().connected);
        UartEthModem::stop_result = ESP_FAIL;
        assert(controller.Pause() != ESP_OK && output_port == 0xff);
        UartEthModem::stop_result = ESP_OK;
        assert(controller.Pause() == ESP_OK && output_port == 0x7f);
        assert(controller.Resume() == ESP_OK && output_port == 0xff && UartEthModem::starts == 2);
        assert(controller.SetEnabled(false));
        controller.Shutdown();
        background.Run();
        assert(restarts == 1);  // A queued mode change cannot restart during power-off.
        assert(controller.Resume() == ESP_OK && output_port == 0x7f);
        assert(!controller.SetEnabled(false));
    }
}
