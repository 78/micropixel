#include "runtime/wamr/wamr_runtime.hpp"

#include <cinttypes>
#include <cstdio>
#include <utility>

#include "bh_platform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr uint32_t kExecStackSize = 8U * 1024U;
constexpr uint32_t kGuestAppHeapSize = 8U * 1024U;
constexpr char kTag[] = "micropixel_wamr";
bool guest_memory_placement_logged;

WamrFailure MakeFailure(WamrError code, const char* message) {
    WamrFailure failure{.code = code};
    (void)std::snprintf(failure.message.data(), failure.message.size(), "%s", message);
    return failure;
}

wasm_module_inst_t InstantiateGuest(wasm_module_t module, char* error_buf, uint32_t error_buf_size) {
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, kExecStackSize, kGuestAppHeapSize, error_buf, error_buf_size);

    if (module_inst == nullptr) {
        return nullptr;
    }

    void* linear_base = wasm_runtime_addr_app_to_native(module_inst, 0);
    uint64_t linear_start = 0;
    uint64_t linear_end = 0;
    const bool linear_range_valid = wasm_runtime_get_app_addr_range(module_inst, 0, &linear_start, &linear_end);

    if (linear_base == nullptr || !linear_range_valid || !esp_ptr_external_ram(linear_base) ||
        esp_ptr_external_ram(module_inst)) {
        std::snprintf(error_buf, error_buf_size, "guest memory placement failed: linear=%p (%s), instance=%p (%s)",
                      linear_base, linear_base != nullptr && esp_ptr_external_ram(linear_base) ? "PSRAM" : "internal",
                      module_inst, esp_ptr_external_ram(module_inst) ? "PSRAM" : "internal");
        wasm_runtime_deinstantiate(module_inst);
        return nullptr;
    }

    if (!guest_memory_placement_logged) {
        const size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI(kTag, "guest linear memory/app heap: base=%p, size=%" PRIu64 ", region=PSRAM", linear_base,
                 linear_end - linear_start);
        ESP_LOGI(kTag, "WAMR instance metadata: %p, region=internal SRAM", module_inst);
        ESP_LOGI(kTag, "placement allocation delta: internal=%zu, PSRAM=%zu bytes", internal_before - internal_after,
                 psram_before - psram_after);
        guest_memory_placement_logged = true;
    }

    return module_inst;
}

}  // namespace

WamrRuntime::WamrRuntime(WamrRuntime&& other) noexcept : initialized_(std::exchange(other.initialized_, false)) {}

WamrRuntime& WamrRuntime::operator=(WamrRuntime&& other) noexcept {
    if (this != &other) {
        Reset();
        initialized_ = std::exchange(other.initialized_, false);
    }
    return *this;
}

WamrRuntime::~WamrRuntime() { Reset(); }

std::expected<WamrRuntime, WamrFailure> WamrRuntime::Initialize() {
    RuntimeInitArgs args{};
    args.mem_alloc_type = Alloc_With_Allocator;
    args.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void*>(os_malloc);
    args.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void*>(os_realloc);
    args.mem_alloc_option.allocator.free_func = reinterpret_cast<void*>(os_free);
    args.max_thread_num = CONFIG_WAMR_RUNTIME_MAX_GUEST_THREADS;

    WamrRuntime runtime;
    runtime.initialized_ = wasm_runtime_full_init(&args);
    if (!runtime.initialized_) {
        return std::unexpected(MakeFailure(WamrError::kInitialization, "wasm_runtime_full_init failed"));
    }
    return runtime;
}

void WamrRuntime::Reset() {
    if (initialized_) {
        wasm_runtime_destroy();
        initialized_ = false;
    }
}

LoadedModule::LoadedModule(LoadedModule&& other) noexcept : module_(std::exchange(other.module_, nullptr)) {}

LoadedModule& LoadedModule::operator=(LoadedModule&& other) noexcept {
    if (this != &other) {
        Reset();
        module_ = std::exchange(other.module_, nullptr);
    }
    return *this;
}

LoadedModule::~LoadedModule() { Reset(); }

std::expected<LoadedModule, WamrFailure> LoadedModule::Load(const AotPackage& package) {
    WamrFailure failure{.code = WamrError::kModuleLoad};
    LoadedModule module;
    module.module_ = wasm_runtime_load(package.data(), package.size(), failure.message.data(), failure.message.size());
    if (module.module_ == nullptr) {
        return std::unexpected(failure);
    }
    return module;
}

wasm_module_t LoadedModule::get() const { return module_; }

void LoadedModule::Reset() {
    if (module_ != nullptr) {
        wasm_runtime_unload(module_);
        module_ = nullptr;
        ESP_LOGI(kTag, "AOT module unloaded");
    }
}

GuestInstance::GuestInstance(GuestInstance&& other) noexcept
    : instance_(std::exchange(other.instance_, nullptr)), exec_env_(std::exchange(other.exec_env_, nullptr)) {}

GuestInstance& GuestInstance::operator=(GuestInstance&& other) noexcept {
    if (this != &other) {
        Reset();
        instance_ = std::exchange(other.instance_, nullptr);
        exec_env_ = std::exchange(other.exec_env_, nullptr);
    }
    return *this;
}

GuestInstance::~GuestInstance() { Reset(); }

std::expected<GuestInstance, WamrFailure> GuestInstance::Instantiate(wasm_module_t module) {
    WamrFailure failure{.code = WamrError::kGuestInstantiation};
    GuestInstance guest;
    guest.instance_ = InstantiateGuest(module, failure.message.data(), failure.message.size());
    if (guest.instance_ == nullptr) {
        return std::unexpected(failure);
    }
    return guest;
}

std::expected<void, WamrFailure> GuestInstance::CreateExecEnv() {
    if (instance_ == nullptr || exec_env_ != nullptr) {
        return std::unexpected(
            MakeFailure(WamrError::kExecEnvironmentCreation, "invalid Guest state for exec_env creation"));
    }
    exec_env_ = wasm_runtime_create_exec_env(instance_, kExecStackSize);
    if (exec_env_ == nullptr) {
        return std::unexpected(MakeFailure(WamrError::kExecEnvironmentCreation, "exec_env creation failed"));
    }
    return {};
}

wasm_module_inst_t GuestInstance::get() const { return instance_; }

wasm_exec_env_t GuestInstance::exec_env() const { return exec_env_; }

void GuestInstance::Reset() {
    if (exec_env_ != nullptr) {
        wasm_runtime_destroy_exec_env(exec_env_);
        exec_env_ = nullptr;
    }
    if (instance_ != nullptr) {
        wasm_runtime_deinstantiate(instance_);
        instance_ = nullptr;
        ESP_LOGI(kTag, "Guest instance deinstantiated");
    }
}

GuestContextBinding::GuestContextBinding(wasm_module_inst_t instance, void* context) : instance_(instance) {
    wasm_runtime_set_custom_data(instance_, context);
}

GuestContextBinding::~GuestContextBinding() { wasm_runtime_set_custom_data(instance_, nullptr); }

}  // namespace micropixel::runtime
