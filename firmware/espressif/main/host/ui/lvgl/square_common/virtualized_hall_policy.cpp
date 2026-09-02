#include "host/ui/lvgl/square_common/virtualized_hall_policy.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <utility>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "host/ui/lvgl/square_common/hall_catalog.hpp"
#include "host/ui/lvgl/square_common/hall_cover_codec.hpp"
#include "host/ui/lvgl/square_common/hall_transition_policy.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"

namespace micropixel::host_ui::lvgl::square_common {
namespace {
constexpr char kTag[] = "square_virtual_hall";
constexpr uint32_t kCoverPrefetchCards = 1U;
}  // namespace

HallPresentationRect HallCardPresentationRect(const SquareSystemUiProfile& profile, uint32_t index,
                                              int32_t scroll_offset) {
    return {.x = profile.hall_scene.carousel.x +
                 static_cast<int32_t>(index) * (profile.hall_scene.card_width + profile.hall_scene.card_gap) -
                 scroll_offset,
            .y = profile.hall_scene.carousel.y,
            .width = profile.hall_scene.card_width,
            .height = profile.hall_scene.carousel.height};
}

HallTransitionPresentation HallTransitionPresentationFor(const SquareSystemUiProfile& profile, uint32_t index,
                                                         int32_t scroll_offset) {
    HallPresentationRect card = HallCardPresentationRect(profile, index, scroll_offset);
    const int32_t viewport_left = profile.hall_scene.carousel.x;
    const int32_t viewport_right = viewport_left + profile.hall_scene.carousel.width;
    card.x = std::clamp(card.x, viewport_left, viewport_right - card.width);
    card.height = profile.hall_scene.card_width;
    const uint32_t cover_size = static_cast<uint32_t>(profile.hall_card.width);
    const uint32_t cover_stride = cover_size * 3U;
    return {
        .card = card,
        .cover_size = cover_size,
        .cover_stride = cover_stride,
        .cover_bytes = cover_stride * cover_size,
        .intermediate_size = profile.square.transition_intermediate_width,
        .cover_corner_radius = static_cast<uint32_t>(profile.hall_card.radius),
        .cover_top_background_rgb = theme::kHallBackground,
        .cover_bottom_background_rgb = theme::kHallCardBackground,
        .card_valid = index < host_ui::kMaxHallApps,
    };
}

void MaskHallTransitionCoverRgb888(const HallTransitionPresentation& presentation, uint8_t* destination) {
    MaskHallCoverRgb888(destination, presentation.cover_size, presentation.cover_corner_radius,
                        presentation.cover_top_background_rgb, presentation.cover_bottom_background_rgb);
}

VirtualizedHallPolicy::VirtualizedHallPolicy(SquareSystemUiState& state, SquarePresentation& presentation)
    : state_(state), presentation_(presentation) {
    state_.BindHallReset(ResetCallback, this);
    state_.hall_cover_cache.BindUi({
        .context = this,
        .show_placeholder = ShowPlaceholder,
        .attach = AttachCover,
        .detach = DetachCover,
        .request_refresh = RequestRefresh,
    });
}

void VirtualizedHallPolicy::ResetCallback(void* context) {
    static_cast<VirtualizedHallPolicy*>(context)->ResetLocked();
}

int32_t VirtualizedHallPolicy::CardStep() const {
    return state_.profile.hall_scene.card_width + state_.profile.hall_scene.card_gap;
}

int32_t VirtualizedHallPolicy::MaximumOffset(uint32_t app_count) const {
    const uint32_t visible = state_.profile.hall_scene.fully_visible_cards;
    return static_cast<int32_t>(app_count > visible ? app_count - visible : 0U) * CardStep();
}

int32_t VirtualizedHallPolicy::ClampOffset(uint32_t app_count, int32_t offset) const {
    return std::clamp<int32_t>(offset, 0, MaximumOffset(app_count));
}

int32_t VirtualizedHallPolicy::RevealOffset(uint32_t app_count, int32_t offset, uint32_t index) const {
    const int32_t clamped = ClampOffset(app_count, offset);
    if (index >= app_count) {
        return clamped;
    }
    const HallPresentationRect card = HallCardPresentationRect(state_.profile, index, clamped);
    const int32_t viewport_left = state_.profile.hall_scene.carousel.x;
    const int32_t viewport_right = viewport_left + state_.profile.hall_scene.carousel.width;
    if (card.x < viewport_left) {
        return ClampOffset(app_count, clamped - (viewport_left - card.x));
    }
    if (card.x + card.width > viewport_right) {
        return ClampOffset(app_count, clamped + card.x + card.width - viewport_right);
    }
    return clamped;
}

uint32_t VirtualizedHallPolicy::CoverWindowFirst(uint32_t app_count, int32_t offset) const {
    if (app_count == 0U) {
        return 0U;
    }
    const uint32_t first_visible = static_cast<uint32_t>(ClampOffset(app_count, offset) / CardStep());
    return first_visible > kCoverPrefetchCards ? first_visible - kCoverPrefetchCards : 0U;
}

uint32_t VirtualizedHallPolicy::CoverWindowLast(uint32_t app_count, int32_t offset) const {
    if (app_count == 0U) {
        return 0U;
    }
    const int32_t clamped = ClampOffset(app_count, offset);
    const uint32_t last_intersecting =
        static_cast<uint32_t>((clamped + state_.profile.hall_scene.carousel.width + CardStep() - 1) / CardStep());
    return std::min(app_count, last_intersecting + kCoverPrefetchCards);
}

void VirtualizedHallPolicy::ResetLocked() {
    state_.hall_scene_ui.ResetLocked();
    state_.hall_card_window_first = host_ui::kMaxHallApps;
    state_.hall_card_window_last = host_ui::kMaxHallApps;
    for (uint32_t index = 0U; index < host_ui::kMaxHallApps; ++index) {
        state_.hall_cards[index] = nullptr;
        state_.hall_card_press_overlays[index] = nullptr;
        state_.hall_cover_images[index] = nullptr;
        state_.hall_cover_placeholders[index] = nullptr;
        state_.hall_install_progress_arcs[index] = nullptr;
        state_.hall_install_progress_labels[index] = nullptr;
        state_.hall_app_running[index] = false;
        auto& descriptor = state_.hall_cover_descriptors[index];
        if (descriptor.data != nullptr) {
            lv_image_cache_drop(&descriptor);
            lv_image_header_cache_drop(&descriptor);
        }
        descriptor = {};
    }
}

void VirtualizedHallPolicy::ShowPlaceholderLocked(uint32_t index) {
    if (index >= host_ui::kMaxHallApps) {
        return;
    }
    if (state_.hall_cover_images[index] != nullptr) {
        lv_obj_add_flag(state_.hall_cover_images[index], LV_OBJ_FLAG_HIDDEN);
    }
    if (state_.hall_install_progress_arcs[index] != nullptr) {
        if (state_.hall_cover_placeholders[index] != nullptr) {
            lv_obj_add_flag(state_.hall_cover_placeholders[index], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (state_.hall_cover_placeholders[index] != nullptr) {
        lv_obj_remove_flag(state_.hall_cover_placeholders[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void VirtualizedHallPolicy::AttachCoverLocked(uint32_t index, const host_ui::HallCoverModel& cover) {
    if (index >= state_.hall_app_count || cover.data == nullptr || state_.hall_cover_images[index] == nullptr) {
        return;
    }
    if (state_.hall_install_progress_arcs[index] != nullptr) {
        ShowPlaceholderLocked(index);
        return;
    }
    auto& descriptor = state_.hall_cover_descriptors[index];
    if (descriptor.data != nullptr && descriptor.data != cover.data) {
        lv_image_cache_drop(&descriptor);
        lv_image_header_cache_drop(&descriptor);
    }
    descriptor = {};
    descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf = LV_COLOR_FORMAT_RGB888;
    descriptor.header.w = static_cast<uint32_t>(state_.profile.hall_card.width);
    descriptor.header.h = static_cast<uint32_t>(state_.profile.hall_card.width);
    descriptor.header.stride = cover.stride;
    descriptor.data_size = cover.size;
    descriptor.data = cover.data;
    lv_image_set_src(state_.hall_cover_images[index], &descriptor);
    lv_obj_remove_flag(state_.hall_cover_images[index], LV_OBJ_FLAG_HIDDEN);
    if (state_.hall_cover_placeholders[index] != nullptr) {
        lv_obj_add_flag(state_.hall_cover_placeholders[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void VirtualizedHallPolicy::ShowPlaceholder(void* context, uint32_t app_index) {
    static_cast<VirtualizedHallPolicy*>(context)->ShowPlaceholderLocked(app_index);
}

void VirtualizedHallPolicy::AttachCover(void* context, uint32_t app_index, const host_ui::HallCoverModel& cover) {
    static_cast<VirtualizedHallPolicy*>(context)->AttachCoverLocked(app_index, cover);
}

void VirtualizedHallPolicy::DetachCover(void* context, uint32_t app_index) {
    auto& policy = *static_cast<VirtualizedHallPolicy*>(context);
    if (app_index >= host_ui::kMaxHallApps) {
        return;
    }
    auto& descriptor = policy.state_.hall_cover_descriptors[app_index];
    if (descriptor.data != nullptr) {
        lv_image_cache_drop(&descriptor);
        lv_image_header_cache_drop(&descriptor);
        descriptor = {};
    }
    policy.ShowPlaceholderLocked(app_index);
}

void VirtualizedHallPolicy::RequestRefresh(void* context) {
    auto& policy = *static_cast<VirtualizedHallPolicy*>(context);
    if (policy.state_.display != nullptr) {
        platform::lvgl::RequestDisplayRefresh(policy.state_.display);
    }
}

bool VirtualizedHallPolicy::PrepareSource(const host_ui::HallCoverModel& source, uint32_t index,
                                          host_ui::HallCoverModel& prepared) {
    if (source.data == nullptr || (!esp_ptr_in_drom(source.data) && !esp_ptr_external_ram(source.data)) ||
        source.width == 0U || source.height == 0U || source.size == 0U) {
        return false;
    }
    DisplayTransition* transition = presentation_.Transition();
    if (transition != nullptr && transition->RetainNativeCover(source, prepared)) {
        return true;
    }
    if (source.format == host_ui::HallCoverFormat::kJpeg || source.format == host_ui::HallCoverFormat::kPng) {
        if (!esp_ptr_in_drom(source.data) || source.stride != 0U) {
            return false;
        }
    } else {
        const uint64_t minimum_stride = static_cast<uint64_t>(source.width) * 3U;
        if (source.stride < minimum_stride || static_cast<uint64_t>(source.stride) * source.height > source.size) {
            return false;
        }
    }
    return index < host_ui::kMaxHallApps && state_.hall_cover_cache.PrepareSource(source, prepared);
}

void VirtualizedHallPolicy::DrawCard(lv_obj_t* parent, const HallAppPresentation& app, uint32_t index) {
    const HallPresentationRect bounds = HallCardPresentationRect(state_.profile, index, 0);
    HallCardObjects objects{};
    DrawHallCard(parent, state_.profile.hall_card,
                 {.app_id = app.app_id.data(),
                  .display_name = app.display_name.data(),
                  .install_progress_percent = app.install_progress_percent,
                  .running = app.running,
                  .installing = app.installing},
                 index, CardEvent, StopButtonEvent, this, objects);
    state_.hall_cards[index] = objects.card;
    state_.hall_cover_images[index] = objects.cover_image;
    state_.hall_cover_placeholders[index] = objects.cover_placeholder;
    state_.hall_install_progress_arcs[index] = objects.install_progress_arc;
    state_.hall_install_progress_labels[index] = objects.install_progress_label;
    state_.hall_card_press_overlays[index] = objects.press_overlay;
    lv_obj_set_pos(objects.card, bounds.x - state_.profile.hall_scene.carousel.x, 0);
    host_ui::HallCoverModel prepared{};
    if (PrepareSource(state_.hall_cover_sources[index], index, prepared)) {
        AttachCoverLocked(index, prepared);
    }
    state_.hall_app_running[index] = app.running;
}

void VirtualizedHallPolicy::DestroyCardLocked(uint32_t index) {
    if (index >= host_ui::kMaxHallApps || state_.hall_cards[index] == nullptr) {
        return;
    }
    auto& descriptor = state_.hall_cover_descriptors[index];
    if (descriptor.data != nullptr) {
        lv_image_cache_drop(&descriptor);
        lv_image_header_cache_drop(&descriptor);
        descriptor = {};
    }
    lv_obj_delete(state_.hall_cards[index]);
    state_.hall_cards[index] = nullptr;
    state_.hall_card_press_overlays[index] = nullptr;
    state_.hall_cover_images[index] = nullptr;
    state_.hall_cover_placeholders[index] = nullptr;
    state_.hall_install_progress_arcs[index] = nullptr;
    state_.hall_install_progress_labels[index] = nullptr;
}

void VirtualizedHallPolicy::SyncCardWindowLocked() {
    lv_obj_t* content = state_.hall_scene_ui.objects().carousel_content;
    if (content == nullptr) {
        return;
    }
    const uint32_t first = CoverWindowFirst(state_.hall_app_count, state_.hall_scroll_offset);
    const uint32_t last = CoverWindowLast(state_.hall_app_count, state_.hall_scroll_offset);
    if (first == state_.hall_card_window_first && last == state_.hall_card_window_last) {
        return;
    }
    for (uint32_t index = 0U; index < state_.hall_app_count; ++index) {
        if (state_.hall_cards[index] != nullptr && (index < first || index >= last)) {
            DestroyCardLocked(index);
        }
    }
    for (uint32_t index = first; index < last; ++index) {
        if (state_.hall_cards[index] == nullptr) {
            DrawCard(content, state_.hall_app_presentations[index], index);
        }
    }
    state_.hall_card_window_first = first;
    state_.hall_card_window_last = last;
}

void VirtualizedHallPolicy::RequestCoverWindowLocked(bool force) {
    if (state_.hall_app_count == 0U || state_.hall_scene_ui.objects().carousel_content == nullptr) {
        return;
    }
    state_.hall_cover_cache.RequestWindow(CoverWindowFirst(state_.hall_app_count, state_.hall_scroll_offset),
                                          CoverWindowLast(state_.hall_app_count, state_.hall_scroll_offset), force);
}

void VirtualizedHallPolicy::UpdateCarouselLocked(int32_t offset) {
    state_.hall_scroll_offset = ClampOffset(state_.hall_app_count, offset);
    state_.hall_scene_ui.UpdateScrollLocked(state_.hall_app_count, state_.hall_scroll_offset);
    SyncCardWindowLocked();
    RequestCoverWindowLocked();
}

uint32_t VirtualizedHallPolicy::RunningAppIndex() const {
    for (uint32_t index = 0U; index < state_.hall_app_count; ++index) {
        if (state_.hall_app_running[index]) {
            return index;
        }
    }
    return host_ui::kMaxHallApps;
}

uint32_t VirtualizedHallPolicy::FindCardIndex(const lv_obj_t* card) const {
    for (uint32_t index = 0U; index < state_.hall_app_count; ++index) {
        if (state_.hall_cards[index] == card) {
            return index;
        }
    }
    return host_ui::kMaxHallApps;
}

bool VirtualizedHallPolicy::PrepareCleanBackgroundLocked(uint32_t running_index) {
    DisplayTransition* transition = presentation_.Transition();
    if (transition == nullptr) {
        return false;
    }
    lv_obj_t* content = state_.hall_scene_ui.objects().carousel_content;
    if (running_index >= host_ui::kMaxHallApps || state_.hall_cards[running_index] == nullptr || content == nullptr) {
        return transition->PrepareBackgroundLocked(state_.root);
    }
    const host_ui::HallCoverModel running_cover = state_.hall_cover_sources[running_index];
    DestroyCardLocked(running_index);
    state_.hall_cover_sources[running_index] = state_.hall_idle_cover_sources[running_index];
    state_.hall_app_presentations[running_index].running = false;
    DrawCard(content, state_.hall_app_presentations[running_index], running_index);
    const bool prepared = transition->PrepareBackgroundLocked(state_.root);
    DestroyCardLocked(running_index);
    state_.hall_cover_sources[running_index] = running_cover;
    state_.hall_app_presentations[running_index].running = true;
    DrawCard(content, state_.hall_app_presentations[running_index], running_index);
    return prepared;
}

void VirtualizedHallPolicy::CarouselEvent(lv_event_t* event) {
    auto* policy = static_cast<VirtualizedHallPolicy*>(lv_event_get_user_data(event));
    lv_obj_t* viewport = lv_event_get_current_target_obj(event);
    if (policy == nullptr || viewport == nullptr ||
        viewport != policy->state_.hall_scene_ui.objects().carousel_viewport) {
        return;
    }
    policy->UpdateCarouselLocked(lv_obj_get_scroll_x(viewport));
    if (lv_event_get_code(event) == LV_EVENT_SCROLL_END) {
        policy->RequestCoverWindowLocked(true);
        const uint32_t running = policy->RunningAppIndex();
        if (running < policy->state_.hall_app_count) {
            (void)policy->PrepareCleanBackgroundLocked(running);
        }
    }
    platform::lvgl::RequestDisplayRefresh(policy->state_.display);
}

void VirtualizedHallPolicy::CardEvent(lv_event_t* event) {
    auto* policy = static_cast<VirtualizedHallPolicy*>(lv_event_get_user_data(event));
    lv_obj_t* card = lv_event_get_current_target_obj(event);
    if (policy == nullptr || card == nullptr) {
        return;
    }
    const uint32_t index = policy->FindCardIndex(card);
    if (index >= policy->state_.hall_app_count) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        SetHallCardPressed({.press_overlay = policy->state_.hall_card_press_overlays[index]}, true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        SetHallCardPressed({.press_overlay = policy->state_.hall_card_press_overlays[index]}, false);
    } else if (code == LV_EVENT_SHORT_CLICKED && policy->state_.hall_launch_enabled &&
               policy->state_.hall_action_sink != nullptr) {
        const int32_t reveal =
            policy->RevealOffset(policy->state_.hall_app_count, policy->state_.hall_scroll_offset, index);
        lv_obj_t* viewport = policy->state_.hall_scene_ui.objects().carousel_viewport;
        if (reveal != policy->state_.hall_scroll_offset && viewport != nullptr) {
            lv_obj_scroll_to_x(viewport, reveal, LV_ANIM_OFF);
            policy->UpdateCarouselLocked(lv_obj_get_scroll_x(viewport));
        }
        policy->state_.hall_action_sink(
            policy->state_.hall_action_context,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kLaunchApp, .app_index = index});
    }
}

void VirtualizedHallPolicy::StopButtonEvent(lv_event_t* event) {
    auto* policy = static_cast<VirtualizedHallPolicy*>(lv_event_get_user_data(event));
    lv_obj_t* stop = lv_event_get_current_target_obj(event);
    const lv_obj_t* card = stop != nullptr ? lv_obj_get_parent(stop) : nullptr;
    if (policy == nullptr || card == nullptr) {
        return;
    }
    const uint32_t index = policy->FindCardIndex(card);
    if (index < policy->state_.hall_app_count && policy->state_.hall_app_running[index] &&
        policy->state_.hall_action_sink != nullptr) {
        policy->state_.hall_action_sink(
            policy->state_.hall_action_context,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kStopApp, .app_index = index});
    }
}

std::expected<void, host_ui::SystemUiError> VirtualizedHallPolicy::Show(const host_ui::HallModel& model,
                                                                        host_ui::SystemUiActionSink action_sink,
                                                                        void* action_context) {
    if (state_.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    const uint32_t visible_count = HallVisibleAppCount(model);
    const uint64_t signature = HallCatalogSignature(model, visible_count);
    const bool may_resume = state_.hall_retained_for_status && state_.root != nullptr &&
                            HallResumeModelMatches(model, visible_count, signature, state_.hall_app_count,
                                                   state_.hall_catalog_signature, state_.hall_app_running,
                                                   state_.hall_launch_enabled, state_.hall_firmware_update_available);
    state_.hall_retained_for_status = false;
    if (may_resume && esp_lv_adapter_lock(-1) == ESP_OK) {
        const auto& objects = state_.hall_scene_ui.objects();
        if (lv_obj_is_valid(state_.root) && objects.carousel_content != nullptr &&
            lv_obj_is_valid(objects.carousel_content)) {
            if (!state_.hall_status_bar_valid || !HallStatusBarMatches(state_.hall_status_bar, model.status_bar)) {
                state_.hall_scene_ui.UpdateStatusBarLocked(model.status_bar);
                state_.hall_status_bar = model.status_bar;
                state_.hall_status_bar_valid = true;
                platform::lvgl::RequestDisplayRefresh(state_.display);
            }
            state_.status_layer_ui.RaisePerformanceOverlayLocked();
            state_.SetHostPointerEnabledLocked(true);
            esp_lv_adapter_unlock();
            state_.hall_action_sink = action_sink;
            state_.hall_action_context = action_context;
            state_.BindHostPointerTouchSink();
            state_.input_router.BindSystemActionSink(action_sink, action_context);
            return {};
        }
        esp_lv_adapter_unlock();
    }
    if (signature != state_.hall_catalog_signature) {
        state_.hall_catalog_signature = signature;
        state_.hall_scroll_offset = 0;
        state_.hall_idle_cover_sources.fill({});
    }
    state_.hall_cover_cache.BeginCatalog(model, visible_count, signature);
    state_.hall_app_count = visible_count;
    state_.hall_cover_sources.fill({});
    state_.hall_app_presentations.fill({});
    for (uint32_t index = 0U; index < visible_count; ++index) {
        state_.hall_cover_sources[index] = model.apps[index].cover;
        if (!model.apps[index].running) {
            state_.hall_idle_cover_sources[index] = model.apps[index].cover;
        }
        auto& presentation = state_.hall_app_presentations[index];
        (void)std::snprintf(presentation.app_id.data(), presentation.app_id.size(), "%s",
                            model.apps[index].app_id != nullptr ? model.apps[index].app_id : "");
        (void)std::snprintf(presentation.display_name.data(), presentation.display_name.size(), "%s",
                            model.apps[index].display_name != nullptr ? model.apps[index].display_name : "");
        presentation.running = model.apps[index].running;
        presentation.installing = model.apps[index].installing;
        presentation.install_progress_percent = model.apps[index].install_progress_percent;
    }
    state_.hall_scroll_offset = ClampOffset(visible_count, state_.hall_scroll_offset);
    const bool hall_was_visible = state_.root != nullptr && state_.hall_action_context != nullptr;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    state_.SetHostPointerEnabledLocked(false);
    presentation_.BeforeHallRebuildLocked();
    if (state_.EnsureRootLocked(theme::kHallBackground) == nullptr) {
        esp_lv_adapter_unlock();
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    lv_obj_clean(state_.root);
    ResetLocked();
    lv_obj_set_pos(state_.root, 0, 0);
    lv_obj_set_style_opa(state_.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(state_.root, lv_color_hex(theme::kHallBackground), 0);
    state_.DropLaunchBitmapLocked();
    state_.hall_scene_ui.DrawLocked(state_.root, state_.profile.hall_scene, model, visible_count,
                                    {.carousel_event = CarouselEvent,
                                     .carousel_context = this,
                                     .action_sink = action_sink,
                                     .action_context = action_context});
    const auto& scene = state_.hall_scene_ui.objects();
    lv_obj_update_layout(scene.carousel_viewport);
    lv_obj_scroll_to_x(scene.carousel_viewport, state_.hall_scroll_offset, LV_ANIM_OFF);
    UpdateCarouselLocked(lv_obj_get_scroll_x(scene.carousel_viewport));
    const uint32_t running = RunningAppIndex();
    DisplayTransition* transition = presentation_.Transition();
    lv_obj_t* guest = state_.GuestFrameLocked();
    const bool candidate =
        running < visible_count && guest != nullptr && transition != nullptr && transition->EnterTransitionPending();
    bool background_ready = false;
    if (running < visible_count && transition != nullptr && transition->BackgroundAvailable() &&
        state_.hall_cards[running] != nullptr) {
        background_ready = transition->UpdateBackgroundRegionLocked(
            state_.hall_cards[running], HallCardPresentationRect(state_.profile, running, state_.hall_scroll_offset));
    }
    if (!background_ready && running < visible_count) {
        background_ready = PrepareCleanBackgroundLocked(running);
        if (background_ready && transition != nullptr && state_.hall_cards[running] != nullptr) {
            background_ready = transition->UpdateBackgroundRegionLocked(
                state_.hall_cards[running],
                HallCardPresentationRect(state_.profile, running, state_.hall_scroll_offset));
        }
    } else if (!background_ready && transition != nullptr) {
        background_ready = transition->PrepareBackgroundLocked(state_.root);
    }
    const bool transition_ready = candidate && background_ready;
    lv_obj_move_foreground(transition_ready ? guest : state_.root);
    if (state_.status_layer_ui.PerformanceOverlayVisibleLocked()) {
        state_.status_layer_ui.RaisePerformanceOverlayLocked();
    }
    if (!transition_ready) {
        platform::lvgl::RequestDisplayRefresh(state_.display);
    }
    state_.SetHostPointerEnabledLocked(true);
    esp_lv_adapter_unlock();
    if (transition_ready) {
        const bool animated =
            transition->AnimateToHall(HallCardPresentationRect(state_.profile, running, state_.hall_scroll_offset),
                                      state_.profile.square.guest_transition_duration_ms, model.transition_trigger_us);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_obj_set_pos(state_.root, 0, 0);
            lv_obj_set_style_opa(state_.root, LV_OPA_COVER, 0);
            lv_obj_move_foreground(state_.root);
            if (state_.status_layer_ui.PerformanceOverlayVisibleLocked()) {
                state_.status_layer_ui.RaisePerformanceOverlayLocked();
            }
            platform::lvgl::RequestDisplayRefresh(state_.display);
            esp_lv_adapter_unlock();
        }
        if (!animated) {
            ESP_LOGW(kTag, "Guest-to-Hall transition failed; Hall restored directly");
        }
    } else if (transition != nullptr) {
        transition->CancelEnterTransition();
        if (!hall_was_visible) {
            ESP_LOGI(kTag, "Hall presented without a root transition");
        }
    }
    state_.hall_action_sink = action_sink;
    state_.hall_action_context = action_context;
    state_.hall_firmware_update_available = model.firmware_update_available;
    state_.hall_launch_enabled = model.launch_enabled;
    state_.hall_status_bar = model.status_bar;
    state_.hall_status_bar_valid = true;
    state_.BindHostPointerTouchSink();
    state_.input_router.BindSystemActionSink(action_sink, action_context);
    ESP_LOGI(kTag, "Hall visible: apps=%" PRIu32, visible_count);
    return {};
}

void VirtualizedHallPolicy::UpdateStatusBar(const host_ui::HallStatusBarModel& model) {
    if (state_.display == nullptr || state_.root == nullptr || state_.hall_action_context == nullptr ||
        esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    state_.hall_scene_ui.UpdateStatusBarLocked(model);
    state_.hall_status_bar = model;
    state_.hall_status_bar_valid = true;
    platform::lvgl::RequestDisplayRefresh(state_.display);
    esp_lv_adapter_unlock();
}

void VirtualizedHallPolicy::UpdateInstallProgress(uint32_t app_index, uint8_t progress_percent) {
    if (app_index >= state_.hall_app_count || state_.display == nullptr || esp_lv_adapter_lock(0) != ESP_OK) {
        return;
    }
    SetHallCardInstallProgress({.install_progress_arc = state_.hall_install_progress_arcs[app_index],
                                .install_progress_label = state_.hall_install_progress_labels[app_index]},
                               progress_percent);
    platform::lvgl::RequestDisplayRefresh(state_.display);
    esp_lv_adapter_unlock();
}

void VirtualizedHallPolicy::PauseCoverLoading() { state_.hall_cover_cache.Pause(); }

void VirtualizedHallPolicy::PrepareLaunch(uint32_t app_index) {
    pending_launch_index_ = app_index < state_.hall_app_count ? app_index : host_ui::kMaxHallApps;
}

void VirtualizedHallPolicy::Leave() {
    PauseCoverLoading();
    const uint32_t launch_index = std::exchange(pending_launch_index_, host_ui::kMaxHallApps);
    const uint32_t running = RunningAppIndex();
    const uint32_t app_count = state_.hall_app_count;
    state_.UnbindHostPointerTouchSink();
    state_.input_router.ClearSystemActionSink(state_.hall_action_context);
    state_.hall_action_sink = nullptr;
    state_.hall_action_context = nullptr;
    state_.hall_launch_enabled = false;
    state_.hall_retained_for_status = false;
    state_.hall_status_bar_valid = false;
    if (state_.display == nullptr || state_.root == nullptr || !state_.GuestRefreshSynchronizationAvailable()) {
        state_.hall_app_count = 0U;
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        state_.hall_app_count = 0U;
        return;
    }
    device::BitmapView launch_cover{};
    if (launch_index < app_count) {
        const lv_image_dsc_t& descriptor = state_.hall_cover_descriptors[launch_index];
        if (descriptor.data != nullptr && descriptor.header.cf == LV_COLOR_FORMAT_RGB888 && descriptor.header.w > 0U &&
            descriptor.header.h > 0U && descriptor.header.stride > 0U &&
            descriptor.data_size == descriptor.header.stride * descriptor.header.h) {
            launch_cover = {.data = descriptor.data,
                            .size = descriptor.data_size,
                            .width = descriptor.header.w,
                            .height = descriptor.header.h,
                            .stride = descriptor.header.stride,
                            .pixel_format = MICROPIXEL_PIXEL_FORMAT_BGR888};
        }
    }
    state_.SetHostPointerEnabledLocked(false);
    state_.DrainGuestRefreshReady();
    DisplayTransition* transition = presentation_.Transition();
    const HallLaunchBackgroundPlan plan =
        PlanHallLaunchBackground(running < app_count, transition != nullptr && transition->BackgroundAvailable());
    if (plan != HallLaunchBackgroundPlan::kReuseCleanBaseline) {
        if (plan == HallLaunchBackgroundPlan::kPrepareCleanBaseline) {
            (void)PrepareCleanBackgroundLocked(running);
        } else if (transition != nullptr) {
            (void)transition->PrepareBackgroundLocked(state_.root);
        }
    }
    state_.hall_app_count = 0U;
    lv_obj_clean(state_.root);
    lv_obj_set_pos(state_.root, 0, 0);
    lv_obj_set_style_opa(state_.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(state_.root, lv_color_hex(theme::kHallBackground), 0);
    state_.DropLaunchBitmapLocked();
    platform::lvgl::RequestDisplayRefresh(state_.display);
    esp_lv_adapter_unlock();
    state_.WaitForGuestRefreshReady();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        ResetLocked();
        esp_lv_adapter_unlock();
    }
    if (launch_cover.data != nullptr) {
        const int32_t status = state_.ShowLaunchBitmap(launch_cover);
        if (status == MICROPIXEL_STATUS_OK) {
            ESP_LOGI(kTag, "retained Hall card as native-size App launch cover: index=%" PRIu32 " size=%" PRIu32,
                     launch_index, launch_cover.width);
        } else {
            ESP_LOGW(kTag, "unable to retain Hall card as App launch cover: index=%" PRIu32 " status=%" PRId32,
                     launch_index, status);
        }
    }
}

}  // namespace micropixel::host_ui::lvgl::square_common
