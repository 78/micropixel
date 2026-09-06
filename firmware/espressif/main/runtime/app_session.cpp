#include "runtime/app_session.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "conformance/guest_test_hooks.hpp"
#include "device/device_services.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "runtime/guest_context.hpp"
#include "runtime/guest_log_sink.hpp"
#include "runtime/wamr/diagnostics.h"
#include "runtime/wamr/watchdog.h"
#include "sdkconfig.h"
#include "wasm_export.h"
#include "work/background_executor.hpp"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_session";

void CopyAppId(const micropixel_aot_package_t& package,
               std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U>& destination) {
    (void)std::snprintf(destination.data(), destination.size(), "%s", reinterpret_cast<const char*>(package.app_id));
}

AppSessionFailure MakeFailure(AppSessionError code, const char* detail = nullptr) {
    AppSessionFailure failure{.code = code};
    (void)std::snprintf(failure.detail.data(), failure.detail.size(), "%s", detail != nullptr ? detail : "");
    return failure;
}

AppSessionFailure MakeFailure(AppSessionError code, const micropixel_aot_package_t& package,
                              const char* detail = nullptr) {
    AppSessionFailure failure = MakeFailure(code, detail);
    CopyAppId(package, failure.app_id);
    return failure;
}

}  // namespace

AppSession::AppSession(device::DeviceServices& devices, AotPackage package, LoadedModule module, GuestInstance guest,
                       wasm_function_inst_t entry, std::unique_ptr<GuestContext> context,
                       std::unique_ptr<GuestContextBinding> context_binding)
    : devices_(devices),
      package_(std::move(package)),
      module_(std::move(module)),
      guest_(std::move(guest)),
      entry_(entry),
      context_(std::move(context)),
      context_binding_(std::move(context_binding)) {}

AppSession::AppSession(AppSession&& other) noexcept
    : devices_(other.devices_),
      package_(std::move(other.package_)),
      module_(std::move(other.module_)),
      guest_(std::move(other.guest_)),
      entry_(std::exchange(other.entry_, nullptr)),
      context_(std::move(other.context_)),
      context_binding_(std::move(other.context_binding_)),
      stop_requested_(other.stop_requested_.load(std::memory_order_acquire)) {}

AppSession::~AppSession() = default;

std::expected<AppSession, AppSessionFailure> AppSession::Create(
    device::DeviceServices& devices, work::BackgroundExecutor& background_executor, const bundlefs_file_t& file,
    std::string_view effective_locale, const micropixel_system_launch_arguments_response_t& launch_arguments,
    GuestLogSink* log_sink) {
    int64_t stage_started_us = esp_timer_get_time();
    ESP_LOGD(kTag, "AppSession stage begin: package");
    auto package_result = AotPackage::Load(file);
    if (!package_result) {
        ESP_LOGE(kTag, "unable to read the configured AOT package");
        return std::unexpected(MakeFailure(AppSessionError::kPackageLoad, "unable to read the configured AOT package"));
    }
    AotPackage package = std::move(*package_result);
    ESP_LOGI(kTag, "AppSession stage complete: package elapsed=%" PRId64 " us bytes=%" PRIu32,
             esp_timer_get_time() - stage_started_us, package.size());
    micropixel_log_heap_state("AOT package loaded");

    stage_started_us = esp_timer_get_time();
    ESP_LOGD(kTag, "AppSession stage begin: AOT module load");
    auto module_result = LoadedModule::Load(package);
    if (!module_result) {
        ESP_LOGE(kTag, "AOT load failed: %s", module_result.error().message.data());
        return std::unexpected(
            MakeFailure(AppSessionError::kModuleLoad, package.raw(), module_result.error().message.data()));
    }
    LoadedModule module = std::move(*module_result);
    ESP_LOGI(kTag, "AppSession stage complete: AOT module load elapsed=%" PRId64 " us",
             esp_timer_get_time() - stage_started_us);
    micropixel_log_heap_state("AOT module loaded");

    // GUEST_BUFFERS Direct Surfaces hand the Host raw pointers into the Guest's
    // linear memory, so those Bundles declare PINNED_MEMORY and get their whole
    // ceiling reserved up front. Everyone else starts small and grows, leaving
    // PSRAM for Host-side bitmap decoding.
    const bool pinned_memory = (package.raw().aot_flags & MICROPIXEL_BUNDLE_AOT_FLAG_PINNED_MEMORY) != 0U;
    auto guest_result = GuestInstance::Instantiate(module.get(), pinned_memory);
    if (!guest_result) {
        ESP_LOGE(kTag, "AOT instantiate failed: %s", guest_result.error().message.data());
        return std::unexpected(
            MakeFailure(AppSessionError::kGuestInstantiation, package.raw(), guest_result.error().message.data()));
    }
    GuestInstance guest = std::move(*guest_result);
    auto exec_env_result = guest.CreateExecEnv();
    if (!exec_env_result) {
        ESP_LOGE(kTag, "%s", exec_env_result.error().message.data());
        return std::unexpected(
            MakeFailure(AppSessionError::kExecEnvironment, package.raw(), exec_env_result.error().message.data()));
    }
    micropixel_log_heap_state("Guest instantiated");

    constexpr char kEntryName[] = "__micropixel_start";
    wasm_function_inst_t entry = wasm_runtime_lookup_function(guest.get(), kEntryName);
    if (entry == nullptr) {
        ESP_LOGE(kTag, "AOT module does not export %s", kEntryName);
        return std::unexpected(MakeFailure(AppSessionError::kMissingEntry, package.raw(),
                                           "AOT module does not export __micropixel_start"));
    }

    auto context = std::unique_ptr<GuestContext>(new (std::nothrow) GuestContext(
        package.raw(), devices, background_executor, effective_locale, launch_arguments, log_sink));
    if (context == nullptr || !context->valid()) {
        ESP_LOGE(kTag, "unable to initialize bounded Guest services");
        return std::unexpected(
            MakeFailure(AppSessionError::kGuestServices, package.raw(), "unable to initialize bounded Guest services"));
    }
    auto context_binding =
        std::unique_ptr<GuestContextBinding>(new (std::nothrow) GuestContextBinding(guest.get(), context.get()));
    if (context_binding == nullptr) {
        ESP_LOGE(kTag, "unable to bind Guest services to the WAMR instance");
        return std::unexpected(
            MakeFailure(AppSessionError::kGuestServices, package.raw(), "unable to bind Guest services to WAMR"));
    }
    // GUEST_BUFFERS Direct Surfaces present Guest buffers by linear-memory
    // offset; the Host resolves them against this instance. Only a
    // PINNED_MEMORY Bundle gets a base that never moves, so only then may a
    // resolved pointer outlive the call (stable_base below); other Bundles are
    // refused GUEST_BUFFERS (the default Host buffers need no pinning) instead
    // of being handed a pointer memory.grow can free.
    // The check must not go through wasm_runtime_validate_app_addr: that call
    // raises a Guest exception on failure, turning a refused request into a
    // trap instead of the documented INVALID_MEMORY status.
    context->BindGuestMemory({
        .context = guest.get(),
        .resolve =
            [](void* instance, uint32_t offset, uint32_t length, uint8_t** host_out) {
                auto module_instance = static_cast<wasm_module_inst_t>(instance);
                if (length == 0U) {
                    return false;
                }
                const wasm_memory_inst_t memory = wasm_runtime_get_memory(module_instance, 0U);
                if (memory == nullptr) {
                    return false;
                }
                const uint64_t linear_bytes = static_cast<uint64_t>(wasm_memory_get_cur_page_count(memory)) *
                                              wasm_memory_get_bytes_per_page(memory);
                if (offset >= linear_bytes || length > linear_bytes - offset) {
                    return false;
                }
                *host_out = static_cast<uint8_t*>(wasm_memory_get_base_address(memory)) + offset;
                return true;
            },
        .stable_base = guest.pinned_memory(),
    });

    AppSession session(devices, std::move(package), std::move(module), std::move(guest), entry, std::move(context),
                       std::move(context_binding));
    return std::expected<AppSession, AppSessionFailure>{std::move(session)};
}

std::expected<void, AppSessionFailure> AppSession::Run() {
    constexpr char kEntryName[] = "__micropixel_start";
    if (stop_requested_.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "Guest entry skipped by Host stop request");
        return {};
    }
    if (context_ == nullptr || context_binding_ == nullptr) {
        return std::unexpected(MakeFailure(AppSessionError::kGuestServices, package_.raw(),
                                           "Guest services are not bound to the WAMR instance"));
    }
    conformance::GuestTestHooks test_hooks(guest_.get(), guest_.exec_env(), *context_);
    if (!test_hooks.ready()) {
        return std::unexpected(MakeFailure(AppSessionError::kConformanceTestHook, package_.raw(),
                                           "unable to start the conformance event injector"));
    }

    uint32_t argv[1]{};
    bool timed_out = false;
    ESP_LOGI(kTag, "invoking Guest entry %s()", kEntryName);
    const int64_t entry_started_us = esp_timer_get_time();
    bool call_succeeded = micropixel_call_with_watchdog(guest_.get(), guest_.exec_env(), entry_, 0, argv,
                                                        CONFIG_WAMR_DEFAULT_WATCHDOG_TIMEOUT_MS, &timed_out);
    const int64_t entry_elapsed_us = esp_timer_get_time() - entry_started_us;
    ESP_LOGI(kTag, "Guest entry %s elapsed=%" PRId64 " us", kEntryName, entry_elapsed_us);

    if (stop_requested_.load(std::memory_order_acquire)) {
        wasm_runtime_clear_exception(guest_.get());
        ESP_LOGI(kTag, "Guest entry stopped by Host request");
        return {};
    }
    if (timed_out) {
        ESP_LOGE(kTag, "Guest entry %s exceeded its watchdog quota", kEntryName);
        return std::unexpected(
            MakeFailure(AppSessionError::kWatchdogTimeout, package_.raw(), "Guest entry exceeded its watchdog quota"));
    }
    if (!call_succeeded) {
        const char* exception = wasm_runtime_get_exception(guest_.get());
        if (exception == nullptr) {
            exception = "unknown WAMR trap";
        }
        // The SDK reports its own panics as a single "panic: ..." log line right
        // before __builtin_trap(); surface it next to the bare WAMR exception.
        const char* panic = context_ != nullptr ? context_->LastPanic() : "";
        std::array<char, 256U> detail{};
        if (panic[0] != '\0') {
            (void)std::snprintf(detail.data(), detail.size(), "%s; %s", panic, exception);
        } else {
            (void)std::snprintf(detail.data(), detail.size(), "%s", exception);
        }
        ESP_LOGE(kTag, "guest trapped: %s", detail.data());
        ESP_LOGE(kTag, "Guest call stack follows");
        wasm_runtime_dump_call_stack(guest_.exec_env());
        return std::unexpected(MakeFailure(AppSessionError::kGuestTrap, package_.raw(), detail.data()));
    }

    const int32_t exit_code = static_cast<int32_t>(argv[0]);
    if (exit_code != 0) {
        ESP_LOGE(kTag, "Guest main returned failure=%" PRId32, exit_code);
        AppSessionFailure failure =
            MakeFailure(AppSessionError::kGuestExit, package_.raw(), "Guest main returned a non-zero exit code");
        failure.exit_code = exit_code;
        failure.has_exit_code = true;
        return std::unexpected(failure);
    }
    ESP_LOGI(kTag, "Guest main returned successfully");
    ESP_LOGI(kTag, "WAMR memory profile after Guest entry");
    wasm_runtime_dump_mem_consumption(guest_.exec_env());
    return {};
}

const char* AppSessionErrorCode(AppSessionError error) {
    switch (error) {
        case AppSessionError::kPackageLoad:
            return "package_load_failed";
        case AppSessionError::kLaunchBitmap:
            return "launch_bitmap_failed";
        case AppSessionError::kModuleLoad:
            return "aot_load_failed";
        case AppSessionError::kGuestInstantiation:
            return "aot_instantiate_failed";
        case AppSessionError::kExecEnvironment:
            return "exec_environment_failed";
        case AppSessionError::kMissingEntry:
            return "entry_missing";
        case AppSessionError::kGuestServices:
            return "guest_services_failed";
        case AppSessionError::kConformanceTestHook:
            return "conformance_hook_failed";
        case AppSessionError::kWatchdogTimeout:
            return "watchdog_timeout";
        case AppSessionError::kGuestTrap:
            return "guest_trap";
        case AppSessionError::kGuestExit:
            return "guest_exit_nonzero";
        case AppSessionError::kSessionAlreadyActive:
            return "session_already_active";
        case AppSessionError::kRuntimeSynchronization:
        default:
            return "runtime_synchronization_failed";
    }
}

const char* AppSessionErrorPhase(AppSessionError error) {
    switch (error) {
        case AppSessionError::kPackageLoad:
        case AppSessionError::kLaunchBitmap:
            return "package";
        case AppSessionError::kModuleLoad:
            return "load";
        case AppSessionError::kGuestInstantiation:
        case AppSessionError::kExecEnvironment:
        case AppSessionError::kMissingEntry:
        case AppSessionError::kGuestServices:
        case AppSessionError::kConformanceTestHook:
            return "instantiate";
        case AppSessionError::kWatchdogTimeout:
        case AppSessionError::kGuestTrap:
        case AppSessionError::kGuestExit:
            return "run";
        case AppSessionError::kSessionAlreadyActive:
        case AppSessionError::kRuntimeSynchronization:
        default:
            return "host";
    }
}

bool AppSession::Suspend(TickType_t timeout) { return context_ != nullptr && context_->Suspend(timeout); }

bool AppSession::Resume() { return context_ != nullptr && context_->Resume(); }

void AppSession::RequestStop() {
    stop_requested_.store(true, std::memory_order_release);
    if (context_ == nullptr || !context_->RequestStop()) {
        ForceStop();
    }
}

void AppSession::ForceStop() {
    stop_requested_.store(true, std::memory_order_release);
    wasm_runtime_terminate(guest_.get());
    if (context_ != nullptr) {
        context_->ForceStop();
    }
}

const char* AppSession::app_id() const { return reinterpret_cast<const char*>(package_.raw().app_id); }

}  // namespace micropixel::runtime
