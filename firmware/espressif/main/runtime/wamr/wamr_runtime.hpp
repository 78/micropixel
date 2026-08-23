#ifndef MICROPIXEL_RUNTIME_WAMR_WAMR_RUNTIME_HPP
#define MICROPIXEL_RUNTIME_WAMR_WAMR_RUNTIME_HPP

#include <array>
#include <cstdint>
#include <expected>

#include "runtime/bundle/aot_package.hpp"
#include "wasm_export.h"

namespace micropixel::runtime {

enum class WamrError {
    kInitialization,
    kModuleLoad,
    kGuestInstantiation,
    kExecEnvironmentCreation,
};

struct WamrFailure final {
    WamrError code{};
    std::array<char, 256> message{};
};

// Owns WAMR's process-wide initialization for one Firmware runtime session.
class WamrRuntime final {
   public:
    WamrRuntime(const WamrRuntime&) = delete;
    WamrRuntime& operator=(const WamrRuntime&) = delete;
    WamrRuntime(WamrRuntime&& other) noexcept;
    WamrRuntime& operator=(WamrRuntime&& other) noexcept;
    ~WamrRuntime();

    [[nodiscard]] static std::expected<WamrRuntime, WamrFailure> Initialize();

   private:
    WamrRuntime() = default;
    void Reset();

    bool initialized_{};
};

class LoadedModule final {
   public:
    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;
    LoadedModule(LoadedModule&& other) noexcept;
    LoadedModule& operator=(LoadedModule&& other) noexcept;
    ~LoadedModule();

    [[nodiscard]] static std::expected<LoadedModule, WamrFailure> Load(const AotPackage& package);
    [[nodiscard]] wasm_module_t get() const;  // NOLINT(readability-identifier-naming)

   private:
    LoadedModule() = default;
    void Reset();

    wasm_module_t module_{};
};

class GuestInstance final {
   public:
    GuestInstance(const GuestInstance&) = delete;
    GuestInstance& operator=(const GuestInstance&) = delete;
    GuestInstance(GuestInstance&& other) noexcept;
    GuestInstance& operator=(GuestInstance&& other) noexcept;
    ~GuestInstance();

    [[nodiscard]] static std::expected<GuestInstance, WamrFailure> Instantiate(wasm_module_t module);
    [[nodiscard]] std::expected<void, WamrFailure> CreateExecEnv();
    [[nodiscard]] wasm_module_inst_t get() const;    // NOLINT(readability-identifier-naming)
    [[nodiscard]] wasm_exec_env_t exec_env() const;  // NOLINT(readability-identifier-naming)

   private:
    GuestInstance() = default;
    void Reset();

    wasm_module_inst_t instance_{};
    wasm_exec_env_t exec_env_{};
};

class GuestContextBinding final {
   public:
    GuestContextBinding(wasm_module_inst_t instance, void* context);
    GuestContextBinding(const GuestContextBinding&) = delete;
    GuestContextBinding& operator=(const GuestContextBinding&) = delete;
    ~GuestContextBinding();

   private:
    wasm_module_inst_t instance_{};
};

}  // namespace micropixel::runtime

#endif
