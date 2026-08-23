#include "runtime/engine.hpp"

#include <cinttypes>
#include <utility>

#include "conformance/guest_test_hooks.hpp"
#include "device/device_services.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "runtime/abi/abi_bridge.h"
#include "runtime/bundle/aot_package.hpp"
#include "runtime/guest_context.hpp"
#include "runtime/wamr/diagnostics.h"
#include "runtime/wamr/wamr_runtime.hpp"
#include "runtime/wamr/watchdog.h"
#include "sdkconfig.h"
#include "wasm_export.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_engine";

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

   private:
    device::GraphicsService* graphics_{};
};

std::expected<void, EngineError> ShowLaunchBitmap(const AotPackage& package, device::GraphicsService& graphics) {
    if (!graphics.Available() || package.raw().launch_asset_id == 0U) {
        return {};
    }
    micropixel_bundle_asset_view_t asset{};
    if (!micropixel_bundle_find_asset(&package.raw(), package.raw().launch_asset_id, &asset) ||
        asset.format != MICROPIXEL_BUNDLE_FORMAT_RAW_RGB888) {
        ESP_LOGE(kTag, "launch asset must be an opaque local raw RGB888 Bitmap");
        return std::unexpected(EngineError::kLaunchBitmap);
    }
    device::BitmapView bitmap{asset.data,   asset.size,   asset.width,
                              asset.height, asset.stride, MICROPIXEL_PIXEL_FORMAT_RGB888};
    if (!graphics.ShowLaunchBitmap(bitmap)) {
        ESP_LOGE(kTag, "unable to display the Bundle launch bitmap");
        return std::unexpected(EngineError::kLaunchBitmap);
    }
    return {};
}

std::expected<void, EngineError> RunApplication(GuestInstance& guest, const micropixel_aot_package_t& package,
                                                device::DeviceServices& devices) {
    auto exec_env_result = guest.CreateExecEnv();
    if (!exec_env_result) {
        ESP_LOGE(kTag, "%s", exec_env_result.error().message.data());
        return std::unexpected(EngineError::kExecEnvironment);
    }

    constexpr char kEntryName[] = "__micropixel_start";
    wasm_function_inst_t entry = wasm_runtime_lookup_function(guest.get(), kEntryName);
    if (entry == nullptr) {
        ESP_LOGE(kTag, "AOT module does not export __micropixel_start");
        return std::unexpected(EngineError::kMissingEntry);
    }

    GuestContext context{package, devices};
    if (!context.valid()) {
        ESP_LOGE(kTag, "unable to initialize bounded Guest services");
        return std::unexpected(EngineError::kGuestServices);
    }
    GuestContextBinding context_binding(guest.get(), &context);
    conformance::GuestTestHooks test_hooks(guest.get(), guest.exec_env(), context);
    if (!test_hooks.ready()) {
        return std::unexpected(EngineError::kConformanceTestHook);
    }

    uint32_t argv[1]{};
    bool timed_out = false;
    ESP_LOGI(kTag, "invoking Guest entry %s()", kEntryName);
    const int64_t entry_started_us = esp_timer_get_time();
    bool call_succeeded = micropixel_call_with_watchdog(guest.get(), guest.exec_env(), entry, 0, argv,
                                                        CONFIG_WAMR_DEFAULT_WATCHDOG_TIMEOUT_MS, &timed_out);
    const int64_t entry_elapsed_us = esp_timer_get_time() - entry_started_us;
    ESP_LOGI(kTag, "Guest entry %s elapsed=%" PRId64 " us", kEntryName, entry_elapsed_us);

    if (timed_out) {
        ESP_LOGE(kTag, "Guest entry %s exceeded its watchdog quota", kEntryName);
        return std::unexpected(EngineError::kWatchdogTimeout);
    }
    if (!call_succeeded) {
        const char* exception = wasm_runtime_get_exception(guest.get());
        ESP_LOGE(kTag, "guest trapped: %s", exception != nullptr ? exception : "unknown trap");
        ESP_LOGE(kTag, "Guest call stack follows");
        wasm_runtime_dump_call_stack(guest.exec_env());
        return std::unexpected(EngineError::kGuestTrap);
    }

    int32_t exit_code = static_cast<int32_t>(argv[0]);
    if (exit_code != 0) {
        ESP_LOGE(kTag, "Guest main returned failure=%" PRId32, exit_code);
        return std::unexpected(EngineError::kGuestExit);
    }
    ESP_LOGI(kTag, "Guest main returned successfully");
    ESP_LOGI(kTag, "WAMR memory profile after Guest entry");
    wasm_runtime_dump_mem_consumption(guest.exec_env());
    return {};
}

std::expected<void, EngineError> RunSession(device::DeviceServices& devices) {
    auto package_result = AotPackage::Load();
    if (!package_result) {
        ESP_LOGE(kTag, "unable to read the configured AOT package");
        return std::unexpected(EngineError::kPackageLoad);
    }
    AotPackage package = std::move(*package_result);
    micropixel_log_heap_state("AOT package loaded");

    auto& graphics = devices.graphics();
    const bool launch_visible = graphics.Available() && package.raw().launch_asset_id != 0U;
    auto launch_result = ShowLaunchBitmap(package, graphics);
    if (!launch_result) {
        return launch_result;
    }
    // The LVGL image directly references the Bundle mmap. Always delete it
    // before AotPackage unmaps, including WAMR/AOT failure paths. The first
    // successful Guest frame dismisses it early; this guard is then a no-op.
    LaunchBitmapGuard launch_guard{launch_visible ? &graphics : nullptr};

    auto runtime_result = WamrRuntime::Initialize();
    if (!runtime_result) {
        ESP_LOGE(kTag, "%s", runtime_result.error().message.data());
        return std::unexpected(EngineError::kRuntimeInitialization);
    }
    WamrRuntime runtime = std::move(*runtime_result);
    micropixel_log_heap_state("WAMR initialized");
    ESP_LOGI(kTag, "guest quotas: max child threads=%d, watchdog=%d ms", CONFIG_WAMR_RUNTIME_MAX_GUEST_THREADS,
             CONFIG_WAMR_DEFAULT_WATCHDOG_TIMEOUT_MS);

    if (!micropixel_register_native_apis()) {
        ESP_LOGE(kTag, "failed to register Host native APIs");
        return std::unexpected(EngineError::kNativeApiRegistration);
    }
    auto module_result = LoadedModule::Load(package);
    if (!module_result) {
        ESP_LOGE(kTag, "AOT load failed: %s", module_result.error().message.data());
        return std::unexpected(EngineError::kModuleLoad);
    }
    LoadedModule module = std::move(*module_result);
    micropixel_log_heap_state("AOT module loaded");

    auto guest_result = GuestInstance::Instantiate(module.get());
    if (!guest_result) {
        ESP_LOGE(kTag, "AOT instantiate failed: %s", guest_result.error().message.data());
        return std::unexpected(EngineError::kGuestInstantiation);
    }
    GuestInstance guest = std::move(*guest_result);
    micropixel_log_heap_state("initial Guest instantiated");
    return RunApplication(guest, package.raw(), devices);
}

}  // namespace

std::expected<void, EngineError> Engine::Run() {
    micropixel_log_heap_state("host task entry");
    auto result = RunSession(devices_);
    micropixel_log_heap_state("host cleanup complete");
    micropixel_log_stack_profiles();
    ESP_LOGI(kTag, "AOT host task finished: %s", result ? "success" : "failure");
    return result;
}

}  // namespace micropixel::runtime
