#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "device/local_control.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "remote_control/remote_control_agent.hpp"

namespace micropixel::firmware::local_control {

class UsbLocalControlAgent final {
   public:
    UsbLocalControlAgent(device::LocalControlBackend& backend, remote_control::RemoteControlAgent& host_commands);
    ~UsbLocalControlAgent();
    UsbLocalControlAgent(const UsbLocalControlAgent&) = delete;
    UsbLocalControlAgent& operator=(const UsbLocalControlAgent&) = delete;

    [[nodiscard]] bool Start();
    void Stop();

   private:
    static constexpr size_t kResponseCapacity = 1024U;
    static constexpr UBaseType_t kResponseQueueCapacity = 8U;

    struct Response final {
        std::array<char, kResponseCapacity> text{};
    };

    struct InstallSession final {
        uint8_t* data{};
        size_t size{};
        size_t received{};
        uint32_t request_id{};
        TickType_t last_activity{};
        std::array<char, remote_control::kRemoteControlAppIdCapacity> app_id{};
        std::array<uint8_t, 32U> sha256{};
    };

    static void ReceiveCommand(void* context, const char* command);
    static bool ProvideResponse(void* context, char* response, size_t capacity);
    static bool ReceiveHostResult(void* context, const remote_control::RemoteControlHostResult& result);

    void HandleCommand(const char* command);
    [[nodiscard]] bool PollResponse(char* response, size_t capacity);
    [[nodiscard]] bool HandleHostResult(const remote_control::RemoteControlHostResult& result);
    [[nodiscard]] bool QueueResponse(uint32_t request_id, const char* status, const char* detail);
    [[nodiscard]] bool QueueHostCommand(uint32_t request_id, remote_control::RemoteControlHostCommandType type,
                                        std::string_view app_id = {});
    void HandleAppList(uint32_t request_id, std::string_view arguments);
    void HandleInstallBegin(uint32_t request_id, std::string_view arguments);
    void HandleInstallChunk(uint32_t request_id, std::string_view arguments);
    void HandleInstallCommit(uint32_t request_id, std::string_view arguments);
    void HandleInstallAbort(uint32_t request_id, std::string_view arguments);
    void AbortInstall();
    void ExpireInstallIfNeeded();

    device::LocalControlBackend& backend_;
    remote_control::RemoteControlAgent& host_commands_;
    StaticQueue_t response_queue_storage_{};
    std::array<uint8_t, sizeof(Response) * kResponseQueueCapacity> response_queue_bytes_{};
    QueueHandle_t response_queue_{};
    remote_control::RemoteControlLocalSnapshot snapshot_workspace_{};
    InstallSession install_{};
    bool started_{};
};

}  // namespace micropixel::firmware::local_control
