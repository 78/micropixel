#include "apps/snake/snake_game.hpp"

namespace snake {

void SnakeGame::Render() {
    const uint32_t maximum = renderer_info_.max_draw_operations();
    const Theme& theme = ThemeForLevel(model_.level());
    const int32_t board_x = kBoardX;
    const int32_t board_y = kBoardY;
    micropixel::Assert(board_texture_.valid(), "snake: board texture missing");

    constexpr uint32_t kMinimumBodySlots = 13U;
    constexpr uint32_t kFixedMandatoryRects = 13U;
    const uint32_t retained_rect_slots = RetainedRectSlotsForDrawBudget(maximum);
    micropixel::Assert(retained_rect_slots >= kMinimumBodySlots + kFixedMandatoryRects,
                       "snake: Host renderer command budget too small");

    uint32_t body_slots = model_.length() < kMinimumBodySlots ? kMinimumBodySlots : model_.length();
    if (body_slots + kFixedMandatoryRects > retained_rect_slots) {
        body_slots = retained_rect_slots - kFixedMandatoryRects;
    }
    uint32_t optional = retained_rect_slots - body_slots - kFixedMandatoryRects;
    // Motion feedback is part of the core Snake look, so claim four trail
    // samples before purely decorative food/obstacle detail.
    const uint32_t trail_slots = TakeSlots(optional, kVisibleTrailSlots);
    const uint32_t obstacle_detail_slots = TakeSlots(optional, 5U);
    const uint32_t flash_slots = TakeSlots(optional, 4U);
    const uint32_t flame_slots = TakeSlots(optional, 4U);
    const uint32_t particle_slots = TakeSlots(optional, kParticlePoolSize);
    const uint32_t food_detail_slots = TakeSlots(optional, 4U);

    micropixel::Frame commands = renderer_.BeginFrame();
    commands.Clear(micropixel::Color::Rgb(5U, 5U, 5U));

    constexpr int32_t kPadding = 8;
    constexpr micropixel::Rect kBoardClip{
        kBoardX - kPadding,
        kBoardY - kPadding,
        static_cast<int32_t>(kColumns) * kCellPitch + kPadding * 2,
        static_cast<int32_t>(kRows) * kCellPitch + kPadding * 2,
    };
    const bool shake_active = shake_remaining_us_ != 0U && shake_capture_delay_frames_ == 0U;
    const micropixel::Point translation{shake_active ? ShakeX() : 0, shake_active ? ShakeY() : 0};
    commands.Save();
    commands.SetClipRect(kBoardClip);
    commands.Translate(translation);
    commands.DrawTexture(micropixel::Point{kBoardX, kBoardY}, board_texture_);

    // Keep all but the final two runtime-budgeted rectangle slots below
    // the cached surface. The final two remain the header combo bar while
    // playing, but join the surface when reused as menu/pause buttons.
    const uint32_t board_rect_start = commands.draw_operation_count();
    RenderComboFlame(commands, kBoardX, kBoardY, theme, flame_slots);
    RenderTrails(commands, board_x, board_y, theme, trail_slots);
    RenderFood(commands, model_.food(), board_x, board_y, food_detail_slots);
    RenderObstacles(commands, board_x, board_y, obstacle_detail_slots);
    RenderSnake(commands, board_x, board_y, theme, body_slots);
    RenderFoodBurst(commands, board_x, board_y);
    RenderParticles(commands, board_x, board_y, theme, particle_slots);
    RenderOverlayRect(commands, kBoardX, kBoardY, theme);
    RenderFlash(commands, kBoardX, kBoardY, theme, flash_slots);
    while (commands.draw_operation_count() - board_rect_start < retained_rect_slots - 2U) {
        AppendPlaceholderRect(commands);
    }
    micropixel::Assert(commands.draw_operation_count() - board_rect_start == retained_rect_slots - 2U,
                       "snake: retained board rectangle slot count drifted");

    if (screen_ == Screen::kPlaying) {
        commands.Restore();
        RenderComboBar(commands);
        commands.Save();
        commands.SetClipRect(kBoardClip);
        commands.Translate(translation);
    } else {
        RenderComboBar(commands);
    }

    const uint32_t board_text_start = commands.draw_operation_count();
    RenderPopups(commands, board_x, board_y, theme);
    RenderOverlayTexts(commands, theme);
    micropixel::Assert(commands.draw_operation_count() - board_text_start == 19U,
                       "snake: retained board text slot count drifted");
    commands.Restore();

    const uint32_t header_text_start = commands.draw_operation_count();
    RenderHeaderTexts(commands, theme);
    micropixel::Assert(commands.draw_operation_count() - header_text_start == 8U,
                       "snake: retained header text slot count drifted");

    micropixel::Assert(commands.draw_operation_count() == 2U + retained_rect_slots + kRetainedTextSlots &&
                           commands.draw_operation_count() <= maximum,
                       "snake: retained surface scene slot budget drifted");
    micropixel::Assert(commands.Present().has_value(), "snake: frame present failed");
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
    return micropixel::Rect{board_x + static_cast<int32_t>(cell.x) * kCellPitch + inset,
                            board_y + static_cast<int32_t>(cell.y) * kCellPitch + inset, kCellPitch - inset * 2,
                            kCellPitch - inset * 2};
}

[[nodiscard]] uint32_t SnakeGame::MotionFractionQ8() const {
    if (screen_ != Screen::kPlaying) {
        return 256U;
    }
    uint64_t period = MovementPeriodUs();
    uint64_t fraction = period == 0U ? 256U : (accumulated_us_ * 256U) / period;
    return fraction > 256U ? 256U : static_cast<uint32_t>(fraction);
}

micropixel::Rect SnakeGame::InterpolatedSlotRect(uint32_t slot, uint32_t index, int32_t inset, int32_t board_x,
                                                 int32_t board_y) const {
    Cell current = model_.body()[index];
    Cell previous = slot < body_slot_length_ ? body_slot_previous_[slot] : current;
    if (AbsoluteValue(static_cast<int32_t>(current.x) - previous.x) > 1 ||
        AbsoluteValue(static_cast<int32_t>(current.y) - previous.y) > 1) {
        previous = current;
    }
    uint32_t fraction = MotionFractionQ8();
    int32_t previous_x = static_cast<int32_t>(previous.x) * kCellPitch;
    int32_t previous_y = static_cast<int32_t>(previous.y) * kCellPitch;
    int32_t current_x = static_cast<int32_t>(current.x) * kCellPitch;
    int32_t current_y = static_cast<int32_t>(current.y) * kCellPitch;
    return micropixel::Rect{
        board_x + InterpolateAxis(previous_x, current_x, fraction) + inset,
        board_y + InterpolateAxis(previous_y, current_y, fraction) + inset,
        kCellPitch - inset * 2,
        kCellPitch - inset * 2,
    };
}

void SnakeGame::AppendPlaceholderRect(micropixel::Frame& commands) const {
    commands.FillRect(micropixel::Rect{kBoardX - 8, kBoardY - 8, 1, 1}, micropixel::Color::Rgb(5U, 5U, 5U));
}

void SnakeGame::AppendPlaceholderText(micropixel::Frame& commands) {
    commands.DrawText(micropixel::Point{kBoardX, kBoardY}, " ", micropixel::Color::Rgb(5U, 5U, 5U),
                      micropixel::SystemFont::kMedium);
}

void SnakeGame::FillClippedRect(micropixel::Frame& commands, micropixel::Rect rect, micropixel::Color color) const {
    micropixel::Rect clipped{};
    if (ClipRectToScreen(rect, clipped)) {
        constexpr int32_t kPadding = 8;
        constexpr micropixel::Rect kBoardClip{kBoardX - kPadding, kBoardY - kPadding,
                                              static_cast<int32_t>(kColumns) * kCellPitch + kPadding * 2,
                                              static_cast<int32_t>(kRows) * kCellPitch + kPadding * 2};
        clipped = clipped.intersection(kBoardClip);
        if (!clipped.empty()) {
            commands.FillRect(clipped, color);
            return;
        }
    }
    AppendPlaceholderRect(commands);
}

void SnakeGame::RenderComboBar(micropixel::Frame& commands) const {
    if (screen_ == Screen::kMenu) {
        micropixel::Assert(start_button_texture_.valid(), "snake: start button texture missing");
        micropixel::ui::DrawTextureButton(commands, screen_button_, start_button_texture_);
        return;
    }
    if (screen_ == Screen::kPaused) {
        micropixel::Assert(start_button_texture_.valid(), "snake: continue button texture missing");
        micropixel::ui::DrawTextureButton(commands, screen_button_, start_button_texture_);
        return;
    }
    if (screen_ == Screen::kGameOver) {
        micropixel::Assert(restart_button_texture_.valid(), "snake: restart button texture missing");
        micropixel::ui::DrawTextureButton(commands, screen_button_, restart_button_texture_);
        return;
    }
    const bool visible = model_.combo() > 1U;
    const uint64_t duration_us = model_.combo_duration_us();
    uint32_t width = duration_us == 0U ? 0U : static_cast<uint32_t>((model_.combo_remaining_us() * 96U) / duration_us);
    width = width > 96U ? 96U : width;
    width = width == 0U ? 1U : width;
    commands.FillRect(micropixel::Rect{380, 54, 96, 4},
                      visible ? micropixel::Color::Rgb(38U, 38U, 38U) : micropixel::Color::Rgb(5U, 5U, 5U));
    commands.FillRect(micropixel::Rect{380, 54, static_cast<int32_t>(width), 4},
                      !visible              ? micropixel::Color::Rgb(5U, 5U, 5U)
                      : model_.combo() > 3U ? micropixel::Color::Rgb(251U, 191U, 36U)
                                            : micropixel::Color::White());
}

void SnakeGame::RenderComboFlame(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                                 uint32_t slots) const {
    uint32_t pulse = static_cast<uint32_t>((animation_time_us_ / 100000U) % 6U);
    uint32_t opacity = 105U + (pulse <= 3U ? pulse : 6U - pulse) * 25U;
    Rgb flame = model_.combo() >= 5U ? Rgb{251U, 191U, 36U} : theme.accent;
    Rgb edge = MixRgb(flame, theme.board, opacity);
    constexpr int32_t kSize = static_cast<int32_t>(kColumns) * kCellPitch;
    for (uint32_t index = 0U; index < slots; ++index) {
        if (model_.combo() < 5U || index >= 4U) {
            AppendPlaceholderRect(commands);
        } else if (index == 0U) {
            commands.FillRect(micropixel::Rect{board_x - 3, board_y - 3, kSize + 6, 3}, AsColor(edge));
        } else if (index == 1U) {
            commands.FillRect(micropixel::Rect{board_x - 3, board_y + kSize, kSize + 6, 3}, AsColor(edge));
        } else if (index == 2U) {
            commands.FillRect(micropixel::Rect{board_x - 3, board_y, 3, kSize}, AsColor(edge));
        } else {
            commands.FillRect(micropixel::Rect{board_x + kSize, board_y, 3, kSize}, AsColor(edge));
        }
    }
}

void SnakeGame::RenderTrails(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                             uint32_t slots) const {
    uint32_t emitted = 0U;
    // The producer cycles through all twelve storage entries. Scan the entire
    // pool so a smaller retained-command allowance does not make the trail
    // disappear whenever the producer cursor advances beyond the visible
    // prefix.
    for (uint32_t index = 0U; index < kTrailPoolSize && emitted < slots; ++index) {
        const Trail& trail = trails_[index];
        if (!trail.active) {
            continue;
        }
        uint32_t opacity = 102U * (400000U - trail.age_us) / 400000U;
        commands.FillRect(CellRect(trail.cell, 5, board_x, board_y),
                          AsColor(MixRgb(theme.accent, theme.board, opacity)));
        ++emitted;
    }
    while (emitted < slots) {
        AppendPlaceholderRect(commands);
        ++emitted;
    }
}

void SnakeGame::RenderFood(micropixel::Frame& commands, const Food& food, int32_t board_x, int32_t board_y,
                           uint32_t detail_slots) const {
    if (food.cell.x < 0) {
        for (uint32_t index = 0U; index < detail_slots + 1U; ++index) {
            AppendPlaceholderRect(commands);
        }
        return;
    }
    const uint32_t phase = static_cast<uint32_t>(((animation_time_us_ % kFoodAnimationDurationUs) * kFoodFrameCount) /
                                                 kFoodAnimationDurationUs);
    const int32_t center_x = board_x + static_cast<int32_t>(food.cell.x) * kCellPitch + kCellPitch / 2;
    const int32_t center_y = board_y + static_cast<int32_t>(food.cell.y) * kCellPitch + kCellPitch / 2;
    const micropixel::Texture& sheet = food_sheets_[static_cast<uint32_t>(food.type)];
    micropixel::Assert(sheet.valid(), "snake: food sprite sheet missing");
    const micropixel::Rect source{static_cast<int32_t>((phase % kSpriteSheetColumns) * kFoodSpriteCellSize),
                                  static_cast<int32_t>((phase / kSpriteSheetColumns) * kFoodSpriteCellSize),
                                  static_cast<int32_t>(kFoodSpriteCellSize), static_cast<int32_t>(kFoodSpriteCellSize)};
    commands.DrawTexture(micropixel::Point{center_x - static_cast<int32_t>(kFoodSpriteCellSize / 2U),
                                           center_y - static_cast<int32_t>(kFoodSpriteCellSize / 2U)},
                         sheet, source);
    for (uint32_t index = 0U; index < detail_slots; ++index) {
        AppendPlaceholderRect(commands);
    }
}

void SnakeGame::RenderObstacles(micropixel::Frame& commands, int32_t board_x, int32_t board_y,
                                uint32_t detail_slots) const {
    for (uint32_t index = 0U; index < 5U; ++index) {
        if (index >= model_.obstacle_count()) {
            AppendPlaceholderRect(commands);
            continue;
        }
        commands.FillRect(CellRect(model_.obstacles()[index], 2, board_x, board_y),
                          micropixel::Color::Rgb(82U, 82U, 82U));
    }
    for (uint32_t index = 0U; index < detail_slots; ++index) {
        if (index >= model_.obstacle_count()) {
            AppendPlaceholderRect(commands);
            continue;
        }
        micropixel::Rect rock = CellRect(model_.obstacles()[index], 2, board_x, board_y);
        commands.FillRect(micropixel::Rect{rock.x + 4, rock.y + 3, 8, 3}, micropixel::Color::Rgb(126U, 126U, 126U));
    }
}

void SnakeGame::RenderSnake(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                            uint32_t body_slots) const {
    uint32_t length = model_.length();
    Rgb accent = model_.invincible() ? Rgb{34U, 211U, 238U} : theme.accent;
    micropixel::Assert(body_slot_length_ == length, "snake: retained body ring length drifted");
    // Slots form a ring. On each Move only the old tail slot is recycled
    // as the new head; all interior LVGL rectangles keep their position.
    for (uint32_t slot = 0U; slot < body_slots; ++slot) {
        if (slot >= length) {
            AppendPlaceholderRect(commands);
            continue;
        }
        const uint32_t index = (slot + length - body_slot_head_) % length;
        if (index == 0U) {
            // The ring still owns the logical head position, but its
            // physical slot rotates. Keep it hidden and draw one stable
            // head object After Every body slot so the body can never
            // cover the face because of retained-object z order.
            AppendPlaceholderRect(commands);
            continue;
        }
        const uint32_t band = (index * 8U) / length;
        // HTML scales body segments from 0.90 to 0.75 cell. Integer P4
        // geometry uses 23/21/19 px (0.92/0.84/0.76 cell) in 8 stable
        // bands, avoiding a full-tail dirty cascade when length changes.
        int32_t inset = band < 3U ? 1 : (band < 7U ? 2 : 3);
        uint32_t opacity = 255U - (band * 190U) / 8U;
        commands.FillRect(InterpolatedSlotRect(slot, index, inset, board_x, board_y),
                          AsColor(MixRgb(accent, theme.board, opacity)));
    }

    // Original HTML head scale is 1.15 cell: 25 * 1.15 = 28.75 px.
    // A -2 px inset produces a centered 29x29 head on the integer raster.
    micropixel::Rect head = InterpolatedSlotRect(body_slot_head_, 0U, -2, board_x, board_y);
    commands.FillRect(head, AsColor(accent));
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
    commands.FillRect(micropixel::Rect{first_x, first_y, 4, 5}, micropixel::Color::Black());
    commands.FillRect(micropixel::Rect{second_x, second_y, 4, 5}, micropixel::Color::Black());
}

void SnakeGame::RenderFoodBurst(micropixel::Frame& commands, int32_t board_x, int32_t board_y) const {
    if (burst_remaining_us_ == 0U) {
        AppendPlaceholderRect(commands);
        return;
    }
    const uint64_t elapsed = kBurstDurationUs - burst_remaining_us_;
    uint32_t display_phase = static_cast<uint32_t>(elapsed * kBurstDisplayPhaseCount / kBurstDurationUs);
    display_phase = display_phase >= kBurstDisplayPhaseCount ? kBurstDisplayPhaseCount - 1U : display_phase;
    const uint32_t frame_index = display_phase * (kBurstFrameCount - 1U) / (kBurstDisplayPhaseCount - 1U);
    const uint32_t type_index = static_cast<uint32_t>(burst_type_);
    const micropixel::Texture& sheet = burst_sheets_[type_index];
    micropixel::Assert(sheet.valid(), "snake: food burst sheet missing");
    int32_t center_x = board_x + static_cast<int32_t>(burst_cell_.x) * kCellPitch + kCellPitch / 2;
    int32_t center_y = board_y + static_cast<int32_t>(burst_cell_.y) * kCellPitch + kCellPitch / 2;
    constexpr int32_t kCanvasWidth = static_cast<int32_t>(snake_assets::burst_canvas_width);
    constexpr int32_t kCanvasHeight = static_cast<int32_t>(snake_assets::burst_canvas_height);
    int32_t canvas_x = center_x - kCanvasWidth / 2;
    int32_t canvas_y = center_y - kCanvasHeight / 2;
    canvas_x = canvas_x < 0 ? 0 : (canvas_x > kScreenWidth - kCanvasWidth ? kScreenWidth - kCanvasWidth : canvas_x);
    canvas_y = canvas_y < 0 ? 0 : (canvas_y > kScreenHeight - kCanvasHeight ? kScreenHeight - kCanvasHeight : canvas_y);
    const snake_assets::AtlasFrame& frame = snake_assets::burst_atlases[type_index].frames[frame_index];
    const int32_t x = canvas_x + frame.canvas_x;
    const int32_t y = canvas_y + frame.canvas_y;
    const micropixel::Rect source{frame.x, frame.y, frame.width, frame.height};
    commands.DrawTexture(micropixel::Point{x, y}, sheet, source);
}

void SnakeGame::RenderParticles(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                                uint32_t slots) const {
    for (uint32_t index = 0U; index < slots; ++index) {
        const Particle& particle = particles_[index];
        if (!particle.active) {
            AppendPlaceholderRect(commands);
            continue;
        }
        uint32_t progress = particle.age_us * 256U / particle.duration_us;
        uint32_t opacity = 255U - (progress > 255U ? 255U : progress);
        int32_t x = board_x + static_cast<int32_t>(particle.origin.x) * kCellPitch + kCellPitch / 2 +
                    particle.dx * static_cast<int32_t>(progress) / 256;
        int32_t y = board_y + static_cast<int32_t>(particle.origin.y) * kCellPitch + kCellPitch / 2 +
                    particle.dy * static_cast<int32_t>(progress) / 256;
        int32_t size = static_cast<int32_t>(particle.size) * static_cast<int32_t>(256U - progress) / 256;
        size = size < 2 ? 2 : size;
        FillClippedRect(commands, micropixel::Rect{x - size / 2, y - size / 2, size, size},
                        AsColor(MixRgb(particle.color, theme.board, opacity)));
    }
}

void SnakeGame::RenderFlash(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                            uint32_t slots) const {
    // Game Over has no board-wide color overlay. Preserve the same retained
    // slots without painting fake corner masks over the board artwork.
    if (screen_ == Screen::kGameOver) {
        for (uint32_t index = 0U; index < slots; ++index) {
            AppendPlaceholderRect(commands);
        }
        return;
    }
    uint32_t flash_opacity =
        flash_duration_us_ == 0U ? 0U : static_cast<uint32_t>(flash_remaining_us_ * 190U / flash_duration_us_);
    uint32_t shake_opacity = 0U;
    uint32_t opacity = flash_opacity > shake_opacity ? flash_opacity : shake_opacity;
    const Rgb source = flash_opacity != 0U ? flash_color_ : theme.accent;
    const int32_t dx = ShakeX();
    const int32_t dy = ShakeY();
    auto directional_opacity = [opacity](bool active) { return active ? opacity : opacity * 2U / 5U; };
    const Rgb top = MixRgb(source, theme.board, directional_opacity(dy <= 0));
    const Rgb bottom = MixRgb(source, theme.board, directional_opacity(dy >= 0));
    const Rgb left = MixRgb(source, theme.board, directional_opacity(dx <= 0));
    const Rgb right = MixRgb(source, theme.board, directional_opacity(dx >= 0));
    constexpr int32_t kSize = static_cast<int32_t>(kColumns) * kCellPitch;
    for (uint32_t index = 0U; index < slots; ++index) {
        if (opacity == 0U || index >= 4U) {
            AppendPlaceholderRect(commands);
        } else if (index == 0U) {
            commands.FillRect(micropixel::Rect{board_x, board_y, kSize, 6}, AsColor(top));
        } else if (index == 1U) {
            commands.FillRect(micropixel::Rect{board_x, board_y + kSize - 6, kSize, 6}, AsColor(bottom));
        } else if (index == 2U) {
            commands.FillRect(micropixel::Rect{board_x, board_y + 6, 6, kSize - 12}, AsColor(left));
        } else {
            commands.FillRect(micropixel::Rect{board_x + kSize - 6, board_y + 6, 6, kSize - 12}, AsColor(right));
        }
    }
}

void SnakeGame::RenderOverlayRect(micropixel::Frame& commands, int32_t board_x, int32_t board_y,
                                  const Theme& theme) const {
    constexpr int32_t kBoardSize = static_cast<int32_t>(kColumns) * kCellPitch;
    micropixel::Color color = micropixel::Color::Black();
    const uint8_t opacity = screen_ == Screen::kPlaying ? 0U : kOverlayOpacity;
    if (screen_ == Screen::kGameOver) {
        color = micropixel::Color::Rgb(69U, 10U, 10U);
    }
    (void)theme;
    commands.FillRect(micropixel::Rect{board_x, board_y, kBoardSize, kBoardSize}, color, opacity);
}

void SnakeGame::RenderHeaderTexts(micropixel::Frame& commands, const Theme& theme) const {
    commands.DrawText(micropixel::Point{24, 8}, strings_.Get(snake_strings::Id::kAppTitle), AsColor(theme.text),
                      micropixel::SystemFont::kTitle);
    commands.DrawText(micropixel::Point{24, 47}, strings_.Get(snake_strings::Id::kBrandEdition),
                      micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
    Line level;
    level.Append(strings_.Get(snake_strings::Id::kLabelLevelShort));
    level.AppendUint(model_.level());
    commands.DrawText(micropixel::Point{172, 47}, level.c_str(), AsColor(theme.text), micropixel::SystemFont::kSmall);
    commands.DrawText(micropixel::Point{545, 12}, strings_.Get(snake_strings::Id::kLabelScore),
                      micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
    Line score;
    score.AppendPadded4(model_.score());
    commands.DrawText(micropixel::Point{545, 33}, score.c_str(), micropixel::Color::White(),
                      micropixel::SystemFont::kLarge);
    commands.DrawText(micropixel::Point{645, 12}, strings_.Get(snake_strings::Id::kLabelBest),
                      micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
    Line best;
    best.AppendPadded4(best_score_);
    commands.DrawText(micropixel::Point{635, 33}, best.c_str(), AsColor(theme.text), micropixel::SystemFont::kLarge);
    Line status;
    if (screen_ == Screen::kPlaying && model_.invincible()) {
        status.Append(strings_.Get(snake_strings::Id::kStatusShieldPrefix));
        const uint32_t seconds = static_cast<uint32_t>((model_.invincible_remaining_us() + 999999U) / 1000000U);
        status.AppendUint(seconds);
        status.Append(strings_.Get(snake_strings::Id::kStatusSecondsSuffix));
        if (model_.combo() > 1U) {
            status.Append("  x");
            status.AppendUint(model_.combo());
        }
    } else if (screen_ == Screen::kPlaying && model_.combo() > 1U) {
        status.Append(strings_.Get(snake_strings::Id::kStatusComboPrefix));
        status.AppendUint(model_.combo());
        status.Append("  ");
    } else {
        status.Append(" ");
    }
    // Match the Score/Best value row instead of leaving Combo on a lower,
    // visually disconnected baseline.
    commands.DrawText(micropixel::Point{380, 33}, status.c_str(),
                      model_.invincible()   ? micropixel::Color::Rgb(34U, 211U, 238U)
                      : model_.combo() > 1U ? micropixel::Color::Rgb(251U, 191U, 36U)
                                            : micropixel::Color::Rgb(5U, 5U, 5U),
                      micropixel::SystemFont::kSmall);
}

void SnakeGame::RenderPopups(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme) const {
    bool visible = screen_ == Screen::kPlaying && level_banner_us_ == 0U;
    for (uint32_t index = 0U; index < kPopupPoolSize; ++index) {
        const Popup& popup = popups_[index];
        if (!visible || !popup.active) {
            AppendPlaceholderText(commands);
            continue;
        }
        uint32_t progress = popup.age_us * 256U / 800000U;
        uint32_t opacity = 255U - (progress > 255U ? 255U : progress);
        Line label;
        label.Append("+");
        label.AppendUint(popup.points);
        commands.DrawText(micropixel::Point{board_x + static_cast<int32_t>(popup.cell.x) * kCellPitch,
                                            board_y + static_cast<int32_t>(popup.cell.y) * kCellPitch -
                                                static_cast<int32_t>(progress * 50U / 256U)},
                          label.c_str(), AsColor(MixRgb(popup.color, theme.board, opacity)), popup.font);
    }
}

void SnakeGame::RenderOverlayTexts(micropixel::Frame& commands, const Theme& theme) const {
    uint32_t used = 0U;
    if (screen_ == Screen::kMenu) {
        commands.DrawTextCentered(360, ActionButtonTextY(kStartButtonRect),
                                  strings_.Get(snake_strings::Id::kActionStart), micropixel::Color::Black(),
                                  kActionButtonFont);
        used = 1U;
    } else if (screen_ == Screen::kPaused) {
        commands.DrawTextCentered(360, ActionButtonTextY(kStartButtonRect),
                                  strings_.Get(snake_strings::Id::kActionContinue), micropixel::Color::Black(),
                                  kActionButtonFont);
        used = 1U;
    } else if (screen_ == Screen::kGameOver) {
        commands.DrawTextCentered(360, 268, strings_.Get(snake_strings::Id::kGameOverTitle),
                                  micropixel::Color::Rgb(244U, 63U, 94U), micropixel::SystemFont::kTitle);
        Line score;
        score.AppendPadded4(model_.score());
        commands.DrawTextCentered(360, 320, score.c_str(), micropixel::Color::White(), micropixel::SystemFont::kLarge);
        constexpr int32_t kFoodCenter = 250;
        constexpr int32_t kComboCenter = 360;
        constexpr int32_t kLevelCenter = 470;
        const micropixel::Color label_color = micropixel::Color::Rgb(115U, 115U, 115U);
        commands.DrawTextCentered(kFoodCenter, 360, strings_.Get(snake_strings::Id::kLabelFood), label_color,
                                  micropixel::SystemFont::kSmall);
        commands.DrawTextCentered(kComboCenter, 360, strings_.Get(snake_strings::Id::kLabelMaxCombo), label_color,
                                  micropixel::SystemFont::kSmall);
        commands.DrawTextCentered(kLevelCenter, 360, strings_.Get(snake_strings::Id::kLabelLevel), label_color,
                                  micropixel::SystemFont::kSmall);
        Line food;
        food.AppendUint(model_.food_eaten());
        commands.DrawTextCentered(kFoodCenter, 382, food.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium);
        Line combo;
        combo.Append("x");
        combo.AppendUint(model_.max_combo());
        commands.DrawTextCentered(kComboCenter, 382, combo.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium);
        Line level;
        level.AppendUint(model_.level());
        commands.DrawTextCentered(kLevelCenter, 382, level.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium);
        commands.DrawTextCentered(360, ActionButtonTextY(kRestartButtonRect),
                                  strings_.Get(snake_strings::Id::kActionRestart),
                                  micropixel::Color::Rgb(69U, 10U, 10U), kActionButtonFont);
        used = 9U;
    } else if (level_banner_us_ != 0U) {
        commands.DrawTextCentered(360, 315, strings_.Get(snake_strings::Id::kUpgradeTitle), AsColor(theme.text),
                                  micropixel::SystemFont::kLarge);
        Line reached;
        reached.Append(strings_.Get(snake_strings::Id::kLabelLevelShort));
        reached.AppendUint(model_.level());
        reached.Append(strings_.Get(snake_strings::Id::kUpgradeReachedSuffix));
        commands.DrawTextCentered(360, 370, reached.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium);
        used = 2U;
    }
    while (used++ < 9U) {
        AppendPlaceholderText(commands);
    }
}

void SnakeGame::SnapshotBody() {
    previous_length_ = model_.length();
    for (uint32_t index = 0U; index < previous_length_; ++index) {
        previous_body_[index] = model_.body()[index];
    }
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
        // Growth and poison change the retained ring topology, but the head
        // still needs to animate across the Move that caused that change.
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
    // The recycled tail slot animates from the old head into the new head;
    // Every other slot is already exactly at its new logical segment cell.
    body_slot_previous_[body_slot_head_] = previous_head;
}

}  // namespace snake
