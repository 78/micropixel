#include "host/controller/control_dispatcher.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "freertos/task.h"

namespace micropixel::firmware::control {
namespace {

template <typename Sink>
void SetSink(std::atomic<Sink>& destination, std::atomic<void*>& destination_context, Sink sink, void* context) {
    if (sink == nullptr) {
        destination.store(nullptr, std::memory_order_release);
        destination_context.store(nullptr, std::memory_order_release);
        return;
    }
    destination_context.store(context, std::memory_order_release);
    destination.store(sink, std::memory_order_release);
}

}  // namespace

ControlDispatcher::ControlDispatcher(GuestLogLifecycleSink guest_log_lifecycle_sink, void* guest_log_lifecycle_context)
    : guest_log_lifecycle_sink_(guest_log_lifecycle_sink), guest_log_lifecycle_context_(guest_log_lifecycle_context) {
    install_chunk_ = static_cast<uint8_t*>(heap_caps_malloc(kInstallChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    snapshot_ =
        static_cast<HostSnapshot*>(heap_caps_calloc(1U, sizeof(HostSnapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    host_command_queue_bytes_ = static_cast<uint8_t*>(
        heap_caps_calloc(kHostCommandQueueCapacity, sizeof(HostCommand), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    remote_result_queue_bytes_ = static_cast<uint8_t*>(
        heap_caps_calloc(kRemoteResultQueueCapacity, sizeof(HostResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (host_command_queue_bytes_ == nullptr) {
        host_command_queue_bytes_ =
            static_cast<uint8_t*>(heap_caps_calloc(kHostCommandQueueCapacity, sizeof(HostCommand), MALLOC_CAP_8BIT));
    }
    if (remote_result_queue_bytes_ == nullptr) {
        remote_result_queue_bytes_ =
            static_cast<uint8_t*>(heap_caps_calloc(kRemoteResultQueueCapacity, sizeof(HostResult), MALLOC_CAP_8BIT));
    }
    if (host_command_queue_bytes_ != nullptr) {
        host_command_queue_ = xQueueCreateStatic(kHostCommandQueueCapacity, sizeof(HostCommand),
                                                 host_command_queue_bytes_, &host_command_queue_storage_);
    }
    if (remote_result_queue_bytes_ != nullptr) {
        remote_result_queue_ = xQueueCreateStatic(kRemoteResultQueueCapacity, sizeof(HostResult),
                                                  remote_result_queue_bytes_, &remote_result_queue_storage_);
    }
}

ControlDispatcher::~ControlDispatcher() {
    HostCommand command{};
    while (host_command_queue_ != nullptr && xQueueReceive(host_command_queue_, &command, 0U) == pdTRUE) {
        ReleaseHostCommand(command);
    }
    HostResult result{};
    while (remote_result_queue_ != nullptr && xQueueReceive(remote_result_queue_, &result, 0U) == pdTRUE) {
        ReleaseArtifacts(result);
    }
    heap_caps_free(host_command_queue_bytes_);
    heap_caps_free(remote_result_queue_bytes_);
    heap_caps_free(snapshot_);
    heap_caps_free(install_chunk_);
}

bool ControlDispatcher::valid() const {
    return install_chunk_ != nullptr && snapshot_ != nullptr && host_command_queue_ != nullptr &&
           remote_result_queue_ != nullptr;
}

bool ControlDispatcher::QueueCommand(const HostCommand& command, bool local) {
    const ControlSource expected_source = local ? ControlSource::kLocal : ControlSource::kRemote;
    if (host_command_queue_ == nullptr || command.source != expected_source ||
        xQueueSend(host_command_queue_, &command, 0U) != pdTRUE) {
        return false;
    }
    const CommandReadySink sink = command_ready_sink_.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(command_ready_context_.load(std::memory_order_acquire));
    }
    return true;
}

bool ControlDispatcher::QueueRemoteCommand(const HostCommand& command) { return QueueCommand(command, false); }

bool ControlDispatcher::QueueLocalCommand(const HostCommand& command) { return QueueCommand(command, true); }

bool ControlDispatcher::PeekHostCommand(HostCommand& command) const {
    return host_command_queue_ != nullptr && xQueuePeek(host_command_queue_, &command, 0U) == pdTRUE;
}

bool ControlDispatcher::PollHostCommand(HostCommand& command, TickType_t timeout) {
    return host_command_queue_ != nullptr && xQueueReceive(host_command_queue_, &command, timeout) == pdTRUE;
}

bool ControlDispatcher::SubmitHostResult(const HostResult& result) {
    if (result.source == ControlSource::kLocal) {
        const LocalResultSink sink = local_result_sink_.load(std::memory_order_acquire);
        void* context = local_result_context_.load(std::memory_order_acquire);
        if (sink != nullptr && context != nullptr && sink(context, result)) {
            return true;
        }
        ReleaseArtifacts(result);
        return false;
    }
    if (remote_result_queue_ == nullptr || xQueueSend(remote_result_queue_, &result, 0U) != pdTRUE) {
        ReleaseArtifacts(result);
        return false;
    }
    const ResultReadySink sink = remote_result_ready_sink_.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(remote_result_ready_context_.load(std::memory_order_acquire));
    }
    return true;
}

bool ControlDispatcher::PollRemoteResult(HostResult& result) {
    return remote_result_queue_ != nullptr && xQueueReceive(remote_result_queue_, &result, 0U) == pdTRUE;
}

void ControlDispatcher::UpdateInstalledApps(const CatalogSnapshot& catalog) {
    {
        std::lock_guard lock(snapshot_mutex_);
        snapshot_->catalog = catalog;
        snapshot_->catalog.count =
            std::min(snapshot_->catalog.count, static_cast<uint32_t>(snapshot_->catalog.apps.size()));
    }
    const CatalogSink sink = catalog_sink_.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(catalog_context_.load(std::memory_order_acquire), catalog);
    }
}

void ControlDispatcher::UpdateAppLifecycle(const char* app_id, const char* lifecycle) {
    {
        std::lock_guard lock(snapshot_mutex_);
        std::snprintf(snapshot_->active_app_id.data(), snapshot_->active_app_id.size(), "%s",
                      app_id != nullptr ? app_id : "");
        std::snprintf(snapshot_->lifecycle.data(), snapshot_->lifecycle.size(), "%s",
                      lifecycle != nullptr ? lifecycle : "not_running");
    }
    if (guest_log_lifecycle_sink_ != nullptr) {
        guest_log_lifecycle_sink_(guest_log_lifecycle_context_, app_id);
    }
    const LifecycleSink sink = lifecycle_sink_.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(lifecycle_context_.load(std::memory_order_acquire), app_id, lifecycle);
    }
}

void ControlDispatcher::UpdateLastAppDiagnostic(const AppDiagnostic& diagnostic) {
    std::lock_guard lock(snapshot_mutex_);
    snapshot_->last_app_diagnostic = diagnostic;
    snapshot_->has_last_app_diagnostic = true;
}

void ControlDispatcher::ResetStoreUpdates() {
    std::lock_guard lock(snapshot_mutex_);
    store_update_count_ = 0U;
}
void ControlDispatcher::AddStoreUpdate(const char* app_id, const char* version, const char* state,
                                       const std::array<uint8_t, 32U>& digest, const char* current_version) {
    std::lock_guard lock(snapshot_mutex_);
    if (app_id == nullptr || version == nullptr || store_update_count_ >= store_updates_.size()) return;
    if (store_update_request_state_ == host::StoreUpdateRequestState::kQueued &&
        std::strcmp(store_update_requested_.data(), app_id) == 0 && state != nullptr &&
        (std::strcmp(state, "failed") == 0 || std::strcmp(state, "available") == 0)) {
        store_update_request_state_ = host::StoreUpdateRequestState::kFailed;
    }
    auto& update = store_updates_[store_update_count_++];
    update.baseline_sha256 = digest;
    std::snprintf(update.current_version.data(), update.current_version.size(), "%s",
                  current_version ? current_version : "");
    std::snprintf(update.app_id.data(), update.app_id.size(), "%s", app_id);
    std::snprintf(update.version.data(), update.version.size(), "%s", version);
    std::snprintf(update.state.data(), update.state.size(), "%s", state != nullptr ? state : "available");
}
StoreAppUpdate ControlDispatcher::FindStoreUpdate(const char* app_id) const {
    std::lock_guard lock(snapshot_mutex_);
    for (uint32_t i = 0; i < store_update_count_; ++i)
        if (app_id != nullptr && std::strcmp(store_updates_[i].app_id.data(), app_id) == 0) return store_updates_[i];
    return {};
}
void ControlDispatcher::RequestStoreUpdate(const char* app_id) {
    std::lock_guard lock(snapshot_mutex_);
    if (app_id == nullptr || app_id[0] == '\0' || std::strlen(app_id) >= store_update_requested_.size() ||
        host::StoreUpdateRequestBusy(store_update_request_state_)) {
        return;
    }
    std::snprintf(store_update_requested_.data(), store_update_requested_.size(), "%s", app_id);
    store_update_request_state_ = host::StoreUpdateRequestState::kRequesting;
    store_update_dispatch_pending_ = true;
}
bool ControlDispatcher::ConsumeStoreUpdate(std::array<char, kAppIdCapacity>& app_id) {
    std::lock_guard lock(snapshot_mutex_);
    if (!store_update_dispatch_pending_) return false;
    app_id = store_update_requested_;
    store_update_dispatch_pending_ = false;
    return true;
}

void ControlDispatcher::CompleteStoreUpdateRequest(bool queued) {
    std::lock_guard lock(snapshot_mutex_);
    if (store_update_request_state_ == host::StoreUpdateRequestState::kRequesting && !store_update_dispatch_pending_) {
        store_update_request_state_ =
            queued ? host::StoreUpdateRequestState::kQueued : host::StoreUpdateRequestState::kFailed;
    }
}

void ControlDispatcher::RejectStoreUpdateRequest(const char* app_id) {
    std::lock_guard lock(snapshot_mutex_);
    if (app_id != nullptr && std::strcmp(store_update_requested_.data(), app_id) == 0 &&
        store_update_request_state_ == host::StoreUpdateRequestState::kQueued) {
        store_update_request_state_ = host::StoreUpdateRequestState::kFailed;
    }
}

host::StoreUpdateRequestState ControlDispatcher::StoreUpdateRequestState() const {
    std::lock_guard lock(snapshot_mutex_);
    return store_update_request_state_;
}

void ControlDispatcher::UpdateStoreSnapshot(const StoreSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    store_snapshot_ = snapshot;
}

StoreSnapshot ControlDispatcher::CopyStoreSnapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return store_snapshot_;
}

void ControlDispatcher::CopySnapshot(HostSnapshot& snapshot) const {
    std::lock_guard lock(snapshot_mutex_);
    snapshot = *snapshot_;
}

bool ControlDispatcher::BeginInstallActivity(ControlSource source, const char* command_id, const char* app_id,
                                             size_t package_size, const std::array<uint8_t, 32U>& sha256) {
    if (command_id == nullptr || command_id[0] == '\0' || app_id == nullptr || app_id[0] == '\0') {
        return false;
    }
    {
        std::lock_guard lock(snapshot_mutex_);
        if (install_activity_.active || install_chunk_pending_) {
            return false;
        }
        const uint32_t generation = install_activity_.generation + 1U;
        install_activity_ = {};
        std::snprintf(install_activity_.command_id.data(), install_activity_.command_id.size(), "%s", command_id);
        std::snprintf(install_activity_.app_id.data(), install_activity_.app_id.size(), "%s", app_id);
        if (++next_install_token_ == 0U) ++next_install_token_;
        install_activity_.install_token = next_install_token_;
        install_activity_.source = source;
        install_activity_.generation = generation;
        install_activity_.active = true;
        install_activity_.package_size = package_size;
        install_activity_.package_sha256 = sha256;
        install_activity_.preflight = package_size == 0U ? InstallPreflight::kNone : InstallPreflight::kPending;
    }
    const CommandReadySink sink = command_ready_sink_.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(command_ready_context_.load(std::memory_order_acquire));
    }
    return true;
}

bool ControlDispatcher::WriteInstallChunk(uint32_t token, size_t offset, std::span<const uint8_t> bytes) {
    if (!QueueInstallChunk(token, offset, bytes)) return false;
    const auto deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000U);
    InstallActivity activity{};
    for (;;) {
        CopyInstallActivity(activity);
        if (!activity.active || activity.install_token != token || activity.preflight != InstallPreflight::kReady)
            return false;
        if (activity.received_bytes == offset + bytes.size()) return true;
        if (static_cast<int32_t>(xTaskGetTickCount() - deadline) >= 0) return false;
        vTaskDelay(1U);
    }
}

bool ControlDispatcher::QueueInstallChunk(uint32_t token, size_t offset, std::span<const uint8_t> bytes) {
    {
        std::lock_guard lock(snapshot_mutex_);
        if (!install_chunk_ || install_chunk_pending_ || !install_activity_.active ||
            install_activity_.install_token != token || install_activity_.preflight != InstallPreflight::kReady ||
            offset != install_activity_.received_bytes || bytes.empty() || bytes.size() > kInstallChunkBytes ||
            offset > install_activity_.package_size || bytes.size() > install_activity_.package_size - offset)
            return false;
        std::memcpy(install_chunk_, bytes.data(), bytes.size());
        install_chunk_token_ = token;
        install_chunk_offset_ = offset;
        install_chunk_size_ = bytes.size();
        install_chunk_pending_ = true;
    }
    const auto sink = command_ready_sink_.load(std::memory_order_acquire);
    if (sink) sink(command_ready_context_.load(std::memory_order_acquire));
    return true;
}

bool ControlDispatcher::PollInstallChunk(uint32_t& token, size_t& offset, std::span<const uint8_t>& bytes) {
    std::lock_guard lock(snapshot_mutex_);
    if (!install_chunk_pending_) return false;
    token = install_chunk_token_;
    offset = install_chunk_offset_;
    bytes = {install_chunk_, install_chunk_size_};
    return true;
}

void ControlDispatcher::CompleteInstallChunk(uint32_t token, size_t received, const char* error) {
    std::lock_guard lock(snapshot_mutex_);
    if (!install_chunk_pending_ || token != install_chunk_token_) return;
    install_chunk_pending_ = false;
    if (!install_activity_.active || install_activity_.install_token != token) return;
    if (error) {
        std::snprintf(install_activity_.error.data(), install_activity_.error.size(), "%s", error);
        install_activity_.preflight = InstallPreflight::kFailed;
    } else {
        install_activity_.received_bytes = received;
    }
}

void ControlDispatcher::UpdateInstallProgress(ControlSource source, const char* command_id, uint8_t progress_percent) {
    bool changed = false;
    {
        std::lock_guard lock(snapshot_mutex_);
        if (install_activity_.active && install_activity_.source == source && command_id != nullptr &&
            std::strcmp(install_activity_.command_id.data(), command_id) == 0) {
            const uint8_t clamped = std::min<uint8_t>(progress_percent, 100U);
            if (install_activity_.progress_percent != clamped) {
                install_activity_.progress_percent = clamped;
                ++install_activity_.generation;
                changed = true;
            }
        }
    }
    const CommandReadySink sink = command_ready_sink_.load(std::memory_order_acquire);
    if (changed && sink != nullptr) {
        sink(command_ready_context_.load(std::memory_order_acquire));
    }
}

void ControlDispatcher::CompleteInstallPreflight(ControlSource source, const char* command_id, uint64_t required_bytes,
                                                 uint64_t free_bytes, const char* error) {
    std::lock_guard lock(snapshot_mutex_);
    if (!install_activity_.active || install_activity_.source != source || command_id == nullptr ||
        std::strcmp(install_activity_.command_id.data(), command_id) != 0 ||
        install_activity_.preflight != InstallPreflight::kPending)
        return;
    install_activity_.required_bytes = required_bytes;
    install_activity_.free_bytes = free_bytes;
    std::snprintf(install_activity_.error.data(), install_activity_.error.size(), "%s", error != nullptr ? error : "");
    install_activity_.preflight = error != nullptr ? InstallPreflight::kFailed : InstallPreflight::kReady;
    ++install_activity_.generation;
}

void ControlDispatcher::DismissInstallFailure() {
    std::lock_guard lock(snapshot_mutex_);
    if (!install_activity_.active && install_activity_.error[0] != '\0') {
        const uint32_t generation = install_activity_.generation + 1U;
        install_activity_ = {};
        install_activity_.generation = generation;
    }
}

void ControlDispatcher::EndInstallActivity(ControlSource source, const char* command_id, const char* error) {
    bool changed = false;
    {
        std::lock_guard lock(snapshot_mutex_);
        if (install_activity_.active && install_activity_.source == source && command_id != nullptr &&
            std::strcmp(install_activity_.command_id.data(), command_id) == 0) {
            if (source == ControlSource::kRemote &&
                std::strcmp(install_activity_.app_id.data(), store_update_requested_.data()) == 0) {
                // The request reached the installer. Its outcome belongs to
                // InstallActivity's dialog, not the action sheet's request error.
                store_update_request_state_ = host::StoreUpdateRequestState::kIdle;
                store_update_requested_[0] = '\0';
                store_update_dispatch_pending_ = false;
            }
            const uint32_t generation = install_activity_.generation + 1U;
            if (error == nullptr) {
                install_activity_ = {};
            } else {
                install_activity_.active = false;
                install_activity_.preflight = InstallPreflight::kFailed;
                std::snprintf(install_activity_.error.data(), install_activity_.error.size(), "%s", error);
            }
            install_activity_.generation = generation;
            changed = true;
        }
    }
    const CommandReadySink sink = command_ready_sink_.load(std::memory_order_acquire);
    if (changed && sink != nullptr) {
        sink(command_ready_context_.load(std::memory_order_acquire));
    }
}

void ControlDispatcher::CopyInstallActivity(InstallActivity& activity) const {
    std::lock_guard lock(snapshot_mutex_);
    activity = install_activity_;
}

void ControlDispatcher::SetCommandReadySink(CommandReadySink sink, void* context) {
    SetSink(command_ready_sink_, command_ready_context_, sink, context);
}

void ControlDispatcher::SetRemoteResultReadySink(ResultReadySink sink, void* context) {
    SetSink(remote_result_ready_sink_, remote_result_ready_context_, sink, context);
}

void ControlDispatcher::SetLocalResultSink(LocalResultSink sink, void* context) {
    SetSink(local_result_sink_, local_result_context_, sink, context);
}

void ControlDispatcher::SetCatalogSink(CatalogSink sink, void* context) {
    SetSink(catalog_sink_, catalog_context_, sink, context);
}

void ControlDispatcher::SetLifecycleSink(LifecycleSink sink, void* context) {
    SetSink(lifecycle_sink_, lifecycle_context_, sink, context);
}

void ReleaseHostCommand(const HostCommand& command) { heap_caps_free(command.package_data); }

void ReleaseArtifacts(const HostResult& result) {
    const uint32_t count = std::min(result.artifact_count, static_cast<uint32_t>(result.artifacts.size()));
    for (uint32_t index = 0U; index < count; ++index) {
        const Artifact& artifact = result.artifacts[index];
        if (artifact.data != nullptr && artifact.release != nullptr) {
            artifact.release(artifact.data);
        }
    }
}

}  // namespace micropixel::firmware::control
