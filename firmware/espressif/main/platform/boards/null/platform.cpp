#include "platform/platform.hpp"

namespace micropixel::platform {
namespace {

// Compilation baseline and example of progressive bring-up: identity alone is
// valid, and shared unavailable implementations fill every optional service.
class NullBoard final : public Board {
   public:
    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        const BoardRegistration registration{{.board = "Null board"}};
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
