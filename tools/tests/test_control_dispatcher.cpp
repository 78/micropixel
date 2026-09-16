#include <cassert>
#include <cstring>

#include "host/controller/control_dispatcher.hpp"

namespace {

using micropixel::firmware::control::CatalogSnapshot;
using micropixel::firmware::control::ControlDispatcher;
using micropixel::firmware::control::ControlSource;
using micropixel::firmware::control::HostCommand;
using micropixel::firmware::control::HostResult;
using micropixel::firmware::control::HostSnapshot;
using micropixel::firmware::control::InstallActivity;
using micropixel::host::StoreUpdateRequestState;

struct Sinks final {
    uint32_t command_ready{};
    uint32_t remote_result_ready{};
    uint32_t catalog_changed{};
    uint32_t lifecycle_changed{};
    uint32_t local_results{};
};

void CommandReady(void* context) { ++static_cast<Sinks*>(context)->command_ready; }
void RemoteResultReady(void* context) { ++static_cast<Sinks*>(context)->remote_result_ready; }
void CatalogChanged(void* context, const CatalogSnapshot&) { ++static_cast<Sinks*>(context)->catalog_changed; }
void LifecycleChanged(void* context, const char*, const char*) { ++static_cast<Sinks*>(context)->lifecycle_changed; }
bool LocalResult(void* context, const HostResult&) {
    ++static_cast<Sinks*>(context)->local_results;
    return true;
}

}  // namespace

int main() {
    ControlDispatcher controls;
    assert(controls.valid());
    controls.AddStoreUpdate("app", "0.2.0", "available", {});
    assert(std::strcmp(controls.FindStoreUpdate("app").version.data(), "0.2.0") == 0);
    assert(controls.FindStoreUpdate("sideload").version[0] == '\0');
    controls.RequestStoreUpdate(nullptr);
    controls.RequestStoreUpdate("");
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kIdle);
    controls.RequestStoreUpdate("app");
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kRequesting);
    controls.RequestStoreUpdate("other");
    std::array<char, micropixel::firmware::control::kAppIdCapacity> update_app{};
    assert(controls.ConsumeStoreUpdate(update_app));
    assert(std::strcmp(update_app.data(), "app") == 0);
    assert(!controls.ConsumeStoreUpdate(update_app));
    // Consuming the request must not let another click enqueue a duplicate.
    controls.RequestStoreUpdate("app");
    assert(!controls.ConsumeStoreUpdate(update_app));
    const auto revision = controls.StoreUpdatesRevision();
    controls.AddStoreUpdate("fonts.zh-cn", "1.1.0", "available", {}, "1.0.0");
    assert(controls.StoreUpdatesRevision() == revision);
    controls.SetStoreCheckState(2U);
    assert(controls.StoreUpdatesRevision() == revision + 1U);
    const auto font_update = controls.FindStoreUpdate("fonts.zh-cn");
    assert(std::strcmp(font_update.current_version.data(), "1.0.0") == 0);
    assert(std::strcmp(font_update.version.data(), "1.1.0") == 0);
    controls.SetStoreCheckState(3U);
    assert(controls.StoreUpdatesRevision() == revision + 1U);
    assert(controls.FindStoreUpdate("fonts.zh-cn").version == font_update.version);
    controls.RequestStoreCheck();
    controls.SetStoreCheckState(3U);
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kRequesting);
    controls.CompleteStoreUpdateRequest(false);
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kFailed);
    controls.SetStoreCheckState(2U);
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kFailed);
    controls.RequestStoreUpdate("app");
    assert(controls.ConsumeStoreUpdate(update_app));
    controls.CompleteStoreUpdateRequest(true);
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kQueued);
    controls.RequestStoreUpdate("other");
    assert(!controls.ConsumeStoreUpdate(update_app));
    assert(controls.BeginInstallActivity(ControlSource::kRemote, "install-other", "other"));
    controls.EndInstallActivity(ControlSource::kRemote, "install-other");
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kQueued);
    assert(controls.BeginInstallActivity(ControlSource::kRemote, "install-app", "app"));
    controls.EndInstallActivity(ControlSource::kRemote, "install-app");
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kIdle);
    controls.RequestStoreUpdate("app");
    assert(controls.ConsumeStoreUpdate(update_app));
    controls.CompleteStoreUpdateRequest(true);
    controls.RejectStoreUpdateRequest("other");
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kQueued);
    controls.RejectStoreUpdateRequest("app");
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kFailed);
    controls.RequestStoreUpdate("app");
    assert(controls.ConsumeStoreUpdate(update_app));
    controls.CompleteStoreUpdateRequest(true);
    controls.AddStoreUpdate("app", "0.2.0", "failed", {});
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kFailed);
    controls.ResetStoreUpdates();
    assert(controls.FindStoreUpdate("app").version[0] == '\0');

    Sinks sinks{};
    controls.SetCommandReadySink(CommandReady, &sinks);
    controls.SetRemoteResultReadySink(RemoteResultReady, &sinks);
    controls.SetLocalResultSink(LocalResult, &sinks);
    controls.SetCatalogSink(CatalogChanged, &sinks);
    controls.SetLifecycleSink(LifecycleChanged, &sinks);

    CatalogSnapshot catalog{};
    catalog.count = 1U;
    std::strcpy(catalog.apps[0].app_id.data(), "micropixel.test");
    controls.UpdateInstalledApps(catalog);
    controls.UpdateAppLifecycle("micropixel.test", "foreground");
    assert(sinks.catalog_changed == 1U);
    assert(sinks.lifecycle_changed == 1U);

    HostSnapshot snapshot{};
    controls.CopySnapshot(snapshot);
    assert(snapshot.catalog.count == 1U);
    assert(std::strcmp(snapshot.active_app_id.data(), "micropixel.test") == 0);
    assert(std::strcmp(snapshot.lifecycle.data(), "foreground") == 0);

    micropixel::firmware::control::AppDiagnostic diagnostic{};
    std::strcpy(diagnostic.app_id.data(), "micropixel.test");
    std::strcpy(diagnostic.phase.data(), "run");
    std::strcpy(diagnostic.code.data(), "guest_trap");
    std::strcpy(diagnostic.detail.data(), "out of bounds memory access");
    controls.UpdateLastAppDiagnostic(diagnostic);
    controls.CopySnapshot(snapshot);
    assert(snapshot.has_last_app_diagnostic);
    assert(std::strcmp(snapshot.last_app_diagnostic.code.data(), "guest_trap") == 0);

    assert(controls.BeginInstallActivity(ControlSource::kLocal, "usb:6", "micropixel.installing"));
    assert(!controls.BeginInstallActivity(ControlSource::kRemote, "remote-6", "micropixel.other"));
    InstallActivity install{};
    controls.CopyInstallActivity(install);
    assert(install.active);
    assert(std::strcmp(install.app_id.data(), "micropixel.installing") == 0);
    assert(install.progress_percent == 0U);
    const uint32_t begin_generation = install.generation;
    controls.UpdateInstallProgress(ControlSource::kLocal, "usb:6", 42U);
    controls.CopyInstallActivity(install);
    assert(install.progress_percent == 42U);
    assert(install.generation > begin_generation);
    controls.UpdateInstallProgress(ControlSource::kRemote, "usb:6", 80U);
    controls.CopyInstallActivity(install);
    assert(install.progress_percent == 42U);
    controls.UpdateInstallProgress(ControlSource::kLocal, "usb:6", 255U);
    controls.CopyInstallActivity(install);
    assert(install.progress_percent == 100U);
    controls.EndInstallActivity(ControlSource::kLocal, "usb:6");
    controls.CopyInstallActivity(install);
    assert(!install.active);
    assert(controls.BeginInstallActivity(ControlSource::kRemote, "remote-7", "micropixel.next"));
    controls.EndInstallActivity(ControlSource::kRemote, "remote-7");

    HostCommand local{};
    std::strcpy(local.command_id.data(), "usb:7");
    local.source = ControlSource::kLocal;
    HostCommand remote{};
    std::strcpy(remote.command_id.data(), "remote-8");
    assert(controls.QueueLocalCommand(local));
    assert(controls.QueueRemoteCommand(remote));
    assert(!controls.QueueRemoteCommand(local));
    assert(!controls.QueueLocalCommand(remote));
    assert(sinks.command_ready == 8U);

    HostCommand received{};
    assert(controls.PeekHostCommand(received));
    assert(std::strcmp(received.command_id.data(), "usb:7") == 0);
    assert(controls.PollHostCommand(received));
    assert(controls.PollHostCommand(received));
    assert(std::strcmp(received.command_id.data(), "remote-8") == 0);

    HostResult local_result{};
    std::strcpy(local_result.command_id.data(), "usb:7");
    local_result.source = ControlSource::kLocal;
    assert(controls.SubmitHostResult(local_result));
    assert(sinks.local_results == 1U);

    HostResult remote_result{};
    std::strcpy(remote_result.command_id.data(), "remote-8");
    assert(controls.SubmitHostResult(remote_result));
    assert(sinks.remote_result_ready == 1U);
    HostResult delivered{};
    assert(controls.PollRemoteResult(delivered));
    assert(std::strcmp(delivered.command_id.data(), "remote-8") == 0);
    // Preflight is owned by one installation; stale completions cannot unblock a retry.
    using micropixel::firmware::control::InstallPreflight;
    std::array<uint8_t, 32U> digest{};
    digest[0] = 42U;
    controls.RequestStoreUpdate("app");
    assert(controls.ConsumeStoreUpdate(update_app));
    controls.CompleteStoreUpdateRequest(true);
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kQueued);
    assert(controls.BeginInstallActivity(ControlSource::kRemote, "check-1", "app", 65536U, digest));
    controls.CopyInstallActivity(install);
    assert(install.preflight == InstallPreflight::kPending && install.package_size == 65536U);
    assert(install.package_sha256 == digest);
    controls.CompleteInstallPreflight(ControlSource::kLocal, "check-1", 131072U, 65536U, "app_store_full");
    controls.CompleteInstallPreflight(ControlSource::kRemote, "stale", 131072U, 65536U, "app_store_full");
    controls.CopyInstallActivity(install);
    assert(install.preflight == InstallPreflight::kPending);
    controls.CompleteInstallPreflight(ControlSource::kRemote, "check-1", 131072U, 65536U, "app_store_full");
    controls.CopyInstallActivity(install);
    assert(install.active && install.preflight == InstallPreflight::kFailed);
    controls.EndInstallActivity(ControlSource::kRemote, "check-1", "app_store_full");
    // The failure is shown only by the dialog; reopening the upgrade sheet
    // must not describe this completed installation as a failed request.
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kIdle);
    controls.CopyInstallActivity(install);
    assert(!install.active && install.required_bytes - install.free_bytes == 65536U);
    assert(std::strcmp(install.error.data(), "app_store_full") == 0);
    controls.DismissInstallFailure();
    controls.CopyInstallActivity(install);
    assert(!install.active && install.error[0] == '\0');
    assert(controls.StoreUpdateRequestState() == StoreUpdateRequestState::kIdle);
    assert(controls.BeginInstallActivity(ControlSource::kRemote, "check-2", "app", 65536U, digest));
    controls.CompleteInstallPreflight(ControlSource::kRemote, "check-1", 0U, 0U, "app_store_full");
    controls.CopyInstallActivity(install);
    assert(install.preflight == InstallPreflight::kPending && install.error[0] == '\0');
    controls.CompleteInstallPreflight(ControlSource::kRemote, "check-2", 131072U, 131072U, nullptr);
    controls.CopyInstallActivity(install);
    assert(install.preflight == InstallPreflight::kReady && install.active);
    controls.DismissInstallFailure();
    controls.CopyInstallActivity(install);
    assert(install.active);
    controls.EndInstallActivity(ControlSource::kRemote, "check-2");
    controls.CopyInstallActivity(install);
    assert(!install.active && install.error[0] == '\0');
    assert(controls.BeginInstallActivity(ControlSource::kLocal, "stream", "app", 65536U, digest));
    controls.CopyInstallActivity(install);
    const auto token = install.install_token;
    std::array<uint8_t, 16U> chunk{};
    assert(!controls.QueueInstallChunk(token, 0U, chunk));
    controls.CompleteInstallPreflight(ControlSource::kLocal, "stream", 69632U, 131072U, nullptr);
    assert(!controls.QueueInstallChunk(token + 1U, 0U, chunk));
    assert(!controls.QueueInstallChunk(token, 1U, chunk));
    assert(controls.QueueInstallChunk(token, 0U, chunk));
    assert(!controls.QueueInstallChunk(token, 0U, chunk));
    uint32_t chunk_token{};
    size_t offset{};
    std::span<const uint8_t> bytes;
    assert(controls.PollInstallChunk(chunk_token, offset, bytes) && bytes.size() == chunk.size());
    controls.EndInstallActivity(ControlSource::kLocal, "stream");
    assert(!controls.BeginInstallActivity(ControlSource::kLocal, "stream", "app", 65536U, digest));
    controls.CompleteInstallChunk(token, chunk.size(), "install_cancelled");
    assert(controls.BeginInstallActivity(ControlSource::kLocal, "stream", "app", 65536U, digest));
    controls.CopyInstallActivity(install);
    assert(install.install_token != token);
    controls.CompleteInstallPreflight(ControlSource::kLocal, "stream", 69632U, 131072U, nullptr);
    assert(!controls.QueueInstallChunk(token, 0U, chunk));
    assert(controls.QueueInstallChunk(install.install_token, 0U, chunk));
    controls.CompleteInstallChunk(token, chunk.size(), nullptr);
    assert(controls.PollInstallChunk(chunk_token, offset, bytes));
    controls.CompleteInstallChunk(install.install_token, chunk.size(), nullptr);
    controls.CopyInstallActivity(install);
    assert(install.received_bytes == chunk.size());
    assert(!controls.QueueInstallChunk(install.install_token, 0U, chunk));
    assert(controls.QueueInstallChunk(install.install_token, chunk.size(), chunk));
    controls.CompleteInstallChunk(install.install_token, chunk.size() * 2, "app_store_write_failed");
    controls.CopyInstallActivity(install);
    assert(install.preflight == InstallPreflight::kFailed && install.received_bytes == chunk.size());
    controls.EndInstallActivity(ControlSource::kLocal, "stream");
    return 0;
}
