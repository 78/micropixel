#include "runtime/wamr/wamr_runtime.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include "bh_platform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr uint32_t kExecStackSize = 8U * 1024U;
constexpr uint32_t kGuestAppHeapSize = 0U;
constexpr uint32_t kWasmPageBytes = 64U * 1024U;
constexpr uint32_t kGuestLinearMemoryMaxPages = CONFIG_MICROPIXEL_GUEST_LINEAR_MEMORY_MAX_PAGES;
constexpr uint32_t kGuestLinearMemoryMinimumPages = 2U;
constexpr size_t kGuestLinearMemoryPsramReserve = 256U * 1024U;
constexpr size_t kGuestLinearMemoryAllocationOverhead = 64U;
// Keep small, latency-sensitive WAMR metadata in internal SRAM.  Module-load
// buffers are larger and must not depend on finding a contiguous internal
// block after the Host UI and a previous Guest have fragmented that heap.
constexpr unsigned kPsramAllocationThreshold = 16U * 1024U;
constexpr uintptr_t kWamrAllocationAlignment = 8U;
constexpr char kTag[] = "micropixel_wamr";
bool guest_memory_placement_logged;

constexpr uint32_t EffectiveGuestLinearMemoryPages(size_t largest_psram_block) {
    constexpr size_t kReservedBytes = kGuestLinearMemoryPsramReserve + kGuestLinearMemoryAllocationOverhead;
    const uint32_t available_pages =
        largest_psram_block > kReservedBytes
            ? static_cast<uint32_t>((largest_psram_block - kReservedBytes) / kWasmPageBytes)
            : 0U;
    return std::min(kGuestLinearMemoryMaxPages, available_pages);
}

static_assert(EffectiveGuestLinearMemoryPages(2U * 1024U * 1024U) == 27U);
static_assert(EffectiveGuestLinearMemoryPages(32U * 1024U * 1024U) == kGuestLinearMemoryMaxPages);

struct WamrAllocationHeader {
    void* origin;
    unsigned size;
};

void LogAllocationFailure(const char* operation, unsigned size) {
    ESP_LOGE(kTag,
             "WAMR %s failed: requested=%u internal-free=%zu internal-largest=%zu "
             "psram-free=%zu psram-largest=%zu",
             operation, size, heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void WamrFree(void* memory);

void* WamrMalloc(unsigned size) {
    constexpr size_t kOverhead = sizeof(WamrAllocationHeader) + kWamrAllocationAlignment - 1U;
    if (size > std::numeric_limits<size_t>::max() - kOverhead) {
        LogAllocationFailure("malloc", size);
        return nullptr;
    }

    const uint32_t caps =
        size >= kPsramAllocationThreshold ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    void* origin = heap_caps_malloc(static_cast<size_t>(size) + kOverhead, caps);
    if (origin == nullptr) {
        LogAllocationFailure("malloc", size);
        return nullptr;
    }

    const uintptr_t aligned =
        (reinterpret_cast<uintptr_t>(origin) + sizeof(WamrAllocationHeader) + kWamrAllocationAlignment - 1U) &
        ~(kWamrAllocationAlignment - 1U);
    auto* header = reinterpret_cast<WamrAllocationHeader*>(aligned) - 1;
    header->origin = origin;
    header->size = size;

    if (size >= kPsramAllocationThreshold) {
        ESP_LOGI(kTag, "WAMR large allocation: requested=%u ptr=%p region=PSRAM", size,
                 reinterpret_cast<void*>(aligned));
    }
    return reinterpret_cast<void*>(aligned);
}

void* WamrRealloc(void* memory, unsigned size) {
    if (memory == nullptr) {
        return WamrMalloc(size);
    }
    if (size == 0U) {
        WamrFree(memory);
        return nullptr;
    }

    const auto* old_header = reinterpret_cast<const WamrAllocationHeader*>(memory) - 1;
    const unsigned old_size = old_header->size;
    void* resized = WamrMalloc(size);
    if (resized == nullptr) {
        return nullptr;
    }
    std::memcpy(resized, memory, std::min(old_size, size));
    WamrFree(memory);
    return resized;
}

void WamrFree(void* memory) {
    if (memory != nullptr) {
        const auto* header = reinterpret_cast<const WamrAllocationHeader*>(memory) - 1;
        heap_caps_free(header->origin);
    }
}

WamrFailure MakeFailure(WamrError code, const char* message) {
    WamrFailure failure{.code = code};
    (void)std::snprintf(failure.message.data(), failure.message.size(), "%s", message);
    return failure;
}

wasm_module_inst_t InstantiateGuest(wasm_module_t module, char* error_buf, uint32_t error_buf_size) {
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const uint32_t effective_max_pages = EffectiveGuestLinearMemoryPages(psram_largest);
    if (effective_max_pages < kGuestLinearMemoryMinimumPages) {
        std::snprintf(error_buf, error_buf_size,
                      "insufficient contiguous PSRAM for Guest linear memory: largest=%zu reserve=%zu", psram_largest,
                      kGuestLinearMemoryPsramReserve);
        return nullptr;
    }
    const uint64_t effective_max_bytes = static_cast<uint64_t>(effective_max_pages) * kWasmPageBytes;
    const InstantiationArgs arguments{
        .default_stack_size = kExecStackSize,
        .host_managed_heap_size = kGuestAppHeapSize,
        .max_memory_pages = effective_max_pages,
    };
    wasm_module_inst_t module_inst = wasm_runtime_instantiate_ex(module, &arguments, error_buf, error_buf_size);

    if (module_inst == nullptr) {
        return nullptr;
    }

    void* linear_base = wasm_runtime_addr_app_to_native(module_inst, 0);
    uint64_t linear_start = 0;
    uint64_t linear_end = 0;
    const bool linear_range_valid = wasm_runtime_get_app_addr_range(module_inst, 0, &linear_start, &linear_end);

    const bool linear_range_ordered = linear_range_valid && linear_end >= linear_start;
    const uint64_t linear_size = linear_range_ordered ? linear_end - linear_start : 0U;
    if (linear_base == nullptr || !linear_range_ordered || linear_size > effective_max_bytes ||
        !esp_ptr_external_ram(linear_base) || esp_ptr_external_ram(module_inst)) {
        std::snprintf(error_buf, error_buf_size,
                      "guest memory policy failed: linear=%p size=%" PRIu64 "/%" PRIu64 " (%s), instance=%p (%s)",
                      linear_base, linear_size, effective_max_bytes,
                      linear_base != nullptr && esp_ptr_external_ram(linear_base) ? "PSRAM" : "internal", module_inst,
                      esp_ptr_external_ram(module_inst) ? "PSRAM" : "internal");
        wasm_runtime_deinstantiate(module_inst);
        return nullptr;
    }

    if (!guest_memory_placement_logged) {
        const size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI(kTag,
                 "guest linear memory: base=%p, initial=%" PRIu64 ", host-max=%" PRIu64 " (%" PRIu32
                 " pages), largest-before=%zu, region=PSRAM",
                 linear_base, linear_size, effective_max_bytes, effective_max_pages, psram_largest);
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
    args.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void*>(WamrMalloc);
    args.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void*>(WamrRealloc);
    args.mem_alloc_option.allocator.free_func = reinterpret_cast<void*>(WamrFree);
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
