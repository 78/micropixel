#include <atomic>
#include <cassert>
#include <cstring>
#include <thread>

#include "cellular_nvs_declarations.hpp"
#include "host/ui/cellular_status.hpp"
#include "platform/boards/metalio-claw4/cellular_controller.hpp"
#include "platform/boards/metalio-claw4/cellular_diagnostics.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "work/background_executor.hpp"

namespace {
int32_t stored_mode{};
bool mode_present{};
bool fail_commit{};
int failed_reads{};
int failed_writes{};
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
    if (failed_reads > 0) {
        --failed_reads;
        return ESP_FAIL;
    }
    *data = output_port;
    return ESP_OK;
}
esp_err_t i2c_master_transmit(void*, const uint8_t* bytes, size_t length, int) {
    assert(length == 2 && bytes[0] == 2);
    if (failed_writes > 0) {
        --failed_writes;
        return ESP_FAIL;
    }
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
    {
        using Slot = micropixel::device::CellularSimSlot;
        CellularController controller;
        controller.Configure(&bus, bus);
        controller.BindBackgroundExecutor(background);
        stored_mode = 1;
        assert(controller.Initialize());
        auto clear_commands = [] {
            UartEthModem::commands.clear();
            UartEthModem::timeouts.clear();
            UartEthModem::failed_command.clear();
        };
        // A missing SIM does not disable the command channel or require connection.
        UartEthModem::instance->Emit(UartEthModem::UartEthModemEvent::ErrorNoSim);
        controller.RequestSimRefresh();
        assert(controller.Snapshot().sim_pending);
        assert(!controller.SetSimSlot(Slot::kInternal));
        assert(!controller.SetEnabled(false));
        background.Run();
        assert(controller.Snapshot().sim_slot == Slot::kExternal);
        assert(!controller.Snapshot().sim_pending && !controller.Snapshot().sim_failed);
        for (const char* malformed : {"OK", "+ECSIMCFG: \"SimSlot\",10\r\nOK", "+ECSIMCFG: \"SimSlot\",-1\r\nOK",
                                      "+ECSIMCFG: \"SimSlot\",1garbage\r\nOK", "+ECSIMCFG: \"SimSlot\",\r\nOK"}) {
            UartEthModem::query_response = malformed;
            controller.RequestSimRefresh();
            background.Run();
            assert(controller.Snapshot().sim_slot == Slot::kUnknown && controller.Snapshot().sim_failed);
        }
        UartEthModem::query_response = "+ECSIMCFG: \"SimSimulator\",0\r\n+ECSIMCFG: \"SimSlot\", 1 \r\nOK";
        clear_commands();
        const int restarts_before_sim = restarts;
        assert(controller.SetSimSlot(Slot::kInternal));
        background.Run();
        assert((UartEthModem::commands ==
                std::vector<std::string>{"AT+CFUN=0", "AT+ECSIMCFG=SimSlot,1", "AT+CFUN=1", "AT+ECSIMCFG?"}));
        assert((UartEthModem::timeouts == std::vector<uint32_t>{8000, 5000, 15000, 5000}));
        assert(controller.Snapshot().sim_slot == Slot::kInternal && !controller.Snapshot().sim_failed);
        assert(restarts == restarts_before_sim + 1);
        // Selecting the known current slot must not enqueue work, drop RF or reboot.
        clear_commands();
        assert(controller.SetSimSlot(Slot::kInternal));
        assert(!controller.Snapshot().sim_pending);
        background.Run();
        assert(UartEthModem::commands.empty() && restarts == restarts_before_sim + 1);
        // First exercise slot-write failure while the current slot is still internal.
        clear_commands();
        UartEthModem::failed_command = "AT+ECSIMCFG=SimSlot,0";
        assert(controller.SetSimSlot(Slot::kExternal));
        background.Run();
        assert((UartEthModem::commands ==
                std::vector<std::string>{"AT+CFUN=0", "AT+ECSIMCFG=SimSlot,0", "AT+CFUN=1", "AT+ECSIMCFG?"}));
        assert(UartEthModem::timeouts[2] == 10000);
        assert(controller.Snapshot().sim_failed && controller.Snapshot().sim_slot == Slot::kInternal);
        assert(restarts == restarts_before_sim + 1);
        clear_commands();
        UartEthModem::failed_command = "AT+CFUN=0";
        assert(controller.SetSimSlot(Slot::kExternal));
        background.Run();
        assert((UartEthModem::commands == std::vector<std::string>{"AT+CFUN=0", "AT+ECSIMCFG?"}));
        assert(controller.Snapshot().sim_failed);
        // Failure to restore RF is best effort after successfully selecting a different SIM.
        clear_commands();
        UartEthModem::failed_command = "AT+CFUN=1";
        UartEthModem::query_response = "+ECSIMCFG: \"SimSlot\",0\r\nOK\r\n";
        assert(controller.SetSimSlot(Slot::kExternal));
        background.Run();
        assert(!controller.Snapshot().sim_failed && controller.Snapshot().sim_slot == Slot::kExternal);
        assert(restarts == restarts_before_sim + 2);
        clear_commands();
        // Queued requests must stay cancelled even if resume finishes before the worker runs.
        assert(controller.SetSimSlot(Slot::kInternal));
        assert(controller.Pause() == ESP_OK);
        assert(controller.Resume() == ESP_OK);
        assert(!controller.SetSimSlot(Slot::kInternal));  // Old job still owns the pending slot.
        background.Run();
        assert(UartEthModem::commands.empty() && !controller.Snapshot().sim_pending);
        assert(restarts == restarts_before_sim + 2);
        controller.RequestSimRefresh();
        assert(controller.Pause() == ESP_OK);
        assert(controller.Resume() == ESP_OK);
        background.Run();
        assert(UartEthModem::commands.empty() && !controller.Snapshot().sim_pending);
        // A new request after draining cancellation is accepted normally.
        controller.RequestSimRefresh();
        background.Run();
        assert(controller.Snapshot().sim_slot == Slot::kExternal);
        clear_commands();
        background.accepting = false;
        assert(!controller.SetSimSlot(Slot::kInternal));
        assert(!controller.Snapshot().sim_pending);
        background.accepting = true;
        assert(!controller.SetSimSlot(Slot::kUnknown));
        assert(!controller.SetSimSlot(static_cast<Slot>(42)));
        assert(controller.SetSimSlot(Slot::kInternal));
        assert(controller.Pause() == ESP_OK);
        background.Run();
        assert(UartEthModem::commands.empty() && !controller.Snapshot().sim_pending);
        assert(!controller.SetSimSlot(Slot::kInternal));
        assert(controller.Resume() == ESP_OK);
        controller.RequestSimRefresh();
        controller.Shutdown();
        background.Run();
        assert(UartEthModem::commands.empty() && !controller.Snapshot().sim_pending);
    }
    {
        CellularController controller;
        controller.Configure(&bus, bus);
        controller.BindBackgroundExecutor(background);
        assert(controller.Initialize());
        int starts = UartEthModem::starts;
        failed_reads = 1;
        assert(controller.Pause() == ESP_FAIL);
        assert(UartEthModem::starts == ++starts && output_port == 0xff);
        assert(controller.Snapshot().state == micropixel::device::CellularState::kConnecting);
        failed_writes = 1;
        assert(controller.Pause() == ESP_FAIL);
        assert(UartEthModem::starts == ++starts && output_port == 0xff);
        // A failed rollback is visible rather than reported as a successful recovery.
        failed_reads = 2;
        assert(controller.Pause() == ESP_FAIL);
        assert(controller.Snapshot().state == micropixel::device::CellularState::kFailed);
        assert(controller.Resume() == ESP_OK);
        starts = UartEthModem::starts;
        failed_writes = 1;
        controller.Shutdown();
        assert(UartEthModem::starts == starts);  // Never power back on during shutdown.
    }
    {
        using Slot = micropixel::device::CellularSimSlot;
        CellularController controller;
        controller.Configure(&bus, bus);
        controller.BindBackgroundExecutor(background);
        assert(controller.Initialize());
        assert(controller.TryBeginFirmwareUpdate());
        assert(!controller.TryBeginFirmwareUpdate());
        assert(!controller.SetEnabled(false));
        assert(!controller.SetSimSlot(Slot::kInternal));
        controller.RequestSimRefresh();
        assert(!controller.Snapshot().sim_pending);
        const int stops = UartEthModem::stops;
        assert(controller.Pause() == ESP_ERR_INVALID_STATE);
        assert(UartEthModem::stops == stops);  // Failed sleep cannot disrupt an update.
        controller.EndFirmwareUpdate();
        controller.RequestSimRefresh();
        assert(controller.Snapshot().sim_pending && !controller.TryBeginFirmwareUpdate());
        background.Run();
        assert(controller.TryBeginFirmwareUpdate());
        controller.EndFirmwareUpdate();
        assert(controller.SetSimSlot(Slot::kInternal));
        assert(!controller.TryBeginFirmwareUpdate());
        controller.Shutdown();
        background.Run();
        assert(!controller.TryBeginFirmwareUpdate());
    }
    {
        // OTA over Wi-Fi must still work when the cellular modem is intentionally off.
        CellularController controller;
        controller.Configure(&bus, bus);
        controller.BindBackgroundExecutor(background);
        stored_mode = 0;
        assert(controller.Initialize());
        assert(controller.TryBeginFirmwareUpdate());
        assert(!controller.SetEnabled(true));
        controller.EndFirmwareUpdate();
        assert(controller.SetEnabled(true));
        assert(!controller.TryBeginFirmwareUpdate());
        controller.Shutdown();
        background.Run();
        stored_mode = 1;
    }
    // Real concurrent submissions: precisely one side may reserve the network.
    // Exercise both a SIM change and a Wi-Fi/4G change against OTA acquisition.
    for (bool change_sim : {false, true}) {
        for (int iteration = 0; iteration < 32; ++iteration) {
            CellularController controller;
            controller.Configure(&bus, bus);
            controller.BindBackgroundExecutor(background);
            assert(controller.Initialize());
            std::atomic<bool> go{};
            bool updating = false;
            bool switching = false;
            std::thread update([&] {
                while (!go.load()) std::this_thread::yield();
                updating = controller.TryBeginFirmwareUpdate();
            });
            std::thread change([&] {
                while (!go.load()) std::this_thread::yield();
                switching = change_sim
                                ? controller.SetSimSlot(micropixel::device::CellularSimSlot::kInternal).has_value()
                                : controller.SetEnabled(false).has_value();
            });
            go = true;
            update.join();
            change.join();
            assert(updating != switching);
            if (updating) controller.EndFirmwareUpdate();
            controller.Shutdown();
            background.Run();  // Cancel the winning configuration job without restarting.
        }
    }
    {
        using micropixel::device::CellularSimStatus;
        using micropixel::device::CellularState;
        using micropixel::host_ui::DescribeCellularConnection;
        stored_mode = 1;
        UartEthModem::failed_command.clear();
        UartEthModem::responses = {
            {"AT+CPIN?", "\r\n+CPIN: READY\r\nOK"},
            {"AT+CFUN?", "+CFUN: 1\r\nOK"},
            {"AT+CSQ", "+CSQ: 99,99\r\nOK"},
            {"AT+CEREG?", "+CEREG: 2,2\r\nOK"},
            {"AT+CGATT?", "+CGATT: 0\r\nOK"},
            {"AT+COPS?", "+COPS: 0\r\nOK"},
            {"AT+CGDCONT?", "+CGDCONT: 11,\"IP\",\"wrong\"\r\n+CGDCONT: 1,\"IP\",\"eapn1.net\",,,,,\r\nOK"},
            {"AT+CGPADDR=1", "+CGPADDR: 1,0.0.0.0\r\nOK"},
        };
        CellularController controller;
        controller.Configure(&bus, bus);
        controller.BindBackgroundExecutor(background);
        assert(controller.Initialize());
        controller.RequestSimRefresh();
        background.Run();
        auto details = controller.Snapshot().diagnostics;
        assert(details.sampled && !details.incomplete);
        assert(details.sim_status == CellularSimStatus::kReady && details.signal_csq == 99);
        assert(details.registration == 2 && details.attached == 0 && details.radio_function == 1);
        assert(std::strcmp(details.apn.data(), "eapn1.net") == 0);
        assert(std::strcmp(DescribeCellularConnection(true, false, CellularState::kConnecting, details).title,
                           "Searching for network") == 0);
        UartEthModem::failed_command = "AT+CPIN?";
        UartEthModem::responses["AT+CPIN?"] = "+CME ERROR: 10\r\n";
        controller.RequestSimRefresh();
        background.Run();
        details = controller.Snapshot().diagnostics;
        assert(details.sim_status == CellularSimStatus::kAbsent);
        assert(std::strcmp(DescribeCellularConnection(true, false, CellularState::kFailed, details).title,
                           "No SIM detected") == 0);
        UartEthModem::failed_command.clear();
        UartEthModem::responses["AT+CPIN?"] = "+CPIN: READY\r\nOK";
        // Registration denial and no signal are distinct; never infer SIM activation from CSQ.
        UartEthModem::responses["AT+CEREG?"] = "+CEREG: 2,3\r\nOK";
        UartEthModem::responses["AT+COPS?"] = "+COPS: 0,0,\"Carrier, Test\",7\r\nOK";
        UartEthModem::responses["AT+CSQ"] = "+CSQ: 17,99\r\nOK";
        controller.RequestSimRefresh();
        background.Run();
        details = controller.Snapshot().diagnostics;
        assert(details.signal_csq == 17 && std::strcmp(details.operator_name.data(), "Carrier, Test") == 0);
        assert(std::strcmp(DescribeCellularConnection(true, false, CellularState::kFailed, details).title,
                           "Registration denied") == 0);
        // Failed/malformed queries replace previous values with unknown, not stale success.
        UartEthModem::failed_command = "AT+CSQ";
        UartEthModem::responses["AT+CEREG?"] = "+CEREG: 2,1garbage\r\nOK";
        UartEthModem::responses["AT+COPS?"] = "+COPS: 0,0,\"" + std::string(60, 'x') + "\",7\r\nOK";
        controller.RequestSimRefresh();
        background.Run();
        details = controller.Snapshot().diagnostics;
        assert(details.incomplete && details.signal_csq == -1 && details.registration == -1 &&
               details.operator_name[0] == 0);
        details.sim_status = CellularSimStatus::kPinRequired;
        assert(std::strcmp(DescribeCellularConnection(true, false, CellularState::kFailed, details).title,
                           "SIM PIN required") == 0);
        details.sim_status = CellularSimStatus::kPukRequired;
        assert(std::strcmp(DescribeCellularConnection(true, false, CellularState::kFailed, details).title,
                           "SIM PUK required") == 0);
        details.sim_status = CellularSimStatus::kReady;
        details.registration = 5;
        details.attached = 0;
        assert(std::strcmp(DescribeCellularConnection(true, false, CellularState::kConnecting, details).title,
                           "Registered, no mobile data") == 0);
        assert(std::strcmp(DescribeCellularConnection(false, false, CellularState::kOff, details).title, "4G is off") ==
               0);
        assert(std::strcmp(DescribeCellularConnection(true, true, CellularState::kConnected, details).title,
                           "Connected") == 0);
        const auto chinese = host_strings::ForTag("zh-CN");
        assert(std::strcmp(DescribeCellularConnection(true, true, CellularState::kConnected, details, chinese).title,
                           "已连接") == 0);
        assert(std::strcmp(DescribeCellularConnection(true, false, CellularState::kConnecting, details, chinese).title,
                           "已注册，未连接移动数据") == 0);
        assert(std::strcmp(micropixel::host_ui::CellularRegistrationText(5, chinese), "已注册（漫游）") == 0);
        UartEthModem::failed_command.clear();
        UartEthModem::responses.clear();
    }
}
