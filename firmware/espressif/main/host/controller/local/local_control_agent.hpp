#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "device/contracts/board_info.hpp"
#include "device/contracts/local_control.hpp"
#include "device/contracts/wifi.hpp"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/controller/control_dispatcher.hpp"
#include "host/logging/system_log_buffer.hpp"

namespace micropixel::firmware::local_control {

// Transport-independent MPX1 protocol handler. Boards supply the byte/line
// transport through the LocalControl device contract.
class LocalControlAgent final {
   public:
    LocalControlAgent(device::LocalControl& transport, control::ControlDispatcher& controls,
                      logging::SystemLogBuffer& system_logs, const device::BoardInfo& board_info, device::Wifi& wifi);
    ~LocalControlAgent();
    LocalControlAgent(const LocalControlAgent&) = delete;
    LocalControlAgent& operator=(const LocalControlAgent&) = delete;

    [[nodiscard]] bool Start();
    void Stop();

   private:
    static constexpr size_t kResponseCapacity = 2048U;
    static constexpr UBaseType_t kResponseQueueCapacity = 8U;

    struct Response final {
        std::array<char, kResponseCapacity> text{};
    };

    struct CommandWorkspace final {
        Response response{};
        std::array<char, kResponseCapacity> detail{};
        std::array<unsigned char, 1400U> encoded{};
    };

    struct InstallSession final {
        uint8_t* data{};
        size_t size{};
        size_t received{};
        uint32_t request_id{};
        TickType_t last_activity{};
        std::array<char, control::kAppIdCapacity> app_id{};
        std::array<uint8_t, 32U> sha256{};
    };

    static void ReceiveCommand(void* context, const char* command);
    static bool ProvideResponse(void* context, char* response, size_t capacity);
    static bool ReceiveHostResult(void* context, const control::HostResult& result);
    static void InstallTimeoutElapsed(void* context);
    static void RestartTimerElapsed(void* context);

    void HandleCommand(const char* command);
    [[nodiscard]] bool PollResponse(char* response, size_t capacity);
    [[nodiscard]] bool HandleHostResult(const control::HostResult& result);
    [[nodiscard]] bool QueueResponse(uint32_t request_id, const char* status, const char* detail);
    [[nodiscard]] bool QueueHostCommand(
        uint32_t request_id, control::HostCommandType type, std::string_view app_id = {},
        const micropixel_system_launch_arguments_response_t* launch_arguments = nullptr);
    void HandleAppList(uint32_t request_id, std::string_view arguments);
    void HandleAppLastError(uint32_t request_id, std::string_view arguments);
    void HandleLogRead(uint32_t request_id, std::string_view arguments);
    void HandleInputSequence(uint32_t request_id, std::string_view arguments);
    void HandleDeviceStatus(uint32_t request_id, std::string_view arguments);
    void HandleTaskDiagnostics(uint32_t request_id, std::string_view arguments);
    void HandleDeviceReboot(uint32_t request_id, std::string_view arguments);
    void HandleInstallBegin(uint32_t request_id, std::string_view arguments);
    void HandleInstallChunk(uint32_t request_id, std::string_view arguments);
    void HandleInstallCommit(uint32_t request_id, std::string_view arguments);
    void HandleInstallAbort(uint32_t request_id, std::string_view arguments);
    [[nodiscard]] bool ArmInstallTimeout(uint64_t delay_us);
    void DisarmInstallTimeout();
    void AbortInstall();
    void ExpireInstallIfNeeded();

    device::LocalControl& transport_;
    control::ControlDispatcher& controls_;
    logging::SystemLogBuffer& system_logs_;
    const device::BoardInfo& board_info_;
    device::Wifi& wifi_;
    StaticQueue_t response_queue_storage_{};
    uint8_t* response_queue_bytes_{};
    QueueHandle_t response_queue_{};
    StaticSemaphore_t response_workspace_mutex_storage_{};
    SemaphoreHandle_t response_workspace_mutex_{};
    CommandWorkspace* command_workspace_{};
    control::HostSnapshot* snapshot_workspace_{};
    InstallSession install_{};
    esp_timer_handle_t install_timer_{};
    esp_timer_handle_t restart_timer_{};
    std::atomic<bool> install_timeout_due_{};
    bool started_{};
};

}  // namespace micropixel::firmware::local_control
