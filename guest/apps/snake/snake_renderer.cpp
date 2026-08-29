#include "apps/snake/gamekit/canvas_geometry.hpp"
#include "apps/snake/snake_game.hpp"

namespace snake {
namespace {

constexpr int32_t kBoardPadding = 8;
constexpr micropixel::Rect kBoardClip{
    kBoardX - kBoardPadding,
    kBoardY - kBoardPadding,
    static_cast<int32_t>(kColumns) * kCellPitch + kBoardPadding * 2,
    static_cast<int32_t>(kRows) * kCellPitch + kBoardPadding * 2,
};
constexpr micropixel::Rect kDisplayClip{0, 0, kScreenWidth, kScreenHeight};
constexpr micropixel::Point kTitlePosition{kBoardX, 8};
constexpr micropixel::Point kLevelPosition{460, 12};
constexpr micropixel::Point kScoreLabelPosition{545, 12};
constexpr micropixel::Point kScoreValuePosition{545, 33};
constexpr micropixel::Point kBestLabelPosition{645, 12};
constexpr micropixel::Point kBestValuePosition{635, 33};
constexpr micropixel::Point kStatusPosition{380, 27};
constexpr int32_t kComboBarX = kStatusPosition.x;
constexpr int32_t kComboBarY = 54;
constexpr uint32_t kComboBarWidth = 96U;
constexpr int32_t kGameOverContentOffsetY = -32;
constexpr int32_t kHudEdgePadding = 12;

constexpr uint16_t kFlameOffset = 0U;
constexpr uint16_t kTrailOffset = kFlameOffset + 4U;
constexpr uint16_t kObstacleOffset = kTrailOffset + kTrailPoolSize;
constexpr uint16_t kObstacleDetailOffset = kObstacleOffset + 5U;
constexpr uint16_t kSceneryCapacity = kObstacleDetailOffset + 5U;

constexpr uint16_t kSnakeHeadInstance = kMaxLength;
constexpr uint16_t kSnakeFirstEyeInstance = kMaxLength + 1U;
constexpr uint16_t kSnakeSecondEyeInstance = kMaxLength + 2U;
constexpr uint16_t kSnakeBatchCapacity = kMaxLength + 3U;

void SetLabel(micropixel::LabelNode& label, micropixel::SceneUpdate& update, micropixel::Point position,
              const char* text, micropixel::Color color, micropixel::SystemFont font, bool visible = true) {
    label.SetPosition(update, position);
    label.SetText(update, text);
    label.SetColor(update, color);
    label.SetFont(update, font);
    label.SetVisible(update, visible);
}

micropixel::Point SafeTitlePosition(micropixel::Point position, const micropixel::RendererInfo& info) {
    const int32_t safe_left = static_cast<int32_t>(info.safe_area_insets().left) + kHudEdgePadding;
    if (position.x < safe_left) {
        position.x = safe_left;
    }
    return position;
}

micropixel::Point ShiftLeftOfSafeRight(micropixel::Point position, const micropixel::RendererInfo& info) {
    position.x -= static_cast<int32_t>(info.safe_area_insets().right);
    return position;
}

}  // namespace

void SnakeGame::InitializeScene() {
    if (scene_initialized_) {
        return;
    }
    micropixel::Assert(board_texture_.valid() && start_button_texture_.valid() && restart_button_texture_.valid(),
                       "snake: retained scene textures missing");
    for (uint32_t index = 0U; index < 4U; ++index) {
        micropixel::Assert(food_sheets_[index].valid() && burst_sheets_[index].valid(),
                           "snake: retained scene atlas missing");
    }

    game_layer_ = scene_.CreateLayer({.clip = kBoardClip, .translation = {}, .z_order = 0});
    hud_layer_ = scene_.CreateLayer({.clip = kDisplayClip, .translation = {}, .z_order = 10});

    board_node_ = scene_.CreateSprite(board_texture_, {kBoardX, kBoardY, 625, 625}, {0, 0, 625, 625}, game_layer_);
    scenery_batch_ = scene_.CreateSpriteBatch(kSceneryCapacity, game_layer_);
    food_node_ = scene_.CreateSprite(
        food_sheets_[0],
        {kBoardX, kBoardY, static_cast<int32_t>(kFoodSpriteCellSize), static_cast<int32_t>(kFoodSpriteCellSize)},
        {0, 0, static_cast<int32_t>(kFoodSpriteCellSize), static_cast<int32_t>(kFoodSpriteCellSize)}, game_layer_);
    snake_batch_ = scene_.CreateSpriteBatch(kSnakeBatchCapacity, game_layer_);
    const snake_assets::AtlasFrame& first_burst = snake_assets::burst_atlases[0].frames[0];
    burst_node_ =
        scene_.CreateSprite(burst_sheets_[0], {kBoardX, kBoardY, first_burst.width, first_burst.height},
                            {first_burst.x, first_burst.y, first_burst.width, first_burst.height}, game_layer_);
    particle_batch_ = scene_.CreateSpriteBatch(kParticlePoolSize, game_layer_);
    overlay_node_ =
        scene_.CreateShape({kBoardX, kBoardY, 625, 625}, micropixel::Color::Black(), game_layer_, kOverlayOpacity);
    flash_batch_ = scene_.CreateSpriteBatch(4U, game_layer_);
    button_node_ = scene_.CreateSprite(start_button_texture_, kStartButtonRect,
                                       {0, 0, kActionButtonWidth, kActionButtonHeight}, game_layer_);
    for (micropixel::LabelNode& label : popup_labels_) {
        label = scene_.CreateLabel({kBoardX, kBoardY}, " ", micropixel::Color::White(), micropixel::SystemFont::kLarge,
                                   game_layer_);
    }
    for (micropixel::LabelNode& label : overlay_labels_) {
        label = scene_.CreateLabel({360, 300}, " ", micropixel::Color::White(), micropixel::SystemFont::kLarge,
                                   game_layer_, true);
    }

    combo_batch_ = scene_.CreateSpriteBatch(2U, hud_layer_);
    header_labels_[0] =
        scene_.CreateLabel(kTitlePosition, " ", micropixel::Color::White(), micropixel::SystemFont::kTitle, hud_layer_);
    header_labels_[1] =
        scene_.CreateLabel(kLevelPosition, " ", micropixel::Color::White(), micropixel::SystemFont::kSmall, hud_layer_);
    header_labels_[2] = scene_.CreateLabel(kScoreLabelPosition, " ", micropixel::Color::White(),
                                           micropixel::SystemFont::kSmall, hud_layer_);
    header_labels_[3] = scene_.CreateLabel(kScoreValuePosition, " ", micropixel::Color::White(),
                                           micropixel::SystemFont::kLarge, hud_layer_);
    header_labels_[4] = scene_.CreateLabel(kBestLabelPosition, " ", micropixel::Color::White(),
                                           micropixel::SystemFont::kSmall, hud_layer_);
    header_labels_[5] = scene_.CreateLabel(kBestValuePosition, " ", micropixel::Color::White(),
                                           micropixel::SystemFont::kLarge, hud_layer_);
    header_labels_[6] = scene_.CreateLabel(kStatusPosition, " ", micropixel::Color::White(),
                                           micropixel::SystemFont::kSmall, hud_layer_);
    scene_initialized_ = true;
}

void SnakeGame::Render() {
    InitializeScene();
    const Theme& theme = ThemeForLevel(model_.level());
    const bool shake_active = shake_remaining_us_ != 0U && shake_capture_delay_frames_ == 0U;
    const micropixel::Point translation{shake_active ? ShakeX() : 0, shake_active ? ShakeY() : 0};

    auto update = scene_.BeginUpdate();
    game_layer_.SetTranslation(update, translation);
    // A translated Game Layer is a frozen visual snapshot. Keeping every
    // child property unchanged lets the Host capture it once and move that
    // cache for the remaining shake frames. Effects continue aging in the
    // Guest model and are reconciled when translation returns to zero.
    if (!shake_active) {
        RenderScenery(update, theme);
        RenderFood(update);
        RenderSnake(update, theme);
        RenderFoodBurst(update);
        RenderParticles(update, theme);
        RenderFlash(update, theme);
        RenderOverlay(update, theme);
    }
    RenderHud(update, theme);
    auto presented = update.Present();
    if (!presented.has_value()) {
        Line failure;
        failure.Append("snake: scene update failed: ");
        failure.Append(presented.error().name());
        app_.log().Error(failure.c_str());
        micropixel::Panic("snake: scene update failed");
    }
    if (shake_capture_delay_frames_ != 0U && MotionFractionQ8() == 256U) {
        --shake_capture_delay_frames_;
    }
}

void SnakeGame::SetBoard(micropixel::Texture texture) {
    micropixel::Assert(texture.width() == 625U && texture.height() == 625U, "snake: board dimensions invalid");
    board_texture_ = static_cast<micropixel::Texture&&>(texture);
}

void SnakeGame::SetButtonTextures(micropixel::Texture start, micropixel::Texture restart) {
    micropixel::Assert(start.width() == static_cast<uint32_t>(kActionButtonWidth) &&
                           start.height() == static_cast<uint32_t>(kActionButtonHeight) &&
                           restart.width() == static_cast<uint32_t>(kActionButtonWidth) &&
                           restart.height() == static_cast<uint32_t>(kActionButtonHeight),
                       "snake: button texture dimensions invalid");
    start_button_texture_ = static_cast<micropixel::Texture&&>(start);
    restart_button_texture_ = static_cast<micropixel::Texture&&>(restart);
}

void SnakeGame::SetBurstSheet(FoodType type, micropixel::Texture texture) {
    const uint32_t type_index = static_cast<uint32_t>(type);
    micropixel::Assert(type_index < 4U, "snake: food burst sheet type invalid");
    const snake_assets::Atlas& atlas = snake_assets::burst_atlases[type_index];
    micropixel::Assert(texture.width() == atlas.width && texture.height() == atlas.height,
                       "snake: food burst atlas dimensions invalid");
    burst_sheets_[type_index] = static_cast<micropixel::Texture&&>(texture);
}

void SnakeGame::SetFoodSheet(FoodType type, micropixel::Texture texture) {
    const uint32_t type_index = static_cast<uint32_t>(type);
    micropixel::Assert(type_index < 4U && texture.width() == kFoodSpriteCellSize * kSpriteSheetColumns &&
                           texture.height() == kFoodSpriteCellSize * kSpriteSheetColumns,
                       "snake: food sheet dimensions invalid");
    food_sheets_[type_index] = static_cast<micropixel::Texture&&>(texture);
}

micropixel::Rect SnakeGame::CellRect(Cell cell, int32_t inset, int32_t board_x, int32_t board_y) {
    return {board_x + static_cast<int32_t>(cell.x) * kCellPitch + inset,
            board_y + static_cast<int32_t>(cell.y) * kCellPitch + inset, kCellPitch - inset * 2,
            kCellPitch - inset * 2};
}

uint32_t SnakeGame::MotionFractionQ8() const {
    if (screen_ != Screen::kPlaying) {
        return 256U;
    }
    const uint64_t period = MovementPeriodUs();
    const uint64_t fraction = period == 0U ? 256U : (accumulated_us_ * 256U) / period;
    return fraction > 256U ? 256U : static_cast<uint32_t>(fraction);
}

micropixel::Rect SnakeGame::InterpolatedSlotRect(uint32_t slot, uint32_t index, int32_t inset, int32_t board_x,
                                                 int32_t board_y) const {
    const Cell current = model_.body()[index];
    Cell previous = slot < body_slot_length_ ? body_slot_previous_[slot] : current;
    if (AbsoluteValue(static_cast<int32_t>(current.x) - previous.x) > 1 ||
        AbsoluteValue(static_cast<int32_t>(current.y) - previous.y) > 1) {
        previous = current;
    }
    const uint32_t fraction = MotionFractionQ8();
    return {board_x +
                InterpolateAxis(static_cast<int32_t>(previous.x) * kCellPitch,
                                static_cast<int32_t>(current.x) * kCellPitch, fraction) +
                inset,
            board_y +
                InterpolateAxis(static_cast<int32_t>(previous.y) * kCellPitch,
                                static_cast<int32_t>(current.y) * kCellPitch, fraction) +
                inset,
            kCellPitch - inset * 2, kCellPitch - inset * 2};
}

void SnakeGame::SetSolidInstance(micropixel::SpriteBatch& batch, micropixel::SceneUpdate& update, uint16_t id,
                                 micropixel::Rect rect, micropixel::Color color, bool visible) {
    batch.SetInstance(update, id,
                      {.destination = rect, .source = {}, .color = color, .opacity = 255U, .visible = visible});
}

void SnakeGame::RenderScenery(micropixel::SceneUpdate& update, const Theme& theme) {
    if (model_.combo() >= 5U) {
        const uint32_t pulse = static_cast<uint32_t>((animation_time_us_ / 100000U) % 6U);
        const uint32_t opacity = 105U + (pulse <= 3U ? pulse : 6U - pulse) * 25U;
        const Rgb flame{251U, 191U, 36U};
        const micropixel::Color edge = AsColor(MixRgb(flame, theme.board, opacity));
        constexpr int32_t size = static_cast<int32_t>(kColumns) * kCellPitch;
        SetSolidInstance(scenery_batch_, update, kFlameOffset + 0U, {kBoardX - 3, kBoardY - 3, size + 6, 3}, edge);
        SetSolidInstance(scenery_batch_, update, kFlameOffset + 1U, {kBoardX - 3, kBoardY + size, size + 6, 3}, edge);
        SetSolidInstance(scenery_batch_, update, kFlameOffset + 2U, {kBoardX - 3, kBoardY, 3, size}, edge);
        SetSolidInstance(scenery_batch_, update, kFlameOffset + 3U, {kBoardX + size, kBoardY, 3, size}, edge);
    } else {
        for (uint16_t index = 0U; index < 4U; ++index) {
            scenery_batch_.SetInstanceVisible(update, kFlameOffset + index, false);
        }
    }
    uint16_t trail_output = 0U;
    for (uint32_t index = 0U; index < kTrailPoolSize && trail_output < kTrailPoolSize; ++index) {
        const Trail& trail = trails_[index];
        if (!trail.active) {
            continue;
        }
        const uint32_t opacity = 102U * (400000U - trail.age_us) / 400000U;
        SetSolidInstance(scenery_batch_, update, kTrailOffset + trail_output++,
                         CellRect(trail.cell, 5, kBoardX, kBoardY),
                         AsColor(MixRgb(theme.accent, theme.board, opacity)));
    }
    for (uint16_t index = trail_output; index < kTrailPoolSize; ++index) {
        scenery_batch_.SetInstanceVisible(update, kTrailOffset + index, false);
    }
    const uint16_t obstacle_count = model_.obstacle_count() < 5U ? model_.obstacle_count() : 5U;
    for (uint16_t index = 0U; index < obstacle_count; ++index) {
        const micropixel::Rect rock = CellRect(model_.obstacles()[index], 2, kBoardX, kBoardY);
        SetSolidInstance(scenery_batch_, update, kObstacleOffset + index, rock, micropixel::Color::Rgb(82U, 82U, 82U));
        SetSolidInstance(scenery_batch_, update, kObstacleDetailOffset + index, {rock.x + 4, rock.y + 3, 8, 3},
                         micropixel::Color::Rgb(126U, 126U, 126U));
    }
    for (uint16_t index = obstacle_count; index < 5U; ++index) {
        scenery_batch_.SetInstanceVisible(update, kObstacleOffset + index, false);
        scenery_batch_.SetInstanceVisible(update, kObstacleDetailOffset + index, false);
    }
}

void SnakeGame::RenderFood(micropixel::SceneUpdate& update) {
    const Food& food = model_.food();
    if (food.cell.x < 0) {
        food_node_.SetVisible(update, false);
        return;
    }
    const uint32_t phase = static_cast<uint32_t>(((animation_time_us_ % kFoodAnimationDurationUs) * kFoodFrameCount) /
                                                 kFoodAnimationDurationUs);
    const int32_t center_x = kBoardX + static_cast<int32_t>(food.cell.x) * kCellPitch + kCellPitch / 2;
    const int32_t center_y = kBoardY + static_cast<int32_t>(food.cell.y) * kCellPitch + kCellPitch / 2;
    food_node_.SetTexture(update, food_sheets_[static_cast<uint32_t>(food.type)]);
    food_node_.SetSource(update,
                         {static_cast<int32_t>((phase % kSpriteSheetColumns) * kFoodSpriteCellSize),
                          static_cast<int32_t>((phase / kSpriteSheetColumns) * kFoodSpriteCellSize),
                          static_cast<int32_t>(kFoodSpriteCellSize), static_cast<int32_t>(kFoodSpriteCellSize)});
    food_node_.SetDestination(
        update, {center_x - static_cast<int32_t>(kFoodSpriteCellSize / 2U),
                 center_y - static_cast<int32_t>(kFoodSpriteCellSize / 2U), static_cast<int32_t>(kFoodSpriteCellSize),
                 static_cast<int32_t>(kFoodSpriteCellSize)});
    food_node_.SetVisible(update, true);
}

void SnakeGame::RenderSnake(micropixel::SceneUpdate& update, const Theme& theme) {
    const uint32_t length = model_.length();
    const Rgb accent = model_.invincible() ? Rgb{34U, 211U, 238U} : theme.accent;
    micropixel::Assert(body_slot_length_ == length, "snake: body ring length drifted");
    for (uint32_t slot = 0U; slot < length; ++slot) {
        const uint32_t index = (slot + length - body_slot_head_) % length;
        if (index == 0U) {
            snake_batch_.SetInstanceVisible(update, static_cast<uint16_t>(slot), false);
            continue;
        }
        const uint32_t band = (index * 8U) / length;
        const int32_t inset = band < 3U ? 1 : (band < 7U ? 2 : 3);
        const uint32_t opacity = 255U - (band * 190U) / 8U;
        SetSolidInstance(snake_batch_, update, static_cast<uint16_t>(slot),
                         InterpolatedSlotRect(slot, index, inset, kBoardX, kBoardY),
                         AsColor(MixRgb(accent, theme.board, opacity)));
    }
    for (uint16_t slot = static_cast<uint16_t>(length); slot < kMaxLength; ++slot) {
        snake_batch_.SetInstanceVisible(update, slot, false);
    }
    const micropixel::Rect head = InterpolatedSlotRect(body_slot_head_, 0U, -2, kBoardX, kBoardY);
    SetSolidInstance(snake_batch_, update, kSnakeHeadInstance, head, AsColor(accent));
    int32_t first_x = head.x + 6;
    int32_t first_y = head.y + 4;
    int32_t second_x = head.x + 19;
    int32_t second_y = head.y + 4;
    if (model_.direction() == Direction::kRight) {
        first_x = second_x = head.x + 20;
        first_y = head.y + 6;
        second_y = head.y + 19;
    } else if (model_.direction() == Direction::kDown) {
        first_y = second_y = head.y + 20;
        first_x = head.x + 6;
        second_x = head.x + 19;
    } else if (model_.direction() == Direction::kLeft) {
        first_x = second_x = head.x + 4;
        first_y = head.y + 6;
        second_y = head.y + 19;
    }
    SetSolidInstance(snake_batch_, update, kSnakeFirstEyeInstance, {first_x, first_y, 4, 5},
                     micropixel::Color::Black());
    SetSolidInstance(snake_batch_, update, kSnakeSecondEyeInstance, {second_x, second_y, 4, 5},
                     micropixel::Color::Black());
}

void SnakeGame::RenderFoodBurst(micropixel::SceneUpdate& update) {
    if (burst_remaining_us_ == 0U) {
        burst_node_.SetVisible(update, false);
        return;
    }
    const uint64_t elapsed = kBurstDurationUs - burst_remaining_us_;
    uint32_t display_phase = static_cast<uint32_t>(elapsed * kBurstDisplayPhaseCount / kBurstDurationUs);
    display_phase = display_phase >= kBurstDisplayPhaseCount ? kBurstDisplayPhaseCount - 1U : display_phase;
    const uint32_t frame_index = display_phase * (kBurstFrameCount - 1U) / (kBurstDisplayPhaseCount - 1U);
    const uint32_t type_index = static_cast<uint32_t>(burst_type_);
    const snake_assets::AtlasFrame& frame = snake_assets::burst_atlases[type_index].frames[frame_index];
    int32_t canvas_x = kBoardX + static_cast<int32_t>(burst_cell_.x) * kCellPitch + kCellPitch / 2 -
                       static_cast<int32_t>(snake_assets::burst_canvas_width) / 2;
    int32_t canvas_y = kBoardY + static_cast<int32_t>(burst_cell_.y) * kCellPitch + kCellPitch / 2 -
                       static_cast<int32_t>(snake_assets::burst_canvas_height) / 2;
    canvas_x = snake::gamekit::ClampCanvasOrigin(canvas_x, static_cast<int32_t>(snake_assets::burst_canvas_width),
                                                 kScreenWidth);
    canvas_y = snake::gamekit::ClampCanvasOrigin(canvas_y, static_cast<int32_t>(snake_assets::burst_canvas_height),
                                                 kScreenHeight);
    burst_node_.SetTexture(update, burst_sheets_[type_index]);
    burst_node_.SetSource(update, {frame.x, frame.y, frame.width, frame.height});
    burst_node_.SetDestination(update,
                               {canvas_x + frame.canvas_x, canvas_y + frame.canvas_y, frame.width, frame.height});
    burst_node_.SetVisible(update, true);
}

void SnakeGame::RenderParticles(micropixel::SceneUpdate& update, const Theme& theme) {
    for (uint16_t index = 0U; index < kParticlePoolSize; ++index) {
        const Particle& particle = particles_[index];
        if (!particle.active) {
            particle_batch_.SetInstanceVisible(update, index, false);
            continue;
        }
        const uint32_t progress = particle.age_us * 256U / particle.duration_us;
        const uint32_t opacity = 255U - (progress > 255U ? 255U : progress);
        const int32_t x = kBoardX + static_cast<int32_t>(particle.origin.x) * kCellPitch + kCellPitch / 2 +
                          particle.dx * static_cast<int32_t>(progress) / 256;
        const int32_t y = kBoardY + static_cast<int32_t>(particle.origin.y) * kCellPitch + kCellPitch / 2 +
                          particle.dy * static_cast<int32_t>(progress) / 256;
        int32_t size = static_cast<int32_t>(particle.size) * static_cast<int32_t>(256U - progress) / 256;
        size = size < 2 ? 2 : size;
        const micropixel::Rect rect = micropixel::Rect{x - size / 2, y - size / 2, size, size}.intersection(kBoardClip);
        if (rect.empty()) {
            particle_batch_.SetInstanceVisible(update, index, false);
        } else {
            SetSolidInstance(particle_batch_, update, index, rect,
                             AsColor(MixRgb(particle.color, theme.board, opacity)));
        }
    }
}

void SnakeGame::RenderFlash(micropixel::SceneUpdate& update, const Theme& theme) {
    if (screen_ == Screen::kGameOver || flash_remaining_us_ == 0U || flash_duration_us_ == 0U) {
        for (uint16_t index = 0U; index < 4U; ++index) {
            flash_batch_.SetInstanceVisible(update, index, false);
        }
        return;
    }
    const uint32_t opacity = static_cast<uint32_t>(flash_remaining_us_ * 190U / flash_duration_us_);
    const int32_t dx = ShakeX();
    const int32_t dy = ShakeY();
    const auto directional = [opacity](bool active) { return active ? opacity : opacity * 2U / 5U; };
    constexpr int32_t size = static_cast<int32_t>(kColumns) * kCellPitch;
    SetSolidInstance(flash_batch_, update, 0U, {kBoardX, kBoardY, size, 6},
                     AsColor(MixRgb(flash_color_, theme.board, directional(dy <= 0))));
    SetSolidInstance(flash_batch_, update, 1U, {kBoardX, kBoardY + size - 6, size, 6},
                     AsColor(MixRgb(flash_color_, theme.board, directional(dy >= 0))));
    SetSolidInstance(flash_batch_, update, 2U, {kBoardX, kBoardY + 6, 6, size - 12},
                     AsColor(MixRgb(flash_color_, theme.board, directional(dx <= 0))));
    SetSolidInstance(flash_batch_, update, 3U, {kBoardX + size - 6, kBoardY + 6, 6, size - 12},
                     AsColor(MixRgb(flash_color_, theme.board, directional(dx >= 0))));
}

void SnakeGame::RenderOverlay(micropixel::SceneUpdate& update, const Theme& theme) {
    const bool overlay_visible = screen_ != Screen::kPlaying;
    overlay_node_.SetVisible(update, overlay_visible);
    overlay_node_.SetColor(
        update, screen_ == Screen::kGameOver ? micropixel::Color::Rgb(69U, 10U, 10U) : micropixel::Color::Black());
    overlay_node_.SetOpacity(update, kOverlayOpacity);

    const bool button_visible = screen_ == Screen::kMenu || screen_ == Screen::kPaused || screen_ == Screen::kGameOver;
    button_node_.SetVisible(update, button_visible);
    if (button_visible) {
        const bool restart = screen_ == Screen::kGameOver;
        button_node_.SetTexture(update, restart ? restart_button_texture_ : start_button_texture_);
        button_node_.SetDestination(update, restart ? kRestartButtonRect : kStartButtonRect);
        button_node_.SetSource(update, {0, 0, kActionButtonWidth, kActionButtonHeight});
        button_node_.SetOpacity(update, screen_button_.pressed() ? 160U : 255U);
    }

    const bool popups_visible = screen_ == Screen::kPlaying && level_banner_us_ == 0U;
    for (uint16_t index = 0U; index < kPopupPoolSize; ++index) {
        const Popup& popup = popups_[index];
        if (!popups_visible || !popup.active) {
            popup_labels_[index].SetVisible(update, false);
            continue;
        }
        const uint32_t progress = popup.age_us * 256U / 800000U;
        const uint32_t opacity = 255U - (progress > 255U ? 255U : progress);
        Line text;
        text.Append("+");
        text.AppendUint(popup.points);
        SetLabel(
            popup_labels_[index], update,
            {kBoardX + static_cast<int32_t>(popup.cell.x) * kCellPitch,
             kBoardY + static_cast<int32_t>(popup.cell.y) * kCellPitch - static_cast<int32_t>(progress * 50U / 256U)},
            text.c_str(), AsColor(MixRgb(popup.color, theme.board, opacity)), popup.font);
    }
    uint16_t slot = 0U;
    const auto centered = [&](int32_t x, int32_t y, const char* text, micropixel::Color color,
                              micropixel::SystemFont font) {
        SetLabel(overlay_labels_[slot++], update, {x, y}, text, color, font);
    };
    if (screen_ == Screen::kMenu || screen_ == Screen::kPaused) {
        centered(360, ActionButtonTextY(kStartButtonRect),
                 strings_.Get(screen_ == Screen::kMenu ? snake_strings::Id::kActionStart
                                                       : snake_strings::Id::kActionContinue),
                 micropixel::Color::Black(), kActionButtonFont);
    } else if (screen_ == Screen::kGameOver) {
        centered(360, 268 + kGameOverContentOffsetY, strings_.Get(snake_strings::Id::kGameOverTitle),
                 micropixel::Color::Rgb(244U, 63U, 94U), micropixel::SystemFont::kTitle);
        Line score;
        score.AppendPadded4(model_.score());
        centered(360, 320 + kGameOverContentOffsetY, score.c_str(), micropixel::Color::White(),
                 micropixel::SystemFont::kLarge);
        constexpr int32_t centers[] = {250, 360, 470};
        centered(centers[0], 356 + kGameOverContentOffsetY, strings_.Get(snake_strings::Id::kLabelFood),
                 micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
        centered(centers[1], 356 + kGameOverContentOffsetY, strings_.Get(snake_strings::Id::kLabelMaxCombo),
                 micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
        centered(centers[2], 356 + kGameOverContentOffsetY, strings_.Get(snake_strings::Id::kLabelLevel),
                 micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
        Line food;
        food.AppendUint(model_.food_eaten());
        centered(centers[0], 390 + kGameOverContentOffsetY, food.c_str(), micropixel::Color::White(),
                 micropixel::SystemFont::kMedium);
        Line combo;
        combo.Append("x");
        combo.AppendUint(model_.max_combo());
        centered(centers[1], 390 + kGameOverContentOffsetY, combo.c_str(), micropixel::Color::White(),
                 micropixel::SystemFont::kMedium);
        Line level;
        level.AppendUint(model_.level());
        centered(centers[2], 390 + kGameOverContentOffsetY, level.c_str(), micropixel::Color::White(),
                 micropixel::SystemFont::kMedium);
        centered(360, ActionButtonTextY(kRestartButtonRect), strings_.Get(snake_strings::Id::kActionRestart),
                 micropixel::Color::Rgb(69U, 10U, 10U), kActionButtonFont);
    } else if (level_banner_us_ != 0U) {
        centered(360, 315, strings_.Get(snake_strings::Id::kUpgradeTitle), AsColor(theme.text),
                 micropixel::SystemFont::kLarge);
        Line reached;
        reached.Append(strings_.Get(snake_strings::Id::kLabelLevelShort));
        reached.AppendUint(model_.level());
        reached.Append(strings_.Get(snake_strings::Id::kUpgradeReachedSuffix));
        centered(360, 370, reached.c_str(), micropixel::Color::White(), micropixel::SystemFont::kMedium);
    }
    while (slot < 9U) {
        overlay_labels_[slot++].SetVisible(update, false);
    }
}

void SnakeGame::RenderHud(micropixel::SceneUpdate& update, const Theme& theme) {
    const int32_t safe_right = static_cast<int32_t>(renderer_info_.safe_area_insets().right);
    if (screen_ == Screen::kPlaying && model_.combo() > 1U) {
        const uint64_t duration = model_.combo_duration_us();
        uint32_t width =
            duration == 0U ? 1U : static_cast<uint32_t>((model_.combo_remaining_us() * kComboBarWidth) / duration);
        width = width == 0U ? 1U : (width > kComboBarWidth ? kComboBarWidth : width);
        SetSolidInstance(combo_batch_, update, 0U,
                         {kComboBarX - safe_right, kComboBarY, static_cast<int32_t>(kComboBarWidth), 4},
                         micropixel::Color::Rgb(38U, 38U, 38U));
        SetSolidInstance(combo_batch_, update, 1U,
                         {kComboBarX - safe_right, kComboBarY, static_cast<int32_t>(width), 4},
                         model_.combo() > 3U ? micropixel::Color::Rgb(251U, 191U, 36U) : micropixel::Color::White());
    } else {
        combo_batch_.SetInstanceVisible(update, 0U, false);
        combo_batch_.SetInstanceVisible(update, 1U, false);
    }

    SetLabel(header_labels_[0], update, SafeTitlePosition(kTitlePosition, renderer_info_),
             strings_.Get(snake_strings::Id::kAppTitle), AsColor(theme.text), micropixel::SystemFont::kTitle);
    Line level;
    level.Append(strings_.Get(snake_strings::Id::kLabelLevelShort));
    level.AppendUint(model_.level());
    const bool level_visible = screen_ != Screen::kPlaying || (!model_.invincible() && model_.combo() <= 1U);
    SetLabel(header_labels_[1], update, ShiftLeftOfSafeRight(kLevelPosition, renderer_info_), level.c_str(),
             AsColor(theme.text), micropixel::SystemFont::kSmall, level_visible);
    SetLabel(header_labels_[2], update, ShiftLeftOfSafeRight(kScoreLabelPosition, renderer_info_),
             strings_.Get(snake_strings::Id::kLabelScore), micropixel::Color::Rgb(115U, 115U, 115U),
             micropixel::SystemFont::kSmall);
    Line score;
    score.AppendPadded4(model_.score());
    SetLabel(header_labels_[3], update, ShiftLeftOfSafeRight(kScoreValuePosition, renderer_info_), score.c_str(),
             micropixel::Color::White(), micropixel::SystemFont::kLarge);
    SetLabel(header_labels_[4], update, ShiftLeftOfSafeRight(kBestLabelPosition, renderer_info_),
             strings_.Get(snake_strings::Id::kLabelBest), micropixel::Color::Rgb(115U, 115U, 115U),
             micropixel::SystemFont::kSmall);
    Line best;
    best.AppendPadded4(best_score_);
    SetLabel(header_labels_[5], update, ShiftLeftOfSafeRight(kBestValuePosition, renderer_info_), best.c_str(),
             AsColor(theme.text), micropixel::SystemFont::kLarge);
    Line status;
    if (screen_ == Screen::kPlaying && model_.invincible()) {
        status.Append(strings_.Get(snake_strings::Id::kStatusShieldPrefix));
        status.AppendUint(static_cast<uint32_t>((model_.invincible_remaining_us() + 999999U) / 1000000U));
        status.Append(strings_.Get(snake_strings::Id::kStatusSecondsSuffix));
        if (model_.combo() > 1U) {
            status.Append("  x");
            status.AppendUint(model_.combo());
        }
    } else if (screen_ == Screen::kPlaying && model_.combo() > 1U) {
        status.Append(strings_.Get(snake_strings::Id::kStatusComboPrefix));
        status.AppendUint(model_.combo());
    } else {
        status.Append(" ");
    }
    SetLabel(header_labels_[6], update, ShiftLeftOfSafeRight(kStatusPosition, renderer_info_), status.c_str(),
             model_.invincible()   ? micropixel::Color::Rgb(34U, 211U, 238U)
             : model_.combo() > 1U ? micropixel::Color::Rgb(251U, 191U, 36U)
                                   : micropixel::Color::Rgb(5U, 5U, 5U),
             micropixel::SystemFont::kSmall);
}

void SnakeGame::ResetBodySlotMapping() {
    body_slot_head_ = 0U;
    body_slot_length_ = model_.length();
    for (uint32_t slot = 0U; slot < body_slot_length_; ++slot) {
        body_slot_previous_[slot] = model_.body()[slot];
    }
}

void SnakeGame::AdvanceBodySlotMapping(Cell previous_head, uint32_t previous_length) {
    const uint32_t length = model_.length();
    if (length == 0U || length != previous_length || body_slot_length_ != previous_length) {
        ResetBodySlotMapping();
        if (length != 0U) {
            body_slot_previous_[body_slot_head_] = previous_head;
        }
        return;
    }
    body_slot_head_ = (body_slot_head_ + length - 1U) % length;
    for (uint32_t slot = 0U; slot < length; ++slot) {
        const uint32_t index = (slot + length - body_slot_head_) % length;
        body_slot_previous_[slot] = model_.body()[index];
    }
    body_slot_previous_[body_slot_head_] = previous_head;
}

}  // namespace snake
