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
#include "runtime/memory/guest_psram.hpp"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr uint32_t kExecStackSize = 8U * 1024U;
constexpr uint32_t kGuestAppHeapSize = 0U;
constexpr uint32_t kWasmPageBytes = 64U * 1024U;
constexpr uint32_t kGuestLinearMemoryMaxPages = CONFIG_MICROPIXEL_GUEST_LINEAR_MEMORY_MAX_PAGES;
constexpr uint32_t kGuestLinearMemoryMinimumPages = 2U;
constexpr size_t kGuestLinearMemoryAllocationOverhead = 64U;
// Keep small, latency-sensitive WAMR metadata in internal SRAM.  Module-load
// buffers are larger and must not depend on finding a contiguous internal
// block after the Host UI and a previous Guest have fragmented that heap.
constexpr unsigned kPsramAllocationThreshold = 16U * 1024U;
constexpr uintptr_t kWamrAllocationAlignment = 8U;
constexpr uint32_t kAotTargetInfoSection = 0U;
constexpr uint32_t kAotTargetInfoSize = 48U;
constexpr uint32_t kAotFeatureMultiThread = 1U << 2U;
constexpr char kTag[] = "micropixel_wamr";
bool guest_memory_placement_logged;

uint32_t ReadLittleEndian32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8U |
           static_cast<uint32_t>(bytes[2]) << 16U | static_cast<uint32_t>(bytes[3]) << 24U;
}

uint64_t ReadLittleEndian64(const uint8_t* bytes) {
    return static_cast<uint64_t>(ReadLittleEndian32(bytes)) | static_cast<uint64_t>(ReadLittleEndian32(bytes + 4U))
                                                                  << 32U;
}

bool ReadAotFeatureFlags(const AotPackage& package, uint64_t& feature_flags_out) {
    // Locked WAMR AOT v6 starts with magic/version, followed by a 48-byte
    // target-info section. feature_flags is 16 bytes into that section body.
    constexpr uint32_t kFileHeaderSize = 8U;
    constexpr uint32_t kSectionHeaderSize = 8U;
    constexpr uint32_t kFeatureFlagsOffset = 16U;
    constexpr uint32_t kRequiredSize = kFileHeaderSize + kSectionHeaderSize + kFeatureFlagsOffset + sizeof(uint64_t);
    if (package.size() < kRequiredSize) {
        return false;
    }
    const uint8_t* bytes = package.data();
    if (ReadLittleEndian32(bytes + kFileHeaderSize) != kAotTargetInfoSection ||
        ReadLittleEndian32(bytes + kFileHeaderSize + sizeof(uint32_t)) != kAotTargetInfoSize) {
        return false;
    }
    feature_flags_out = ReadLittleEndian64(bytes + kFileHeaderSize + kSectionHeaderSize + kFeatureFlagsOffset);
    return true;
}

bool ValidateThreadingDeclaration(const AotPackage& package, char* error_buf, size_t error_buf_size) {
    const uint32_t bundle_flags = package.raw().aot_flags;
    if ((bundle_flags & MICROPIXEL_BUNDLE_AOT_FLAG_THREADING_DECLARED) == 0U) {
        ESP_LOGW(kTag, "legacy Bundle has no Guest threading declaration: app=%s", package.raw().app_id);
        return true;
    }

    uint64_t aot_features = 0U;
    if (!ReadAotFeatureFlags(package, aot_features)) {
        std::snprintf(error_buf, error_buf_size, "unable to read WAMR AOT target-info threading features");
        return false;
    }
    const bool declared_shared = (bundle_flags & MICROPIXEL_BUNDLE_AOT_FLAG_SHARED_MEMORY) != 0U;
    const bool aot_multi_thread = (aot_features & kAotFeatureMultiThread) != 0U;
    if (declared_shared != aot_multi_thread) {
        std::snprintf(error_buf, error_buf_size,
                      "Bundle threading declaration disagrees with AOT features: declared=%s aot=%s",
                      declared_shared ? "shared-memory" : "none", aot_multi_thread ? "multi-thread" : "none");
        return false;
    }
#if !CONFIG_WAMR_ENABLE_LIB_PTHREAD || !CONFIG_WAMR_ENABLE_SHARED_MEMORY
    if (declared_shared) {
        std::snprintf(error_buf, error_buf_size,
                      "Bundle requires shared-memory threading but this Host does not include WAMR pthread support");
        return false;
    }
#endif
    ESP_LOGI(kTag, "Guest threading policy: app=%s mode=%s", package.raw().app_id,
             declared_shared ? "shared-memory" : "none");
    return true;
}

constexpr uint32_t EffectiveGuestLinearMemoryPages(size_t largest_psram_block) {
    constexpr size_t kReservedBytes =
        CONFIG_MICROPIXEL_GUEST_PSRAM_RESERVE_BYTES + kGuestLinearMemoryAllocationOverhead;
    const uint32_t available_pages =
        largest_psram_block > kReservedBytes
            ? static_cast<uint32_t>((largest_psram_block - kReservedBytes) / kWasmPageBytes)
            : 0U;
    return std::min(kGuestLinearMemoryMaxPages, available_pages);
}

static_assert(EffectiveGuestLinearMemoryPages(CONFIG_MICROPIXEL_GUEST_PSRAM_RESERVE_BYTES) == 0U);
static_assert(EffectiveGuestLinearMemoryPages(CONFIG_MICROPIXEL_GUEST_PSRAM_RESERVE_BYTES +
                                              kGuestLinearMemoryAllocationOverhead + kWasmPageBytes) == 1U);
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
    void* origin = size >= kPsramAllocationThreshold ? AllocateGuestPsram(static_cast<size_t>(size) + kOverhead)
                                                     : heap_caps_malloc(static_cast<size_t>(size) + kOverhead, caps);
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
    constexpr size_t kOverhead = sizeof(WamrAllocationHeader) + kWamrAllocationAlignment - 1U;
    if (size > std::numeric_limits<size_t>::max() - kOverhead) {
        LogAllocationFailure("realloc", size);
        return nullptr;
    }
    const bool old_psram = old_size >= kPsramAllocationThreshold;
    const bool new_psram = size >= kPsramAllocationThreshold;
    const size_t old_allocation_bytes = static_cast<size_t>(old_size) + kOverhead;
    const size_t new_allocation_bytes = static_cast<size_t>(size) + kOverhead;
    const size_t psram_growth = new_psram ? (old_psram && new_allocation_bytes > old_allocation_bytes
                                                 ? new_allocation_bytes - old_allocation_bytes
                                                 : (old_psram ? 0U : new_allocation_bytes))
                                          : 0U;
    if (new_psram && !CanGrowGuestPsram(psram_growth)) {
        LogAllocationFailure("realloc", size);
        return nullptr;
    }

    void* old_origin = old_header->origin;
    const size_t old_payload_offset = reinterpret_cast<uintptr_t>(memory) - reinterpret_cast<uintptr_t>(old_origin);
    const uint32_t caps = new_psram ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    void* resized_origin = heap_caps_realloc(old_origin, new_allocation_bytes, caps);
    if (resized_origin == nullptr) {
        LogAllocationFailure("realloc", size);
        return nullptr;
    }
    const uintptr_t resized_aligned =
        (reinterpret_cast<uintptr_t>(resized_origin) + sizeof(WamrAllocationHeader) + kWamrAllocationAlignment - 1U) &
        ~(kWamrAllocationAlignment - 1U);
    auto* resized = reinterpret_cast<void*>(resized_aligned);
    auto* preserved_payload = static_cast<uint8_t*>(resized_origin) + old_payload_offset;
    if (resized != preserved_payload) {
        std::memmove(resized, preserved_payload, std::min(old_size, size));
    }
    auto* resized_header = reinterpret_cast<WamrAllocationHeader*>(resized) - 1;
    resized_header->origin = resized_origin;
    resized_header->size = size;
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
                      GuestPsramReserveBytes());
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
    if (!ValidateThreadingDeclaration(package, failure.message.data(), failure.message.size())) {
        return std::unexpected(failure);
    }
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
