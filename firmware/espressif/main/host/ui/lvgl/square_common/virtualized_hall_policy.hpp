#pragma once

#include "host/ui/lvgl/square_common/square_presentation.hpp"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"

namespace micropixel::host_ui::lvgl::square_common {

[[nodiscard]] HallPresentationRect HallCardPresentationRect(const SquareSystemUiProfile& profile, uint32_t index,
                                                            int32_t scroll_offset);
[[nodiscard]] HallTransitionPresentation HallTransitionPresentationFor(const SquareSystemUiProfile& profile,
                                                                       uint32_t index, int32_t scroll_offset);
void MaskHallTransitionCoverRgb888(const HallTransitionPresentation& presentation, uint8_t* destination);

class VirtualizedHallPolicy final {
   public:
    VirtualizedHallPolicy(SquareSystemUiState& state, SquarePresentation& presentation);

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> Show(const host_ui::HallModel& model,
                                                                   host_ui::SystemUiActionSink action_sink,
                                                                   void* action_context);
    void UpdateStatusBar(const host_ui::HallStatusBarModel& model);
    void UpdateInstallProgress(uint32_t app_index, uint8_t progress_percent);
    void PauseCoverLoading();
    void Leave();

   private:
    static void ResetCallback(void* context);
    static void ShowPlaceholder(void* context, uint32_t app_index);
    static void AttachCover(void* context, uint32_t app_index, const host_ui::HallCoverModel& cover);
    static void DetachCover(void* context, uint32_t app_index);
    static void RequestRefresh(void* context);
    static void CarouselEvent(lv_event_t* event);
    static void CardEvent(lv_event_t* event);
    static void StopButtonEvent(lv_event_t* event);

    [[nodiscard]] int32_t CardStep() const;
    [[nodiscard]] int32_t MaximumOffset(uint32_t app_count) const;
    [[nodiscard]] int32_t ClampOffset(uint32_t app_count, int32_t offset) const;
    [[nodiscard]] int32_t RevealOffset(uint32_t app_count, int32_t offset, uint32_t index) const;
    [[nodiscard]] uint32_t CoverWindowFirst(uint32_t app_count, int32_t offset) const;
    [[nodiscard]] uint32_t CoverWindowLast(uint32_t app_count, int32_t offset) const;
    [[nodiscard]] uint32_t RunningAppIndex() const;
    [[nodiscard]] uint32_t FindCardIndex(const lv_obj_t* card) const;
    [[nodiscard]] bool PrepareSource(const host_ui::HallCoverModel& source, uint32_t index,
                                     host_ui::HallCoverModel& prepared);
    [[nodiscard]] bool PrepareCleanBackgroundLocked(uint32_t running_index);
    void ResetLocked();
    void UpdateCarouselLocked(int32_t offset);
    void RequestCoverWindowLocked(bool force = false);
    void DrawCard(lv_obj_t* parent, const HallAppPresentation& app, uint32_t index);
    void DestroyCardLocked(uint32_t index);
    void SyncCardWindowLocked();
    void ShowPlaceholderLocked(uint32_t index);
    void AttachCoverLocked(uint32_t index, const host_ui::HallCoverModel& cover);

    SquareSystemUiState& state_;
    SquarePresentation& presentation_;
};

}  // namespace micropixel::host_ui::lvgl::square_common
