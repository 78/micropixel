#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/controller/control_types.hpp"

namespace micropixel::firmware::control {

class ControlDispatcher final {
   public:
    using CommandReadySink = void (*)(void*);
    using ResultReadySink = void (*)(void*);
    using LocalResultSink = bool (*)(void*, const HostResult&);
    using CatalogSink = void (*)(void*, const CatalogSnapshot&);
    using LifecycleSink = void (*)(void*, const char*, const char*);
    using GuestLogLifecycleSink = void (*)(void*, const char*);

    explicit ControlDispatcher(GuestLogLifecycleSink guest_log_lifecycle_sink = nullptr,
                               void* guest_log_lifecycle_context = nullptr);
    ~ControlDispatcher();
    ControlDispatcher(const ControlDispatcher&) = delete;
    ControlDispatcher& operator=(const ControlDispatcher&) = delete;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool QueueRemoteCommand(const HostCommand& command);
    [[nodiscard]] bool QueueLocalCommand(const HostCommand& command);
    [[nodiscard]] bool PeekHostCommand(HostCommand& command) const;
    [[nodiscard]] bool PollHostCommand(HostCommand& command, TickType_t timeout = 0U);
    [[nodiscard]] bool SubmitHostResult(const HostResult& result);
    [[nodiscard]] bool PollRemoteResult(HostResult& result);

    void UpdateInstalledApps(const CatalogSnapshot& catalog);
    void UpdateAppLifecycle(const char* app_id, const char* lifecycle);
    void UpdateLastAppDiagnostic(const AppDiagnostic& diagnostic);
    void CopySnapshot(HostSnapshot& snapshot) const;
    void RequestStoreCheck() {
        store_check_state_.store(1U);
        store_check_requested_.store(true);
    }
    [[nodiscard]] bool ConsumeStoreCheck() { return store_check_requested_.exchange(false); }
    void SetStoreCheckState(uint8_t state) { store_check_state_.store(state); }
    [[nodiscard]] uint8_t StoreCheckState() const { return store_check_state_.load(); }
    void ResetStoreUpdates();
    void AddStoreUpdate(const char* app_id, const char* version, const char* state,
                        const std::array<uint8_t, 32U>& digest);
    [[nodiscard]] StoreAppUpdate FindStoreUpdate(const char* app_id) const;
    void RequestStoreUpdate(const char* app_id);
    [[nodiscard]] bool ConsumeStoreUpdate(std::array<char, kAppIdCapacity>& app_id);
    void UpdateStoreSnapshot(const StoreSnapshot& snapshot);
    [[nodiscard]] StoreSnapshot CopyStoreSnapshot() const;
    [[nodiscard]] bool BeginInstallActivity(ControlSource source, const char* command_id, const char* app_id);
    void UpdateInstallProgress(ControlSource source, const char* command_id, uint8_t progress_percent);
    void EndInstallActivity(ControlSource source, const char* command_id);
    void CopyInstallActivity(InstallActivity& activity) const;

    void SetCommandReadySink(CommandReadySink sink, void* context);
    void SetRemoteResultReadySink(ResultReadySink sink, void* context);
    void SetLocalResultSink(LocalResultSink sink, void* context);
    void SetCatalogSink(CatalogSink sink, void* context);
    void SetLifecycleSink(LifecycleSink sink, void* context);

   private:
    static constexpr UBaseType_t kHostCommandQueueCapacity = 4U;
    static constexpr UBaseType_t kRemoteResultQueueCapacity = 4U;

    [[nodiscard]] bool QueueCommand(const HostCommand& command, bool local);

    mutable std::mutex snapshot_mutex_;
    HostSnapshot* snapshot_{};
    InstallActivity install_activity_{};
    StoreSnapshot store_snapshot_{};
    std::array<StoreAppUpdate, kMaxApps> store_updates_{};
    uint32_t store_update_count_{};
    std::array<char, kAppIdCapacity> store_update_requested_{};
    std::atomic<bool> store_check_requested_{};
    std::atomic<uint8_t> store_check_state_{};
    StaticQueue_t host_command_queue_storage_{};
    uint8_t* host_command_queue_bytes_{};
    QueueHandle_t host_command_queue_{};
    StaticQueue_t remote_result_queue_storage_{};
    uint8_t* remote_result_queue_bytes_{};
    QueueHandle_t remote_result_queue_{};
    std::atomic<CommandReadySink> command_ready_sink_{};
    std::atomic<void*> command_ready_context_{};
    std::atomic<ResultReadySink> remote_result_ready_sink_{};
    std::atomic<void*> remote_result_ready_context_{};
    std::atomic<LocalResultSink> local_result_sink_{};
    std::atomic<void*> local_result_context_{};
    std::atomic<CatalogSink> catalog_sink_{};
    std::atomic<void*> catalog_context_{};
    std::atomic<LifecycleSink> lifecycle_sink_{};
    std::atomic<void*> lifecycle_context_{};
    GuestLogLifecycleSink guest_log_lifecycle_sink_{};
    void* guest_log_lifecycle_context_{};
};

void ReleaseHostCommand(const HostCommand& command);
void ReleaseArtifacts(const HostResult& result);

}  // namespace micropixel::firmware::control
