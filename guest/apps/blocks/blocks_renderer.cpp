#include "apps/blocks/blocks_game.hpp"

namespace blocks {
namespace {

constexpr uint32_t kPlayfieldSurfaceCount = 4U;
constexpr uint32_t kRowsPerSurface = kBoardRows / kPlayfieldSurfaceCount;
constexpr uint32_t kSurfaceHeight = kRowsPerSurface * static_cast<uint32_t>(kCellPitch);
constexpr uint8_t kGhostVisual = 0x40U;
constexpr uint8_t kActiveVisual = 0x80U;
constexpr uint8_t kFlashVisual = 0xe0U;
constexpr Rgb kScreenBackground{5U, 5U, 5U};
constexpr Rgb kBoardBackground{10U, 10U, 10U};
constexpr Rgb kGridColor{18U, 18U, 18U};
constexpr Rgb kBorderColor{38U, 38U, 38U};
constexpr Rgb kBestScoreColor{196U, 144U, 38U};

[[nodiscard]] constexpr Rgb MixRgb(Rgb foreground, Rgb background, uint8_t opacity) {
    const uint32_t inverse = 255U - opacity;
    return Rgb{
        static_cast<uint8_t>((foreground.red * opacity + background.red * inverse + 127U) / 255U),
        static_cast<uint8_t>((foreground.green * opacity + background.green * inverse + 127U) / 255U),
        static_cast<uint8_t>((foreground.blue * opacity + background.blue * inverse + 127U) / 255U),
    };
}

[[nodiscard]] constexpr Rgb Lighten(Rgb color, uint8_t opacity) {
    return MixRgb(Rgb{255U, 255U, 255U}, color, opacity);
}

[[nodiscard]] bool InsideRoundedRect(int32_t x, int32_t y, int32_t width, int32_t height, int32_t radius) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return false;
    }
    if (x >= radius && x < width - radius) {
        return true;
    }
    if (y >= radius && y < height - radius) {
        return true;
    }
    const int32_t center_x = x < radius ? radius : width - radius - 1;
    const int32_t center_y = y < radius ? radius : height - radius - 1;
    const int32_t delta_x = x - center_x;
    const int32_t delta_y = y - center_y;
    return delta_x * delta_x + delta_y * delta_y <= radius * radius;
}

[[nodiscard]] bool PieceOccupies(ActivePiece piece, uint32_t column, uint32_t row) {
    for (uint32_t local_y = 0U; local_y < 4U; ++local_y) {
        for (uint32_t local_x = 0U; local_x < 4U; ++local_x) {
            if (!BlocksModel::ShapeOccupied(piece.type, piece.rotation, local_x, local_y)) {
                continue;
            }
            const int32_t piece_x = static_cast<int32_t>(piece.x) + static_cast<int32_t>(local_x);
            const int32_t piece_y = static_cast<int32_t>(piece.y) + static_cast<int32_t>(local_y);
            if (piece_x == static_cast<int32_t>(column) && piece_y == static_cast<int32_t>(row)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

void BlocksGame::InitializePlayfieldSurfaces() {
    static_assert(kBoardRows % kPlayfieldSurfaceCount == 0U, "playfield strips must divide the board rows");
    for (uint32_t index = 0U; index < kPlayfieldSurfaceCount; ++index) {
        auto result = renderer_.CreateStreamingTexture(
            micropixel::Size{static_cast<uint32_t>(kPlayfieldWidth), kSurfaceHeight}, micropixel::PixelFormat::kBgr888);
        micropixel::Assert(result.has_value(), "blocks: streaming texture allocation failed");
        playfield_surfaces_[index] = static_cast<micropixel::StreamingTexture&&>(result.value());
    }
    visual_cache_valid_ = false;
    SyncPlayfield();
    app_.log().Info("blocks: allocated 4 x 300x150 RGB offscreen surfaces in Host PSRAM");
}

uint8_t BlocksGame::VisualCell(uint32_t column, uint32_t row) const {
    if (clear_effect_remaining_us_ != 0U && (clear_rows_mask_ & (1U << row)) != 0U) {
        const uint32_t elapsed = static_cast<uint32_t>(240000U - clear_effect_remaining_us_);
        const uint32_t triangle = elapsed < 120000U ? elapsed : 240000U - elapsed;
        const uint8_t phase = static_cast<uint8_t>((triangle * 7U) / 120000U);
        return static_cast<uint8_t>(kFlashVisual | phase);
    }

    const bool piece_visible = (screen_ == Screen::kPlaying || screen_ == Screen::kPaused) && model_.alive();
    if (piece_visible) {
        const ActivePiece active = model_.active();
        if (PieceOccupies(active, column, row)) {
            return static_cast<uint8_t>(kActiveVisual | (static_cast<uint8_t>(active.type) + 1U));
        }
        ActivePiece ghost = active;
        ghost.y = static_cast<int8_t>(model_.ghost_y());
        if (ghost.y != active.y && PieceOccupies(ghost, column, row)) {
            return static_cast<uint8_t>(kGhostVisual | (static_cast<uint8_t>(ghost.type) + 1U));
        }
    }
    return model_.board_cell(column, row);
}

void BlocksGame::PutCellPixel(uint32_t x, uint32_t y, Rgb color) {
    const uint32_t offset = (y * static_cast<uint32_t>(kCellPitch) + x) * 3U;
    // MICROPIXEL_PIXEL_FORMAT_BGR888 uses LVGL's little-endian B, G, R layout.
    cell_pixels_[offset] = color.blue;
    cell_pixels_[offset + 1U] = color.green;
    cell_pixels_[offset + 2U] = color.red;
}

void BlocksGame::RasterizeCell(uint32_t column, uint32_t row, uint8_t visual) {
    const uint32_t global_origin_x = column * static_cast<uint32_t>(kCellPitch);
    const uint32_t global_origin_y = row * static_cast<uint32_t>(kCellPitch);
    for (uint32_t y = 0U; y < static_cast<uint32_t>(kCellPitch); ++y) {
        for (uint32_t x = 0U; x < static_cast<uint32_t>(kCellPitch); ++x) {
            const int32_t global_x = static_cast<int32_t>(global_origin_x + x);
            const int32_t global_y = static_cast<int32_t>(global_origin_y + y);
            Rgb color = kScreenBackground;
            const bool outer = InsideRoundedRect(global_x, global_y, kPlayfieldWidth, kPlayfieldHeight, 14);
            const bool inner =
                InsideRoundedRect(global_x - 2, global_y - 2, kPlayfieldWidth - 4, kPlayfieldHeight - 4, 12);
            if (outer) {
                color = inner ? kBoardBackground : kBorderColor;
                if (inner &&
                    ((global_x != 0 && global_x % kCellPitch == 0) || (global_y != 0 && global_y % kCellPitch == 0))) {
                    color = kGridColor;
                }
            }
            PutCellPixel(x, y, color);
        }
    }

    if ((visual & 0xe0U) == kFlashVisual) {
        const uint8_t phase = visual & 0x07U;
        const Rgb flash = MixRgb(Rgb{255U, 255U, 255U}, ThemeForLevel(model_.level()).accent,
                                 static_cast<uint8_t>(80U + phase * 22U));
        for (uint32_t y = 1U; y + 1U < static_cast<uint32_t>(kCellPitch); ++y) {
            for (uint32_t x = 1U; x + 1U < static_cast<uint32_t>(kCellPitch); ++x) {
                PutCellPixel(x, y, flash);
            }
        }
        return;
    }

    const uint8_t type_value = visual & 0x0fU;
    if (type_value == 0U || type_value > kTetrominoCount) {
        return;
    }
    const Rgb piece = ColorForTetromino(static_cast<Tetromino>(type_value - 1U));
    const bool ghost = (visual & kGhostVisual) != 0U && (visual & kActiveVisual) == 0U;
    const Rgb fill = ghost ? MixRgb(piece, kBoardBackground, 56U) : piece;
    const Rgb highlight = Lighten(fill, ghost ? 24U : 72U);
    for (uint32_t y = 1U; y <= 28U; ++y) {
        for (uint32_t x = 1U; x <= 28U; ++x) {
            if (!InsideRoundedRect(static_cast<int32_t>(x) - 1, static_cast<int32_t>(y) - 1, 28, 28, 4)) {
                continue;
            }
            if (ghost && x >= 4U && x <= 25U && y >= 4U && y <= 25U) {
                continue;
            }
            PutCellPixel(x, y, y == 3U && x >= 5U && x <= 24U ? highlight : fill);
        }
    }
}

void BlocksGame::SyncPlayfield() {
    micropixel::TextureUpdateBatch update_batch = renderer_.BeginTextureUpdateBatch();
    for (uint32_t row = 0U; row < kBoardRows; ++row) {
        for (uint32_t column = 0U; column < kBoardColumns; ++column) {
            const uint32_t cache_index = row * kBoardColumns + column;
            const uint8_t visual = VisualCell(column, row);
            if (visual_cache_valid_ && visual_cells_[cache_index] == visual) {
                continue;
            }
            RasterizeCell(column, row, visual);
            const uint32_t surface_index = row / kRowsPerSurface;
            const int32_t local_y = static_cast<int32_t>(row % kRowsPerSurface) * kCellPitch;
            micropixel::Assert(playfield_surfaces_[surface_index]
                                   .Update(micropixel::Rect{static_cast<int32_t>(column) * kCellPitch, local_y,
                                                            kCellPitch, kCellPitch},
                                           cell_pixels_, sizeof(cell_pixels_), static_cast<uint32_t>(kCellPitch) * 3U)
                                   .has_value(),
                               "blocks: streaming texture update failed");
            visual_cells_[cache_index] = visual;
        }
    }
    micropixel::Assert(update_batch.Finish().has_value(), "blocks: texture update batch failed");
    visual_cache_valid_ = true;
}

void BlocksGame::RenderMiniPiece(micropixel::ui::ViewportFrame& commands, Tetromino type, int32_t center_x, int32_t top, bool muted,
                                 bool visible) const {
    if (!visible) {
        return;
    }
    constexpr int32_t kMiniPitch = 24;
    uint32_t minimum_x = 4U;
    uint32_t maximum_x = 0U;
    uint32_t minimum_y = 4U;
    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            if (!BlocksModel::ShapeOccupied(type, 0U, x, y)) {
                continue;
            }
            minimum_x = x < minimum_x ? x : minimum_x;
            maximum_x = x > maximum_x ? x : maximum_x;
            minimum_y = y < minimum_y ? y : minimum_y;
        }
    }
    const int32_t piece_width = static_cast<int32_t>(maximum_x - minimum_x + 1U) * kMiniPitch;
    const int32_t origin_x = center_x - piece_width / 2 - static_cast<int32_t>(minimum_x) * kMiniPitch;
    const int32_t origin_y = top - static_cast<int32_t>(minimum_y) * kMiniPitch;
    micropixel::Color color = AsColor(ColorForTetromino(type));
    if (muted) {
        color = micropixel::Color::Mix(color, micropixel::Color::Rgb(10U, 10U, 10U), 96U);
    }
    const micropixel::Color highlight = micropixel::Color::Mix(micropixel::Color::White(), color, 72U);
    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            if (BlocksModel::ShapeOccupied(type, 0U, x, y)) {
                const int32_t cell_x = origin_x + static_cast<int32_t>(x) * kMiniPitch;
                const int32_t cell_y = origin_y + static_cast<int32_t>(y) * kMiniPitch;
                // Match the playfield's compact rounded cell and top highlight using
                // three inexpensive rectangles; the 1 px inset leaves a 2 px seam.
                commands.FillRect(micropixel::Rect{cell_x + 3, cell_y + 1, kMiniPitch - 6, kMiniPitch - 2}, color);
                commands.FillRect(micropixel::Rect{cell_x + 1, cell_y + 3, kMiniPitch - 2, kMiniPitch - 6}, color);
                commands.FillRect(micropixel::Rect{cell_x + 5, cell_y + 3, kMiniPitch - 10, 2}, highlight);
            }
        }
    }
}

void BlocksGame::RenderHeader(micropixel::ui::ViewportFrame& commands, const Theme& theme) const {
    commands.DrawText(micropixel::Point{24, 8}, strings_.Get(blocks_strings::Id::kAppTitle), AsColor(theme.text),
                      micropixel::SystemFont::kTitle);
    commands.DrawText(micropixel::Point{24, 47}, strings_.Get(blocks_strings::Id::kBrandEdition),
                      micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
    Line status;
    status.Append(strings_.Get(blocks_strings::Id::kLabelLevelShort));
    status.AppendUint(model_.level());
    commands.DrawText(micropixel::Point{172, 47}, status.c_str(), AsColor(theme.text), micropixel::SystemFont::kSmall);
    commands.DrawText(micropixel::Point{545, 12}, strings_.Get(blocks_strings::Id::kLabelScore),
                      micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
    Line score;
    score.AppendPadded4(model_.score());
    commands.DrawText(micropixel::Point{545, 33}, score.c_str(), micropixel::Color::White(),
                      micropixel::SystemFont::kLarge);
    commands.DrawText(micropixel::Point{645, 12}, strings_.Get(blocks_strings::Id::kLabelBest),
                      micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
    Line best;
    best.AppendPadded4(best_score_ > model_.score() ? best_score_ : model_.score());
    commands.DrawText(micropixel::Point{635, 33}, best.c_str(), AsColor(kBestScoreColor),
                      micropixel::SystemFont::kLarge);
}

void BlocksGame::RenderSidebar(micropixel::ui::ViewportFrame& commands, const Theme& theme) const {
    const int32_t sidebar_left = kBoardX + kSidebarX;
    const micropixel::Color muted = micropixel::Color::Rgb(115U, 115U, 115U);
    commands.DrawText(micropixel::Point{sidebar_left + 18, kBoardY + 14}, strings_.Get(blocks_strings::Id::kLabelHold),
                      model_.hold_available() ? AsColor(theme.text) : muted, micropixel::SystemFont::kMedium);
    commands.DrawText(micropixel::Point{sidebar_left + 18, kBoardY + 156}, strings_.Get(blocks_strings::Id::kLabelNext),
                      AsColor(theme.text), micropixel::SystemFont::kMedium);
    Line level;
    level.Append(strings_.Get(blocks_strings::Id::kLabelLevelPrefix));
    level.AppendUint(model_.level());
    commands.DrawText(micropixel::Point{sidebar_left + 18, kBoardY + 310}, level.c_str(), AsColor(theme.text),
                      micropixel::SystemFont::kLarge);
    Line lines;
    lines.Append(strings_.Get(blocks_strings::Id::kLabelLinesPrefix));
    lines.AppendUint(model_.lines());
    commands.DrawText(micropixel::Point{sidebar_left + 18, kBoardY + 426}, lines.c_str(), AsColor(theme.text),
                      micropixel::SystemFont::kLarge);
    commands.DrawText(micropixel::Point{sidebar_left + 18, kBoardY + 548},
                      strings_.Get(blocks_strings::Id::kHintTapRotate), muted, micropixel::SystemFont::kSmall);
    commands.DrawText(micropixel::Point{sidebar_left + 18, kBoardY + 574},
                      strings_.Get(blocks_strings::Id::kHintSwipeAnywhere), muted, micropixel::SystemFont::kSmall);
}

void BlocksGame::RenderStatusEffect(micropixel::ui::ViewportFrame& commands, const Theme& theme) const {
    if (clear_effect_remaining_us_ != 0U) {
        int32_t first_row_y = kBoardY + kPlayfieldHeight / 2;
        for (uint32_t row = 0U; row < kBoardRows; ++row) {
            if ((clear_rows_mask_ & (1U << row)) != 0U) {
                first_row_y = kBoardY + static_cast<int32_t>(row) * kCellPitch;
                break;
            }
        }
        Line points;
        points.Append(strings_.Get(blocks_strings::Id::kEffectLineClearPrefix));
        points.AppendUint(clear_points_);
        commands.DrawTextCentered(kBoardX + kPlayfieldWidth / 2, first_row_y - 34, points.c_str(), AsColor(theme.text),
                                  micropixel::SystemFont::kLarge);
        return;
    }
    if (screen_ == Screen::kPlaying && model_.combo() > 1U) {
        commands.FillRect(micropixel::Rect{380, 54, 96, 4}, micropixel::Color::Rgb(38U, 38U, 38U));
        const uint32_t width = model_.combo() > 5U ? 96U : model_.combo() * 16U;
        commands.FillRect(micropixel::Rect{380, 54, static_cast<int32_t>(width), 4},
                          micropixel::Color::Rgb(251U, 191U, 36U));
        Line combo;
        combo.Append(strings_.Get(blocks_strings::Id::kEffectComboPrefix));
        combo.AppendUint(model_.combo());
        commands.DrawTextCentered(428, 31, combo.c_str(), micropixel::Color::Rgb(251U, 191U, 36U),
                                  micropixel::SystemFont::kSmall);
    }
}

void BlocksGame::RenderOverlay(micropixel::ui::ViewportFrame& commands) const {
    if (screen_ == Screen::kPlaying) {
        return;
    }
    micropixel::Color overlay = micropixel::Color::Black();
    uint8_t opacity = kOverlayOpacity;
    const micropixel::Texture* button_texture = &start_button_texture_;
    micropixel::Rect button_bounds = kStartButtonRect;
    micropixel::Rect overlay_bounds{kBoardX, kBoardY, kBoardAssetWidth, kBoardAssetHeight};
    if (screen_ == Screen::kGameOver) {
        overlay = micropixel::Color::Rgb(69U, 10U, 10U);
        button_texture = &restart_button_texture_;
        button_bounds = kRestartButtonRect;
        overlay_bounds = kGameOverOverlayRect;
    }
    commands.FillRect(overlay_bounds, overlay, opacity);
    commands.DrawTexture(micropixel::Point{button_bounds.x, button_bounds.y}, *button_texture,
                         screen_button_.pressed() ? 160U : 255U);

    if (screen_ == Screen::kMenu) {
        commands.DrawTextCentered(kScreenCenterX, ActionButtonTextY(kStartButtonRect),
                                  strings_.Get(blocks_strings::Id::kActionStart), micropixel::Color::Black(),
                                  kActionButtonFont);
    } else if (screen_ == Screen::kPaused) {
        commands.DrawTextCentered(kScreenCenterX, ActionButtonTextY(kStartButtonRect),
                                  strings_.Get(blocks_strings::Id::kActionContinue), micropixel::Color::Black(),
                                  kActionButtonFont);
    } else {
        commands.DrawTextCentered(kScreenCenterX, 260, strings_.Get(blocks_strings::Id::kGameOverTitle),
                                  micropixel::Color::Rgb(244U, 63U, 94U), micropixel::SystemFont::kLarge);
        Line score;
        score.AppendPadded4(model_.score());
        commands.DrawTextCentered(kScreenCenterX, 312, score.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kLarge);
        commands.DrawTextCentered(kScreenCenterX - 55, 360, strings_.Get(blocks_strings::Id::kLabelLines),
                                  micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
        commands.DrawTextCentered(kScreenCenterX + 55, 360, strings_.Get(blocks_strings::Id::kLabelLevel),
                                  micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
        Line lines;
        lines.AppendUint(model_.lines());
        commands.DrawTextCentered(kScreenCenterX - 55, 382, lines.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium);
        Line level;
        level.AppendUint(model_.level());
        commands.DrawTextCentered(kScreenCenterX + 55, 382, level.c_str(), micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium);
        commands.DrawTextCentered(kScreenCenterX, ActionButtonTextY(kRestartButtonRect),
                                  strings_.Get(blocks_strings::Id::kActionRestart),
                                  micropixel::Color::Rgb(69U, 10U, 10U), kActionButtonFont);
    }
}

void BlocksGame::Render() {
    micropixel::Assert(board_texture_.valid(), "blocks: board texture missing");
    for (const auto& surface : playfield_surfaces_) {
        micropixel::Assert(surface.valid(), "blocks: playfield surface missing");
    }
    SyncPlayfield();
    const Theme& theme = ThemeForLevel(model_.level());
    micropixel::ui::ViewportFrame commands{renderer_.BeginFrame(), viewport_};
    commands.Clear(micropixel::Color::Rgb(5U, 5U, 5U));
    commands.DrawTexture(micropixel::Point{kBoardX, kBoardY}, board_texture_);
    for (uint32_t index = 0U; index < kPlayfieldSurfaceCount; ++index) {
        commands.DrawTexture(micropixel::Point{kBoardX, kBoardY + static_cast<int32_t>(index * kSurfaceHeight)},
                             playfield_surfaces_[index]);
    }
    RenderHeader(commands, theme);
    RenderSidebar(commands, theme);
    const int32_t sidebar_center = kBoardX + kSidebarX + kSidebarWidth / 2;
    RenderMiniPiece(commands, model_.held(), sidebar_center, kBoardY + 54, !model_.hold_available(), model_.has_hold());
    RenderMiniPiece(commands, model_.next(), sidebar_center, kBoardY + 196, false, true);
    RenderStatusEffect(commands, theme);
    RenderOverlay(commands);
    micropixel::Assert(commands.Present().has_value(), "blocks: frame present failed");
}

}  // namespace blocks
