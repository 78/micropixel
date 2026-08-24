#ifndef MICROPIXEL_CONFORMANCE_GUEST_TEST_HOOKS_HPP
#define MICROPIXEL_CONFORMANCE_GUEST_TEST_HOOKS_HPP

#include <pthread.h>

#include "runtime/guest_context.hpp"
#include "sdkconfig.h"
#include "wasm_export.h"

namespace micropixel::conformance {

#if CONFIG_MICROPIXEL_ENABLE_CONFORMANCE_TEST_HOOKS

class GuestTestHooks final {
   public:
    GuestTestHooks(wasm_module_inst_t instance, wasm_exec_env_t exec_env, runtime::GuestContext& context);
    GuestTestHooks(const GuestTestHooks&) = delete;
    GuestTestHooks& operator=(const GuestTestHooks&) = delete;
    ~GuestTestHooks();

    [[nodiscard]] bool ready() const {  // NOLINT(readability-identifier-naming)
        return ready_;
    }

   private:
    static void* InjectEventBurst(void* argument);

    enum class Kind {
        kHostWake,
        kTouchPressure,
        kRunHandlerStop,
    };

    runtime::GuestContext* context_{};
    Kind kind_{Kind::kHostWake};
    pthread_t thread_{};
    bool started_{};
    bool ready_{true};
};

#else

class GuestTestHooks final {
   public:
    GuestTestHooks(wasm_module_inst_t, wasm_exec_env_t, runtime::GuestContext&) {}
    [[nodiscard]] constexpr bool ready() const {  // NOLINT(readability-identifier-naming)
        return true;
    }
};

#endif

}  // namespace micropixel::conformance

#endif
