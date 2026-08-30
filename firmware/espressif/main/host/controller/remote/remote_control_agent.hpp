#ifndef MICROPIXEL_FIRMWARE_REMOTE_CONTROL_AGENT_HPP
#define MICROPIXEL_FIRMWARE_REMOTE_CONTROL_AGENT_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

#include "device/contracts/board_info.hpp"
#include "device/contracts/input.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/controller/control_dispatcher.hpp"
#include "host/controller/remote/remote_control_protocol.hpp"
#include "host/controller/remote/remote_identity_store.hpp"
#include "host/logging/system_log_buffer.hpp"
#include "host/ui/system_ui.hpp"

struct cJSON;

namespace micropixel::device {
class Wifi;
}

namespace micropixel::firmware::remote_control {

// Host-owned development control agent. Network work runs on its own bounded
// task and publishes immutable snapshots to the System Shell; it never calls
// WAMR, LVGL, or board drivers from an HTTP/3 callback.
class RemoteControlAgent final {
   public:
    RemoteControlAgent(device::Wifi& wifi, const device::BoardInfo& board_info, control::ControlDispatcher& controls,
                       logging::SystemLogBuffer& system_logs, bool screen_capture_supported);
    RemoteControlAgent(const RemoteControlAgent&) = delete;
    RemoteControlAgent& operator=(const RemoteControlAgent&) = delete;
    ~RemoteControlAgent();

    [[nodiscard]] bool Start(bool enabled);
    void Stop();
    void Stop(TickType_t timeout);
    [[nodiscard]] bool SetEnabled(bool enabled);
    [[nodiscard]] bool RequestPairingCode();
    [[nodiscard]] bool CancelPairingCode();
    [[nodiscard]] bool RequestFirmwareUpdate();
    [[nodiscard]] host_ui::RemoteControlModel Snapshot() const;
    [[nodiscard]] device::BoardInfo BoardInfo() const;
    void UpdateInstalledApps(const control::CatalogSnapshot& catalog);
    void UpdateAppLifecycle(const char* app_id, const char* lifecycle);
    void NotifyNetworkChanged();

   private:
    static constexpr UBaseType_t kCommandQueueCapacity = 8U;
    static constexpr UBaseType_t kHostCommandQueueCapacity = 4U;
    static constexpr UBaseType_t kHostResultQueueCapacity = 4U;
    static constexpr size_t kPendingResultCapacity = 4U;
    static constexpr size_t kControlLineCapacity = 4096U;
    static constexpr size_t kRecentCommandCapacity = 16U;

    enum class CommandType : uint8_t {
        kSetEnabled,
        kRequestPairingCode,
        kCancelPairingCode,
        kRequestFirmwareUpdate,
        kShutdown,
    };

    static constexpr uint32_t kWorkCommand = 1U << 0U;
    static constexpr uint32_t kWorkHostResult = 1U << 1U;
    static constexpr uint32_t kWorkNetwork = 1U << 2U;
    static constexpr uint32_t kWorkTransport = 1U << 3U;
    static constexpr uint32_t kWorkRuntimeSnapshot = 1U << 4U;

    struct Command final {
        CommandType type{CommandType::kSetEnabled};
        bool enabled{};
    };

    using Identity = RemoteIdentity;

    struct ColdState;
    struct TaskContext;

    struct TaskRuntimeSample final {
        TaskHandle_t handle{};
        uint64_t runtime_counter{};
    };

    struct PendingResultBody final {
        uint8_t* data{};
        size_t size{};
    };

    using FirmwareStatusPublisher = std::function<void()>;
    using PackageProgressPublisher = std::function<void(uint8_t)>;

    static void TaskEntry(void* context);
    static void TransportReady(void* context);
    static void RemoteResultReady(void* context);
    static void InstalledAppsChanged(void* context, const control::CatalogSnapshot& catalog);
    static void AppLifecycleChanged(void* context, const char* app_id, const char* lifecycle);
    void TaskMain();
    void NotifyTask(uint32_t work_bits);
    [[nodiscard]] uint32_t WaitForWork(TickType_t timeout);
    [[nodiscard]] bool AllocateTaskContext();
    void ReleaseTaskContext();
    [[nodiscard]] bool QueueCommand(const Command& command);
    void SetConnectionState(host_ui::RemoteControlConnectionState state, const char* message);
    void SetIdentityInSnapshot(const Identity& identity);
    void ClearPairingInSnapshot(const char* message);
    void RefreshPairingDeadline();
    [[nodiscard]] bool LoadIdentity(Identity& identity) const;
    [[nodiscard]] bool SaveIdentity(const Identity& identity) const;
    [[nodiscard]] bool ClearIdentity(Identity& identity);
    [[nodiscard]] bool Bootstrap(void* client, Identity& identity);
    [[nodiscard]] bool RefreshCredential(void* client, Identity& identity);
    [[nodiscard]] bool PostCommandResult(void* client, const Identity& identity, const char* command_id, bool ok,
                                         cJSON* result);
    [[nodiscard]] bool PostCommandAccepted(void* client, const Identity& identity, const char* command_id);
    [[nodiscard]] bool PostEvent(void* client, const Identity& identity, cJSON* root, const char* type);
    [[nodiscard]] const uint8_t* SerializeJson(cJSON* root, size_t& size_out);
    [[nodiscard]] bool SendCommandResultBody(void* client, const Identity& identity, const uint8_t* body,
                                             size_t body_size);
    [[nodiscard]] bool QueuePendingResult(const uint8_t* body, size_t body_size);
    void FlushPendingResults(void* client, const Identity& identity);
    void ClearPendingResults();
    [[nodiscard]] bool PostUnsupportedCommandResult(void* client, const Identity& identity, const char* command_id);
    [[nodiscard]] bool PostRestartResult(void* client, const Identity& identity, const char* command_id);
    [[nodiscard]] bool PostFirmwareUpdateStatus(void* client, const Identity& identity);
    void PublishRuntimeSnapshotIfChanged(void* client, const Identity& identity);
    [[nodiscard]] bool PostSystemInformation(void* client, const Identity& identity, const char* command_id);
    [[nodiscard]] bool PostTaskDiagnostics(void* client, const Identity& identity, const char* command_id);
    [[nodiscard]] bool PostInstalledApps(void* client, const Identity& identity, const char* command_id);
    [[nodiscard]] bool PostSystemLogs(void* client, const Identity& identity, const char* command_id,
                                      uint64_t after_sequence, logging::LogSourceFilter filter);
    [[nodiscard]] bool RefreshFirmwareRelease(void* client);
    [[nodiscard]] bool ApplyFirmwareUpdate(void* client, const Identity& identity, const cJSON* params,
                                           const char* command_id, const FirmwareStatusPublisher& publish_status);
    [[nodiscard]] bool QueueHostCommand(void* client, const Identity& identity, const cJSON* root, const char* name,
                                        const char* command_id, uint32_t timeout_ms);
    void DrainHostResults(void* client, const Identity& identity);
    [[nodiscard]] bool UploadArtifact(void* client, const Identity& identity, const control::Artifact& artifact,
                                      cJSON* artifacts);
    [[nodiscard]] bool DownloadPackage(void* client, const Identity& identity, const char* path, size_t size,
                                       uint8_t*& data_out, bool report_firmware_progress = false,
                                       const FirmwareStatusPublisher& publish_status = {},
                                       const PackageProgressPublisher& publish_package_progress = {});
    void HandleControlBytes(void* client, const Identity& identity, const uint8_t* bytes, size_t size,
                            const FirmwareStatusPublisher& publish_status);
    void HandleControlLine(void* client, const Identity& identity, const char* line,
                           const FirmwareStatusPublisher& publish_status);
    [[nodiscard]] bool RememberCommandId(const char* command_id);
    [[nodiscard]] bool ReplayCommandState(void* client, const Identity& identity, const char* command_id);
    void CacheCompletedResult(const char* command_id, const uint8_t* body, size_t body_size);

    control::ControlDispatcher& controls_;
    device::Wifi& wifi_;
    const device::BoardInfo& board_info_;
    RemoteIdentityStore identity_store_;
    mutable std::mutex model_mutex_;
    host_ui::RemoteControlModel model_{};
    mutable std::mutex diagnostics_mutex_;
    ColdState* cold_state_{};
    std::array<char, control::kAppIdCapacity> active_app_id_{};
    std::array<char, 24U> app_lifecycle_{};
    std::array<char, control::kCommandIdCapacity> app_session_id_{};
    std::array<char, control::kCommandIdCapacity> last_app_session_id_{};
    uint64_t runtime_snapshot_generation_{};
    uint64_t published_runtime_snapshot_generation_{};
    static constexpr size_t kTaskDiagnosticCapacity = 48U;
    uint64_t previous_total_runtime_{};
    logging::SystemLogBuffer& system_logs_;
    StaticQueue_t command_queue_storage_{};
    std::array<uint8_t, sizeof(Command) * kCommandQueueCapacity> command_queue_bytes_{};
    QueueHandle_t command_queue_{};
    std::array<PendingResultBody, kPendingResultCapacity> pending_results_{};
    size_t pending_result_start_{};
    size_t pending_result_count_{};
    StaticSemaphore_t stopped_semaphore_storage_{};
    SemaphoreHandle_t stopped_semaphore_{};
    TaskHandle_t task_{};
    std::atomic<TaskHandle_t> notification_task_{nullptr};
    TaskContext* task_context_{};
    size_t control_line_size_{};
    bool control_line_overflow_{};
    std::array<PendingResultBody, kRecentCommandCapacity> recent_command_results_{};
    size_t recent_command_start_{};
    size_t recent_command_count_{};
    bool screen_capture_supported_{};
    protocol::Uuid device_boot_id_{};
    protocol::Uuid control_session_id_{};
    uint64_t event_sequence_{};
    bool shutdown_requested_{};
    bool pairing_requested_{};
    bool pairing_cancel_requested_{};
    bool firmware_update_requested_{};
    std::array<char, 256U> firmware_download_path_{};
    std::array<uint8_t, 32U> firmware_sha256_{};
    size_t firmware_size_{};
    TickType_t pairing_deadline_ticks_{};
};

}  // namespace micropixel::firmware::remote_control

#endif
