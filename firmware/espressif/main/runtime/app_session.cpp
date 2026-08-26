#include "runtime/app_session.hpp"

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
#include "runtime/resources/bitmap_decoder.hpp"
#include "runtime/wamr/diagnostics.h"
#include "runtime/wamr/watchdog.h"
#include "sdkconfig.h"
#include "wasm_export.h"

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

std::expected<bool, AppSessionError> ShowLaunchBitmap(const AotPackage& package, device::GraphicsService& graphics,
                                                      std::unique_ptr<DecodedBitmap>& decoded_out) {
    if (!graphics.Available() || package.raw().launch_asset_id == 0U) {
        return false;
    }
    micropixel_bundle_asset_view_t asset{};
    if (!micropixel_bundle_find_asset(&package.raw(), package.raw().launch_asset_id, &asset)) {
        ESP_LOGE(kTag, "launch asset is missing from its Bundle");
        return std::unexpected(AppSessionError::kLaunchBitmap);
    }
    device::BitmapView bitmap{};
    if (asset.format == MICROPIXEL_BUNDLE_FORMAT_RAW_BGR888) {
        bitmap = {asset.data, asset.size, asset.width, asset.height, asset.stride, MICROPIXEL_PIXEL_FORMAT_BGR888};
    } else if (asset.format == MICROPIXEL_BUNDLE_FORMAT_PNG) {
        decoded_out = std::make_unique<DecodedBitmap>();
        if (decoded_out == nullptr || !DecodeBitmap(asset, *decoded_out)) {
            // A high-resolution Hall cover must never prevent its App from
            // launching merely because the optional splash could not fit.
            ESP_LOGW(kTag, "PNG launch splash skipped; Hall cover remains usable");
            decoded_out.reset();
            return false;
        }
        bitmap = decoded_out->view();
    } else {
        return std::unexpected(AppSessionError::kLaunchBitmap);
    }
    if (!graphics.ShowLaunchBitmap(bitmap)) {
        ESP_LOGE(kTag, "unable to display the Bundle launch bitmap");
        return std::unexpected(AppSessionError::kLaunchBitmap);
    }
    return true;
}

class LaunchBitmapGuard final {
   public:
    explicit LaunchBitmapGuard(device::GraphicsService* graphics) : graphics_(graphics) {}
    LaunchBitmapGuard(const LaunchBitmapGuard&) = delete;
    LaunchBitmapGuard& operator=(const LaunchBitmapGuard&) = delete;
    ~LaunchBitmapGuard() {
        if (graphics_ != nullptr) {
            graphics_->DismissLaunchBitmap();
        }
    }

    void Release() { graphics_ = nullptr; }

   private:
    device::GraphicsService* graphics_{};
};

}  // namespace

AppSession::AppSession(device::DeviceServices& devices, AotPackage package, LoadedModule module, GuestInstance guest,
                       wasm_function_inst_t entry, std::unique_ptr<GuestContext> context,
                       std::unique_ptr<GuestContextBinding> context_binding,
                       std::unique_ptr<DecodedBitmap> launch_bitmap, bool launch_visible)
    : devices_(devices),
      package_(std::move(package)),
      module_(std::move(module)),
      guest_(std::move(guest)),
      entry_(entry),
      context_(std::move(context)),
      context_binding_(std::move(context_binding)),
      launch_bitmap_(std::move(launch_bitmap)),
      launch_visible_(launch_visible) {}

AppSession::AppSession(AppSession&& other) noexcept
    : devices_(other.devices_),
      package_(std::move(other.package_)),
      module_(std::move(other.module_)),
      guest_(std::move(other.guest_)),
      entry_(std::exchange(other.entry_, nullptr)),
      context_(std::move(other.context_)),
      context_binding_(std::move(other.context_binding_)),
      launch_bitmap_(std::move(other.launch_bitmap_)),
      stop_requested_(other.stop_requested_.load(std::memory_order_acquire)),
      launch_visible_(std::exchange(other.launch_visible_, false)) {}

AppSession::~AppSession() {
    if (launch_visible_) {
        devices_.graphics().DismissLaunchBitmap();
    }
}

std::expected<AppSession, AppSessionFailure> AppSession::Create(device::DeviceServices& devices,
                                                                const bundlefs_file_t& file,
                                                                std::string_view effective_locale,
                                                                GuestLogSink* log_sink) {
    auto package_result = AotPackage::Load(file);
    if (!package_result) {
        ESP_LOGE(kTag, "unable to read the configured AOT package");
        return std::unexpected(MakeFailure(AppSessionError::kPackageLoad, "unable to read the configured AOT package"));
    }
    AotPackage package = std::move(*package_result);
    micropixel_log_heap_state("AOT package loaded");

    auto& graphics = devices.graphics();
    std::unique_ptr<DecodedBitmap> launch_bitmap;
    auto launch_result = ShowLaunchBitmap(package, graphics, launch_bitmap);
    if (!launch_result) {
        return std::unexpected(
            MakeFailure(launch_result.error(), package.raw(), "unable to display the launch bitmap"));
    }
    const bool launch_visible = *launch_result;
    // The LVGL image directly references the Bundle mmap. Delete it before
    // AotPackage unmaps on every construction failure. A completed session
    // transfers the same duty to AppSession's destructor.
    LaunchBitmapGuard launch_guard{launch_visible ? &graphics : nullptr};

    auto module_result = LoadedModule::Load(package);
    if (!module_result) {
        ESP_LOGE(kTag, "AOT load failed: %s", module_result.error().message.data());
        return std::unexpected(
            MakeFailure(AppSessionError::kModuleLoad, package.raw(), module_result.error().message.data()));
    }
    LoadedModule module = std::move(*module_result);
    micropixel_log_heap_state("AOT module loaded");

    auto guest_result = GuestInstance::Instantiate(module.get());
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

    auto context = std::unique_ptr<GuestContext>(new (std::nothrow)
                                                     GuestContext(package.raw(), devices, effective_locale, log_sink));
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

    AppSession session(devices, std::move(package), std::move(module), std::move(guest), entry, std::move(context),
                       std::move(context_binding), std::move(launch_bitmap), launch_visible);
    launch_guard.Release();
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
        ESP_LOGE(kTag, "guest trapped: %s", exception != nullptr ? exception : "unknown trap");
        ESP_LOGE(kTag, "Guest call stack follows");
        wasm_runtime_dump_call_stack(guest_.exec_env());
        return std::unexpected(MakeFailure(AppSessionError::kGuestTrap, package_.raw(),
                                           exception != nullptr ? exception : "unknown WAMR trap"));
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
