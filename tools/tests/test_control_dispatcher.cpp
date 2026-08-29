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
    return 0;
}
