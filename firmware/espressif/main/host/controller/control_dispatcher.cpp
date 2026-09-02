#include "host/controller/control_dispatcher.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"

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
}

bool ControlDispatcher::valid() const {
    return snapshot_ != nullptr && host_command_queue_ != nullptr && remote_result_queue_ != nullptr;
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

void ControlDispatcher::CopySnapshot(HostSnapshot& snapshot) const {
    std::lock_guard lock(snapshot_mutex_);
    snapshot = *snapshot_;
}

bool ControlDispatcher::BeginInstallActivity(ControlSource source, const char* command_id, const char* app_id) {
    if (command_id == nullptr || command_id[0] == '\0' || app_id == nullptr || app_id[0] == '\0') {
        return false;
    }
    {
        std::lock_guard lock(snapshot_mutex_);
        if (install_activity_.active) {
            return false;
        }
        const uint32_t generation = install_activity_.generation + 1U;
        install_activity_ = {};
        std::snprintf(install_activity_.command_id.data(), install_activity_.command_id.size(), "%s", command_id);
        std::snprintf(install_activity_.app_id.data(), install_activity_.app_id.size(), "%s", app_id);
        install_activity_.source = source;
        install_activity_.generation = generation;
        install_activity_.active = true;
    }
    const CommandReadySink sink = command_ready_sink_.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(command_ready_context_.load(std::memory_order_acquire));
    }
    return true;
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

void ControlDispatcher::EndInstallActivity(ControlSource source, const char* command_id) {
    bool changed = false;
    {
        std::lock_guard lock(snapshot_mutex_);
        if (install_activity_.active && install_activity_.source == source && command_id != nullptr &&
            std::strcmp(install_activity_.command_id.data(), command_id) == 0) {
            const uint32_t generation = install_activity_.generation + 1U;
            install_activity_ = {};
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
