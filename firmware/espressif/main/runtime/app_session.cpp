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

AppSessionFailure MakeFailure(AppSessionError code) { return AppSessionFailure{.code = code}; }

AppSessionFailure MakeFailure(AppSessionError code, const micropixel_aot_package_t& package) {
    AppSessionFailure failure{.code = code};
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
                       std::unique_ptr<GuestContext> context, std::unique_ptr<GuestContextBinding> context_binding,
                       std::unique_ptr<DecodedBitmap> launch_bitmap, bool launch_visible)
    : devices_(devices),
      package_(std::move(package)),
      module_(std::move(module)),
      guest_(std::move(guest)),
      context_(std::move(context)),
      context_binding_(std::move(context_binding)),
      launch_bitmap_(std::move(launch_bitmap)),
      launch_visible_(launch_visible) {}

AppSession::AppSession(AppSession&& other) noexcept
    : devices_(other.devices_),
      package_(std::move(other.package_)),
      module_(std::move(other.module_)),
      guest_(std::move(other.guest_)),
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
                                                                const bundlefs_file_t& file, GuestLogSink* log_sink) {
    auto package_result = AotPackage::Load(file);
    if (!package_result) {
        ESP_LOGE(kTag, "unable to read the configured AOT package");
        return std::unexpected(MakeFailure(AppSessionError::kPackageLoad));
    }
    AotPackage package = std::move(*package_result);
    micropixel_log_heap_state("AOT package loaded");

    auto& graphics = devices.graphics();
    std::unique_ptr<DecodedBitmap> launch_bitmap;
    auto launch_result = ShowLaunchBitmap(package, graphics, launch_bitmap);
    if (!launch_result) {
        return std::unexpected(MakeFailure(launch_result.error(), package.raw()));
    }
    const bool launch_visible = *launch_result;
    // The LVGL image directly references the Bundle mmap. Delete it before
    // AotPackage unmaps on every construction failure. A completed session
    // transfers the same duty to AppSession's destructor.
    LaunchBitmapGuard launch_guard{launch_visible ? &graphics : nullptr};

    auto module_result = LoadedModule::Load(package);
    if (!module_result) {
        ESP_LOGE(kTag, "AOT load failed: %s", module_result.error().message.data());
        return std::unexpected(MakeFailure(AppSessionError::kModuleLoad, package.raw()));
    }
    LoadedModule module = std::move(*module_result);
    micropixel_log_heap_state("AOT module loaded");

    auto guest_result = GuestInstance::Instantiate(module.get());
    if (!guest_result) {
        ESP_LOGE(kTag, "AOT instantiate failed: %s", guest_result.error().message.data());
        return std::unexpected(MakeFailure(AppSessionError::kGuestInstantiation, package.raw()));
    }
    GuestInstance guest = std::move(*guest_result);
    auto exec_env_result = guest.CreateExecEnv();
    if (!exec_env_result) {
        ESP_LOGE(kTag, "%s", exec_env_result.error().message.data());
        return std::unexpected(MakeFailure(AppSessionError::kExecEnvironment, package.raw()));
    }
    micropixel_log_heap_state("Guest instantiated");

    auto context = std::unique_ptr<GuestContext>(new (std::nothrow) GuestContext(package.raw(), devices, log_sink));
    if (context == nullptr || !context->valid()) {
        ESP_LOGE(kTag, "unable to initialize bounded Guest services");
        return std::unexpected(MakeFailure(AppSessionError::kGuestServices, package.raw()));
    }
    auto context_binding =
        std::unique_ptr<GuestContextBinding>(new (std::nothrow) GuestContextBinding(guest.get(), context.get()));
    if (context_binding == nullptr) {
        ESP_LOGE(kTag, "unable to bind Guest services to the WAMR instance");
        return std::unexpected(MakeFailure(AppSessionError::kGuestServices, package.raw()));
    }

    AppSession session(devices, std::move(package), std::move(module), std::move(guest), std::move(context),
                       std::move(context_binding), std::move(launch_bitmap), launch_visible);
    launch_guard.Release();
    return std::expected<AppSession, AppSessionFailure>{std::move(session)};
}

std::expected<void, AppSessionError> AppSession::Run() {
    constexpr char kEntryName[] = "__micropixel_start";
    if (stop_requested_.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "Guest entry skipped by Host stop request");
        return {};
    }
    wasm_function_inst_t entry = wasm_runtime_lookup_function(guest_.get(), kEntryName);
    if (entry == nullptr) {
        ESP_LOGE(kTag, "AOT module does not export __micropixel_start");
        return std::unexpected(AppSessionError::kMissingEntry);
    }

    if (context_ == nullptr || context_binding_ == nullptr) {
        return std::unexpected(AppSessionError::kGuestServices);
    }
    conformance::GuestTestHooks test_hooks(guest_.get(), guest_.exec_env(), *context_);
    if (!test_hooks.ready()) {
        return std::unexpected(AppSessionError::kConformanceTestHook);
    }

    uint32_t argv[1]{};
    bool timed_out = false;
    ESP_LOGI(kTag, "invoking Guest entry %s()", kEntryName);
    const int64_t entry_started_us = esp_timer_get_time();
    bool call_succeeded = micropixel_call_with_watchdog(guest_.get(), guest_.exec_env(), entry, 0, argv,
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
        return std::unexpected(AppSessionError::kWatchdogTimeout);
    }
    if (!call_succeeded) {
        const char* exception = wasm_runtime_get_exception(guest_.get());
        ESP_LOGE(kTag, "guest trapped: %s", exception != nullptr ? exception : "unknown trap");
        ESP_LOGE(kTag, "Guest call stack follows");
        wasm_runtime_dump_call_stack(guest_.exec_env());
        return std::unexpected(AppSessionError::kGuestTrap);
    }

    const int32_t exit_code = static_cast<int32_t>(argv[0]);
    if (exit_code != 0) {
        ESP_LOGE(kTag, "Guest main returned failure=%" PRId32, exit_code);
        return std::unexpected(AppSessionError::kGuestExit);
    }
    ESP_LOGI(kTag, "Guest main returned successfully");
    ESP_LOGI(kTag, "WAMR memory profile after Guest entry");
    wasm_runtime_dump_mem_consumption(guest_.exec_env());
    return {};
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
