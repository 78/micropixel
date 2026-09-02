#include "platform/platform.hpp"

#include "esp_log.h"
#include "platform/defaults/unavailable_services.hpp"

namespace micropixel::platform {
namespace {

constexpr char kTag[] = "micropixel_null";

#if CONFIG_MICROPIXEL_SMOKE_AUTORUN_FIRST_APP
class SmokeSystemUi final : public defaults::UnavailableSystemUi {
   public:
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowHall(const host_ui::HallModel& model,
                                                                       host_ui::SystemUiActionSink action_sink,
                                                                       void* action_context) override {
        if (!launch_requested_ && model.launch_enabled && model.app_count != 0U && action_sink != nullptr) {
            launch_requested_ = true;
            ESP_LOGI(kTag, "smoke autorun requesting catalog index 0 of %lu App(s)",
                     static_cast<unsigned long>(model.app_count));
            action_sink(action_context,
                        host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kLaunchApp, .app_index = 0U});
        }
        return {};
    }

   private:
    bool launch_requested_{};
};

SmokeSystemUi& SelfTestSystemUi() {
    static SmokeSystemUi value;
    return value;
}
#endif

// Compilation baseline and example of progressive bring-up: identity alone is
// valid, and shared unavailable implementations fill every optional service.
class NullBoard final : public Board {
   public:
    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        BoardRegistration registration{{.board = "Null board"}};
#if CONFIG_MICROPIXEL_SMOKE_AUTORUN_FIRST_APP
        registration.SetSystemUi(SelfTestSystemUi());
        ESP_LOGI(kTag, "Null smoke board initialized; waiting for one AOT App");
#endif
        return context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    void BindBackgroundExecutor(work::BackgroundExecutor&) override {}
};

}  // namespace

Board& ConfiguredBoard() {
    static NullBoard board;
    return board;
}

}  // namespace micropixel::platform
