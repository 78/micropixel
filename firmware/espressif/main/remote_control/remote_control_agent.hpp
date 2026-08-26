#ifndef MICROPIXEL_FIRMWARE_REMOTE_CONTROL_AGENT_HPP
#define MICROPIXEL_FIRMWARE_REMOTE_CONTROL_AGENT_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

#include "device/input.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host_ui/system_ui.hpp"
#include "remote_control/control_protocol.hpp"
#include "runtime/guest_log_sink.hpp"

struct cJSON;

namespace micropixel::device {
class WifiBackend;
}

namespace micropixel::firmware::remote_control {

constexpr size_t kRemoteControlMaxApps = 50U;
constexpr size_t kRemoteControlAppIdCapacity = 65U;
constexpr size_t kRemoteControlDisplayNameCapacity = 65U;

struct RemoteControlAppDescriptor final {
    std::array<char, kRemoteControlAppIdCapacity> app_id{};
    std::array<char, kRemoteControlDisplayNameCapacity> display_name{};
    uint32_t bundle_size{};
};

struct RemoteControlCatalogSnapshot final {
    std::array<RemoteControlAppDescriptor, kRemoteControlMaxApps> apps{};
    uint32_t count{};
    uint32_t store_total_bytes{};
    uint32_t store_used_bytes{};
};

struct RemoteControlLocalSnapshot final {
    RemoteControlCatalogSnapshot catalog{};
    std::array<char, kRemoteControlAppIdCapacity> active_app_id{};
    std::array<char, 24U> lifecycle{};
};

constexpr size_t kRemoteControlCommandIdCapacity = 64U;
constexpr size_t kRemoteControlMaxSequenceOperations = 16U;
constexpr size_t kRemoteControlMaxResultArtifacts = 4U;

enum class RemoteControlHostCommandType : uint8_t {
    kCaptureScreen,
    kStartApp,
    kStopApp,
    kInstallApp,
    kUninstallApp,
    kInputSequence,
};

enum class RemoteControlSequenceOperationType : uint8_t {
    kTouch,
    kKey,
    kCaptureScreen,
};

struct RemoteControlSequenceOperation final {
    RemoteControlSequenceOperationType type{RemoteControlSequenceOperationType::kTouch};
    uint32_t delay_ms{};
    device::TouchSample touch{};
    device::KeySample key{};
    std::array<char, 33U> capture_id{};
};

struct RemoteControlHostCommand final {
    std::array<char, kRemoteControlCommandIdCapacity> command_id{};
    RemoteControlHostCommandType type{RemoteControlHostCommandType::kCaptureScreen};
    TickType_t deadline_ticks{};
    std::array<char, kRemoteControlAppIdCapacity> app_id{};
    std::array<RemoteControlSequenceOperation, kRemoteControlMaxSequenceOperations> operations{};
    uint32_t operation_count{};
    uint8_t* package_data{};
    size_t package_size{};
    std::array<uint8_t, 32U> package_sha256{};
};

struct RemoteControlArtifact final {
    std::array<char, 33U> capture_id{};
    uint8_t* data{};
    size_t size{};
    uint32_t width{};
    uint32_t height{};
    host_ui::ScreenCaptureRelease release{};
};

struct RemoteControlAppDiagnostic final {
    std::array<char, kRemoteControlAppIdCapacity> app_id{};
    std::array<char, 24U> phase{};
    std::array<char, 48U> code{};
    std::array<char, 256U> detail{};
    int32_t exit_code{};
    bool has_exit_code{};
};

struct RemoteControlHostResult final {
    std::array<char, kRemoteControlCommandIdCapacity> command_id{};
    std::array<char, 96U> message{};
    std::array<RemoteControlArtifact, kRemoteControlMaxResultArtifacts> artifacts{};
    RemoteControlAppDiagnostic diagnostic{};
    uint32_t artifact_count{};
    bool has_diagnostic{};
    bool ok{};
};

// Host-owned development control agent. Network work runs on its own bounded
// task and publishes immutable snapshots to the System Shell; it never calls
// WAMR, LVGL, or board drivers from an HTTP/3 callback.
class RemoteControlAgent final : public runtime::GuestLogSink {
   public:
    using HostCommandReadySink = void (*)(void*);
    using LocalHostResultSink = bool (*)(void*, const RemoteControlHostResult&);

    explicit RemoteControlAgent(device::WifiBackend& wifi);
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
    void UpdateInstalledApps(const RemoteControlCatalogSnapshot& catalog);
    void UpdateAppLifecycle(const char* app_id, const char* lifecycle);
    void WriteGuestLog(const char* app_id, uint32_t level, const uint8_t* bytes, size_t length,
                       uint64_t timestamp_us) override;
    [[nodiscard]] bool PeekHostCommand(RemoteControlHostCommand& command) const;
    [[nodiscard]] bool PollHostCommand(RemoteControlHostCommand& command, TickType_t timeout = 0U);
    [[nodiscard]] bool QueueLocalHostCommand(const RemoteControlHostCommand& command);
    [[nodiscard]] bool SubmitHostResult(const RemoteControlHostResult& result);
    void CopyLocalSnapshot(RemoteControlLocalSnapshot& snapshot) const;
    void SetHostCommandReadySink(HostCommandReadySink sink, void* context);
    void SetLocalHostResultSink(LocalHostResultSink sink, void* context);

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

    struct Command final {
        CommandType type{CommandType::kSetEnabled};
        bool enabled{};
    };

    struct Identity final {
        std::array<char, host_ui::kRemoteControlDeviceIdCapacity> device_id{};
        std::array<char, 1024U> credential{};
        uint32_t auth_epoch{1U};
    };

    struct GuestLogBuffer;

    struct TaskRuntimeSample final {
        TaskHandle_t handle{};
        uint64_t runtime_counter{};
    };

    struct PendingResultBody final {
        uint8_t* data{};
        size_t size{};
    };

    using FirmwareStatusPublisher = std::function<void()>;

    static void TaskEntry(void* context);
    void TaskMain();
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
    [[nodiscard]] bool PostGuestLogs(void* client, const Identity& identity, const char* command_id,
                                     uint64_t after_sequence);
    [[nodiscard]] bool RefreshFirmwareRelease(void* client);
    [[nodiscard]] bool ApplyFirmwareUpdate(void* client, const Identity& identity, const cJSON* params,
                                           const char* command_id, const FirmwareStatusPublisher& publish_status);
    [[nodiscard]] cJSON* CreateGuestLogPayload(uint64_t after_sequence, size_t maximum_entries,
                                               uint64_t& next_cursor_out, bool& has_entries_out) const;
    [[nodiscard]] bool QueueHostCommand(void* client, const Identity& identity, const cJSON* root, const char* name,
                                        const char* command_id, uint32_t timeout_ms);
    void DrainHostResults(void* client, const Identity& identity);
    [[nodiscard]] bool UploadArtifact(void* client, const Identity& identity, const RemoteControlArtifact& artifact,
                                      cJSON* artifacts);
    [[nodiscard]] bool DownloadPackage(void* client, const Identity& identity, const char* path, size_t size,
                                       uint8_t*& data_out, bool report_firmware_progress = false,
                                       const FirmwareStatusPublisher& publish_status = {});
    static void ReleaseHostCommand(const RemoteControlHostCommand& command);
    static void ReleaseArtifacts(const RemoteControlHostResult& result);
    void HandleControlBytes(void* client, const Identity& identity, const uint8_t* bytes, size_t size,
                            const FirmwareStatusPublisher& publish_status);
    void HandleControlLine(void* client, const Identity& identity, const char* line,
                           const FirmwareStatusPublisher& publish_status);
    [[nodiscard]] bool RememberCommandId(const char* command_id);
    [[nodiscard]] bool ReplayCommandState(void* client, const Identity& identity, const char* command_id);
    void CacheCompletedResult(const char* command_id, const uint8_t* body, size_t body_size);

    device::WifiBackend& wifi_;
    mutable std::mutex model_mutex_;
    host_ui::RemoteControlModel model_{};
    mutable std::mutex diagnostics_mutex_;
    RemoteControlCatalogSnapshot installed_apps_{};
    std::array<char, kRemoteControlAppIdCapacity> active_app_id_{};
    std::array<char, 24U> app_lifecycle_{};
    std::array<char, kRemoteControlCommandIdCapacity> app_session_id_{};
    std::array<char, kRemoteControlCommandIdCapacity> last_app_session_id_{};
    uint64_t runtime_snapshot_generation_{};
    uint64_t published_runtime_snapshot_generation_{};
    static constexpr size_t kTaskDiagnosticCapacity = 48U;
    std::array<TaskRuntimeSample, kTaskDiagnosticCapacity> previous_task_runtime_{};
    uint64_t previous_total_runtime_{};
    mutable std::mutex log_mutex_;
    GuestLogBuffer* guest_logs_{};
    StaticQueue_t command_queue_storage_{};
    std::array<uint8_t, sizeof(Command) * kCommandQueueCapacity> command_queue_bytes_{};
    QueueHandle_t command_queue_{};
    StaticQueue_t host_command_queue_storage_{};
    uint8_t* host_command_queue_bytes_{};
    QueueHandle_t host_command_queue_{};
    std::atomic<HostCommandReadySink> host_command_ready_sink_{};
    std::atomic<void*> host_command_ready_context_{};
    std::atomic<LocalHostResultSink> local_host_result_sink_{};
    std::atomic<void*> local_host_result_context_{};
    StaticQueue_t host_result_queue_storage_{};
    uint8_t* host_result_queue_bytes_{};
    QueueHandle_t host_result_queue_{};
    std::array<PendingResultBody, kPendingResultCapacity> pending_results_{};
    size_t pending_result_start_{};
    size_t pending_result_count_{};
    StaticSemaphore_t stopped_semaphore_storage_{};
    SemaphoreHandle_t stopped_semaphore_{};
    TaskHandle_t task_{};
    std::array<char, kControlLineCapacity> control_line_{};
    size_t control_line_size_{};
    bool control_line_overflow_{};
    std::array<std::array<char, kRemoteControlCommandIdCapacity>, kRecentCommandCapacity> recent_command_ids_{};
    std::array<PendingResultBody, kRecentCommandCapacity> recent_command_results_{};
    size_t recent_command_start_{};
    size_t recent_command_count_{};
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
