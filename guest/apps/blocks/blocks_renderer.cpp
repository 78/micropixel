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
// Keep these on stable RGB565 gray levels. The menu's 50% black overlay
// preserves them as neutral 8/16-level grays instead of quantizing individual
// channels differently near black.
constexpr Rgb kBoardBackground{16U, 16U, 16U};
constexpr Rgb kGridColor{32U, 32U, 32U};
constexpr Rgb kBorderColor{38U, 38U, 38U};
constexpr Rgb kBestScoreColor{196U, 144U, 38U};

uint32_t LogicalStrokeWidth(uint32_t logical_extent, uint32_t physical_extent) {
    if (physical_extent == 0U) {
        return 1U;
    }
    const uint32_t width = (logical_extent + physical_extent - 1U) / physical_extent;
    return width == 0U ? 1U : width;
}

Rgb MixRgb(Rgb foreground, Rgb background, uint8_t opacity) {
    const uint32_t inverse = 255U - opacity;
    return {static_cast<uint8_t>((foreground.red * opacity + background.red * inverse + 127U) / 255U),
            static_cast<uint8_t>((foreground.green * opacity + background.green * inverse + 127U) / 255U),
            static_cast<uint8_t>((foreground.blue * opacity + background.blue * inverse + 127U) / 255U)};
}

bool InsideRoundedRect(int32_t x, int32_t y, int32_t width, int32_t height, int32_t radius) {
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
    const int32_t dx = x - center_x;
    const int32_t dy = y - center_y;
    return dx * dx + dy * dy <= radius * radius;
}

bool PieceOccupies(ActivePiece piece, uint32_t column, uint32_t row) {
    for (uint32_t local_y = 0U; local_y < 4U; ++local_y) {
        for (uint32_t local_x = 0U; local_x < 4U; ++local_x) {
            if (BlocksModel::ShapeOccupied(piece.type, piece.rotation, local_x, local_y) &&
                static_cast<int32_t>(piece.x) + static_cast<int32_t>(local_x) == static_cast<int32_t>(column) &&
                static_cast<int32_t>(piece.y) + static_cast<int32_t>(local_y) == static_cast<int32_t>(row)) {
                return true;
            }
        }
    }
    return false;
}

void SetLabel(micropixel::LabelNode& node, micropixel::SceneUpdate& update, micropixel::Point position,
              const char* text, micropixel::Color color, micropixel::SystemFont font, bool visible = true) {
    node.SetPosition(update, position);
    node.SetText(update, text);
    node.SetColor(update, color);
    node.SetFont(update, font);
    node.SetVisible(update, visible);
}

void SetSolid(micropixel::SpriteBatch& batch, micropixel::SceneUpdate& update, uint16_t id, micropixel::Rect rect,
              micropixel::Color color, bool visible = true) {
    batch.SetInstance(update, id, {.destination = rect, .color = color, .opacity = 255U, .visible = visible});
}

}  // namespace

void BlocksGame::InitializePlayfieldSurfaces() {
    static_assert(kBoardRows % kPlayfieldSurfaceCount == 0U);
    for (uint32_t index = 0U; index < kPlayfieldSurfaceCount; ++index) {
        auto result = renderer_.CreateStreamingTexture({static_cast<uint32_t>(kPlayfieldWidth), kSurfaceHeight},
                                                       micropixel::PixelFormat::kRgb565);
        micropixel::Assert(result.has_value(), "blocks: streaming surface allocation failed");
        playfield_surfaces_[index] = static_cast<micropixel::StreamingTexture&&>(result.value());
    }
    visual_cache_valid_ = false;
    SyncPlayfield();
    app_.log().Info("blocks: four retained StreamingSurfaces ready");
}

uint8_t BlocksGame::VisualCell(uint32_t column, uint32_t row) const {
    if (clear_effect_remaining_us_ != 0U && (clear_rows_mask_ & (1U << row)) != 0U) {
        const uint32_t elapsed = static_cast<uint32_t>(240000U - clear_effect_remaining_us_);
        const uint32_t triangle = elapsed < 120000U ? elapsed : 240000U - elapsed;
        return static_cast<uint8_t>(kFlashVisual | ((triangle * 7U) / 120000U));
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
    const uint16_t packed = static_cast<uint16_t>(((static_cast<uint16_t>(color.red) >> 3U) << 11U) |
                                                  ((static_cast<uint16_t>(color.green) >> 2U) << 5U) |
                                                  (static_cast<uint16_t>(color.blue) >> 3U));
    const uint32_t offset = (y * static_cast<uint32_t>(kCellPitch) + x) * 2U;
    cell_pixels_[offset] = static_cast<uint8_t>(packed);
    cell_pixels_[offset + 1U] = static_cast<uint8_t>(packed >> 8U);
}

void BlocksGame::RasterizeCell(uint32_t column, uint32_t row, uint8_t visual) {
    const uint32_t origin_x = column * static_cast<uint32_t>(kCellPitch);
    const uint32_t origin_y = row * static_cast<uint32_t>(kCellPitch);
    const uint32_t vertical_grid_width = LogicalStrokeWidth(renderer_info_.width(), renderer_info_.physical_width());
    const uint32_t horizontal_grid_width =
        LogicalStrokeWidth(renderer_info_.height(), renderer_info_.physical_height());
    for (uint32_t y = 0U; y < static_cast<uint32_t>(kCellPitch); ++y) {
        for (uint32_t x = 0U; x < static_cast<uint32_t>(kCellPitch); ++x) {
            const int32_t global_x = static_cast<int32_t>(origin_x + x);
            const int32_t global_y = static_cast<int32_t>(origin_y + y);
            Rgb color = kScreenBackground;
            const bool outer = InsideRoundedRect(global_x, global_y, kPlayfieldWidth, kPlayfieldHeight, 14);
            const bool inner =
                InsideRoundedRect(global_x - 2, global_y - 2, kPlayfieldWidth - 4, kPlayfieldHeight - 4, 12);
            if (outer) {
                color = inner ? kBoardBackground : kBorderColor;
                const bool vertical_grid = global_x != 0 && x < vertical_grid_width;
                const bool horizontal_grid = global_y != 0 && y < horizontal_grid_width;
                if (inner && (vertical_grid || horizontal_grid)) {
                    color = kGridColor;
                }
            }
            PutCellPixel(x, y, color);
        }
    }
    if ((visual & 0xe0U) == kFlashVisual) {
        const Rgb flash = MixRgb(Rgb{255U, 255U, 255U}, ThemeForLevel(model_.level()).accent,
                                 static_cast<uint8_t>(80U + (visual & 0x07U) * 22U));
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
    const Rgb highlight = MixRgb(Rgb{255U, 255U, 255U}, fill, ghost ? 24U : 72U);
    for (uint32_t y = 1U; y <= 28U; ++y) {
        for (uint32_t x = 1U; x <= 28U; ++x) {
            if (!InsideRoundedRect(static_cast<int32_t>(x) - 1, static_cast<int32_t>(y) - 1, 28, 28, 4) ||
                (ghost && x >= 4U && x <= 25U && y >= 4U && y <= 25U)) {
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
                                   .Update({static_cast<int32_t>(column) * kCellPitch, local_y, kCellPitch, kCellPitch},
                                           cell_pixels_, sizeof(cell_pixels_), static_cast<uint32_t>(kCellPitch) * 2U)
                                   .has_value(),
                               "blocks: surface cell update failed");
            visual_cells_[cache_index] = visual;
        }
    }
    micropixel::Assert(update_batch.Finish().has_value(), "blocks: surface update transaction failed");
    visual_cache_valid_ = true;
}

void BlocksGame::InitializeScene() {
    if (scene_initialized_) {
        return;
    }
    root_container_ = scene_.CreateContainer(
        {.clip = {0, 0, static_cast<int32_t>(kScreenWidth), static_cast<int32_t>(kScreenHeight)},
         .translation = {ContentOffsetX(renderer_info_.width()), ContentOffsetY(renderer_info_.height())}});
    for (uint32_t index = 0U; index < kPlayfieldSurfaceCount; ++index) {
        const int32_t y = kBoardY + static_cast<int32_t>(index * kSurfaceHeight);
        playfield_nodes_[index] = root_container_.CreateSurfaceNode(
            playfield_surfaces_[index], {kBoardX, y, kPlayfieldWidth, static_cast<int32_t>(kSurfaceHeight)},
            {0, 0, kPlayfieldWidth, static_cast<int32_t>(kSurfaceHeight)});
    }
    for (uint32_t index = 0U; index < kSidebarPanelCount; ++index) {
        sidebar_panels_[index] = root_container_.CreateRoundedRect(
            kSidebarPanelRects[index],
            {.fill = AsColor(kBoardBackground), .stroke = AsColor(kBorderColor), .radius = 12, .stroke_width = 3});
    }
    mini_piece_batch_ = root_container_.CreateSpriteBatch(24U);
    status_batch_ = root_container_.CreateSpriteBatch(2U);
    const int32_t safe_left = static_cast<int32_t>(renderer_info_.safe_area_insets().left);
    const int32_t safe_right = static_cast<int32_t>(renderer_info_.safe_area_insets().right);
    hud_ = root_container_.CreateFlexContainer(
        {.bounds = {safe_left, 0, static_cast<int32_t>(kScreenWidth) - safe_left - safe_right, kBoardY},
         .layout = {.direction = micropixel::ui::FlexDirection::kHorizontal,
                    .padding = {2, 8, 2, 8},
                    .gap_pixels = 8,
                    .distribution = micropixel::ui::FlexDistribution::kSpaceBetween,
                    .alignment = micropixel::ui::FlexAlignment::kCenter}});
    const micropixel::Color muted = micropixel::Color::Rgb(115U, 115U, 115U);
    hud_.CreateLabel(strings_.Get(blocks_strings::Id::kAppTitle),
                     {.color = micropixel::Color::White(), .font = micropixel::SystemFont::kMedium});
    hud_.CreateLabel("LVL 1", {.font = micropixel::SystemFont::kSmall});
    auto& hud_stats = hud_.CreateGridContainer({.rows = 2, .columns = 2, .column_gap = 12});
    hud_stats.CreateLabel(strings_.Get(blocks_strings::Id::kLabelScore),
                          {.color = muted, .font = micropixel::SystemFont::kSmall});
    hud_stats.CreateLabel(strings_.Get(blocks_strings::Id::kLabelBest),
                          {.color = muted, .font = micropixel::SystemFont::kSmall});
    hud_stats.CreateLabel("0000", {.font = micropixel::SystemFont::kSmall});
    hud_stats.CreateLabel("0000", {.color = AsColor(kBestScoreColor), .font = micropixel::SystemFont::kSmall});
    for (micropixel::LabelNode& label : sidebar_labels_) {
        label = root_container_.CreateLabel({0, 0}, " ", micropixel::Color::White(), micropixel::SystemFont::kSmall);
    }
    status_label_ =
        root_container_.CreateLabel({428, 31}, " ", micropixel::Color::White(), micropixel::SystemFont::kSmall, true);
    overlay_node_ = root_container_.CreateShape({kBoardX, kBoardY, kBoardAreaWidth, kBoardAreaHeight},
                                                micropixel::Color::Black(), kOverlayOpacity);
    action_button_ = root_container_.CreateTextButton({.bounds = kStartButtonRect,
                                                       .text = strings_.Get(blocks_strings::Id::kActionStart),
                                                       .style = {.background = micropixel::Color::Rgb(52U, 211U, 153U),
                                                                 .text = micropixel::Color::Black(),
                                                                 .font = kActionButtonFont,
                                                                 .corner_radius = kActionButtonCornerRadius},
                                                       .hit_padding = kActionButtonHitPadding});
    game_over_panel_ =
        root_container_.CreateFlexContainer({.bounds = kGameOverOverlayRect,
                                             .layout = {.direction = micropixel::ui::FlexDirection::kVertical,
                                                        .gap_pixels = 15,
                                                        .distribution = micropixel::ui::FlexDistribution::kCenter,
                                                        .alignment = micropixel::ui::FlexAlignment::kCenter},
                                             .visible = false});
    game_over_panel_.CreateLabel(
        strings_.Get(blocks_strings::Id::kGameOverTitle),
        {.color = micropixel::Color::Rgb(244U, 63U, 94U), .font = micropixel::SystemFont::kLarge});
    game_over_panel_.CreateLabel("0000", {.font = micropixel::SystemFont::kLarge});
    auto& stats = game_over_panel_.CreateGridContainer({.columns = 2, .row_gap = 10});
    stats.CreateLabel(strings_.Get(blocks_strings::Id::kLabelLines),
                      {.color = micropixel::Color::Rgb(115U, 115U, 115U), .font = micropixel::SystemFont::kSmall});
    stats.CreateLabel(strings_.Get(blocks_strings::Id::kLabelLevel),
                      {.color = micropixel::Color::Rgb(115U, 115U, 115U), .font = micropixel::SystemFont::kSmall});
    stats.CreateLabel("0");
    stats.CreateLabel("1");
    game_over_panel_.CreateTextButton({.bounds = {0, 0, kActionButtonWidth, kActionButtonHeight},
                                       .text = strings_.Get(blocks_strings::Id::kActionRestart),
                                       .style = {.background = micropixel::Color::White(),
                                                 .text = micropixel::Color::Rgb(69U, 10U, 10U),
                                                 .font = kActionButtonFont,
                                                 .corner_radius = kActionButtonCornerRadius},
                                       .hit_padding = kActionButtonHitPadding});
    scene_initialized_ = true;
}

void BlocksGame::RenderMiniPiece(micropixel::SceneUpdate& update, uint16_t first_instance, Tetromino type,
                                 int32_t center_x, int32_t top, bool muted, bool visible) {
    for (uint16_t index = 0U; index < 12U; ++index) {
        mini_piece_batch_.SetInstanceVisible(update, first_instance + index, false);
    }
    if (!visible) {
        return;
    }
    constexpr int32_t pitch = 24;
    uint32_t minimum_x = 4U;
    uint32_t maximum_x = 0U;
    uint32_t minimum_y = 4U;
    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            if (BlocksModel::ShapeOccupied(type, 0U, x, y)) {
                minimum_x = x < minimum_x ? x : minimum_x;
                maximum_x = x > maximum_x ? x : maximum_x;
                minimum_y = y < minimum_y ? y : minimum_y;
            }
        }
    }
    const int32_t width = static_cast<int32_t>(maximum_x - minimum_x + 1U) * pitch;
    const int32_t origin_x = center_x - width / 2 - static_cast<int32_t>(minimum_x) * pitch;
    const int32_t origin_y = top - static_cast<int32_t>(minimum_y) * pitch;
    micropixel::Color color = AsColor(ColorForTetromino(type));
    if (muted) {
        color = micropixel::Color::Mix(color, micropixel::Color::Rgb(10U, 10U, 10U), 96U);
    }
    const micropixel::Color highlight = micropixel::Color::Mix(micropixel::Color::White(), color, 72U);
    uint16_t output = 0U;
    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            if (!BlocksModel::ShapeOccupied(type, 0U, x, y)) {
                continue;
            }
            const int32_t cell_x = origin_x + static_cast<int32_t>(x) * pitch;
            const int32_t cell_y = origin_y + static_cast<int32_t>(y) * pitch;
            SetSolid(mini_piece_batch_, update, first_instance + output++,
                     {cell_x + 3, cell_y + 1, pitch - 6, pitch - 2}, color);
            SetSolid(mini_piece_batch_, update, first_instance + output++,
                     {cell_x + 1, cell_y + 3, pitch - 2, pitch - 6}, color);
            SetSolid(mini_piece_batch_, update, first_instance + output++, {cell_x + 5, cell_y + 3, pitch - 10, 2},
                     highlight);
        }
    }
}

void BlocksGame::RenderHeader(micropixel::SceneUpdate& update, const Theme& theme) {
    hud_.label(0U).SetColor(update, AsColor(theme.text));
    Line center;
    const bool combo_active = screen_ == Screen::kPlaying && model_.combo() > 1U;
    if (combo_active) {
        center.Append(strings_.Get(blocks_strings::Id::kEffectComboPrefix));
        center.AppendUint(model_.combo());
    } else {
        center.Append(strings_.Get(blocks_strings::Id::kLabelLevelShort));
        center.AppendUint(model_.level());
    }
    micropixel::Assert(hud_.label(1U).SetText(update, center.c_str()).has_value(), "blocks: HUD center invalid");
    hud_.label(1U).SetColor(update, combo_active ? micropixel::Color::Rgb(251U, 191U, 36U) : AsColor(theme.text));
    Line score;
    score.AppendPadded4(model_.score());
    micropixel::Assert(hud_.grid(0U).SetText(update, 1U, 0U, score.c_str()).has_value(), "blocks: HUD score invalid");
    Line best;
    best.AppendPadded4(best_score_ > model_.score() ? best_score_ : model_.score());
    micropixel::Assert(hud_.grid(0U).SetText(update, 1U, 1U, best.c_str()).has_value(), "blocks: HUD best invalid");
    auto hud_layout = hud_.Layout(update);
    if (!hud_layout.has_value()) {
        Line diagnostic;
        diagnostic.Append("blocks: HUD layout failed: ");
        diagnostic.Append(hud_layout.error().name());
        diagnostic.Append(" title=");
        diagnostic.AppendUint(hud_.label(0U).intrinsic_size().width);
        diagnostic.Append(" center=");
        diagnostic.AppendUint(hud_.label(1U).intrinsic_size().width);
        diagnostic.Append(" stats=");
        diagnostic.AppendUint(hud_.grid(0U).intrinsic_size().width);
        app_.log().Error(diagnostic.c_str());
    }
    micropixel::Assert(hud_layout.has_value(), "blocks: HUD layout failed");
}

void BlocksGame::RenderSidebar(micropixel::SceneUpdate& update, const Theme& theme) {
    const int32_t left = kSidebarPanelRects[0].x + 18;
    const micropixel::Color muted = micropixel::Color::Rgb(115U, 115U, 115U);
    SetLabel(sidebar_labels_[0], update, {left, kSidebarPanelRects[0].y + 12},
             strings_.Get(blocks_strings::Id::kLabelHold), model_.hold_available() ? AsColor(theme.text) : muted,
             micropixel::SystemFont::kSmall);
    SetLabel(sidebar_labels_[1], update, {left, kSidebarPanelRects[1].y + 12},
             strings_.Get(blocks_strings::Id::kLabelNext), AsColor(theme.text), micropixel::SystemFont::kSmall);
    Line level;
    level.Append(strings_.Get(blocks_strings::Id::kLabelLevelPrefix));
    level.AppendUint(model_.level());
    SetLabel(sidebar_labels_[2], update, {left, kSidebarPanelRects[2].y + 32}, level.c_str(), AsColor(theme.text),
             micropixel::SystemFont::kSmall);
    Line lines;
    lines.Append(strings_.Get(blocks_strings::Id::kLabelLinesPrefix));
    lines.AppendUint(model_.lines());
    SetLabel(sidebar_labels_[3], update, {left, kSidebarPanelRects[3].y + 32}, lines.c_str(), AsColor(theme.text),
             micropixel::SystemFont::kSmall);
    SetLabel(sidebar_labels_[4], update, {left, kSidebarPanelRects[4].y + 10},
             strings_.Get(blocks_strings::Id::kHintTapRotate), muted, micropixel::SystemFont::kSmall);
    SetLabel(sidebar_labels_[5], update, {left, kSidebarPanelRects[4].y + 46},
             strings_.Get(blocks_strings::Id::kHintSwipeAnywhere), muted, micropixel::SystemFont::kSmall);
}

void BlocksGame::RenderStatusEffect(micropixel::SceneUpdate& update, const Theme& theme) {
    status_batch_.SetInstanceVisible(update, 0U, false);
    status_batch_.SetInstanceVisible(update, 1U, false);
    status_label_.SetVisible(update, false);
    if (clear_effect_remaining_us_ != 0U) {
        int32_t y = kBoardY + kPlayfieldHeight / 2;
        for (uint32_t row = 0U; row < kBoardRows; ++row) {
            if ((clear_rows_mask_ & (1U << row)) != 0U) {
                y = kBoardY + static_cast<int32_t>(row) * kCellPitch - 34;
                break;
            }
        }
        Line points;
        points.Append(strings_.Get(blocks_strings::Id::kEffectLineClearPrefix));
        points.AppendUint(clear_points_);
        status_label_.SetCentered(update, true);
        SetLabel(status_label_, update, {kBoardX + kPlayfieldWidth / 2, y}, points.c_str(), AsColor(theme.text),
                 micropixel::SystemFont::kLarge);
    } else if (screen_ == Screen::kPlaying && model_.combo() > 1U) {
        const int32_t safe_left = static_cast<int32_t>(renderer_info_.safe_area_insets().left);
        const micropixel::Rect center_bounds = hud_.label(1U).bounds();
        const uint32_t full_width = static_cast<uint32_t>(center_bounds.width);
        const uint32_t width = model_.combo() > 5U ? full_width : (full_width * model_.combo()) / 6U;
        const int32_t bar_x = safe_left + center_bounds.x;
        const int32_t bar_y = center_bounds.y + center_bounds.height - 4;
        SetSolid(status_batch_, update, 0U, {bar_x, bar_y, center_bounds.width, 4},
                 micropixel::Color::Rgb(38U, 38U, 38U));
        SetSolid(status_batch_, update, 1U, {bar_x, bar_y, static_cast<int32_t>(width), 4},
                 micropixel::Color::Rgb(251U, 191U, 36U));
    }
}

void BlocksGame::RenderOverlay(micropixel::SceneUpdate& update) {
    if (screen_ == Screen::kPlaying) {
        overlay_node_.SetVisible(update, false);
        action_button_.SetVisible(update, false);
        game_over_panel_.SetVisible(update, false);
        return;
    }
    const bool game_over = screen_ == Screen::kGameOver;
    overlay_node_.SetVisible(update, true);
    overlay_node_.SetRect(update, game_over ? kGameOverOverlayRect
                                            : micropixel::Rect{kBoardX, kBoardY, kBoardAreaWidth, kBoardAreaHeight});
    overlay_node_.SetColor(update, game_over ? micropixel::Color::Rgb(69U, 10U, 10U) : micropixel::Color::Black());
    overlay_node_.SetOpacity(update, game_over ? 255U : kOverlayOpacity);
    game_over_panel_.SetVisible(update, game_over);
    action_button_.SetVisible(update, !game_over);
    if (game_over) {
        Line score;
        score.AppendPadded4(model_.score());
        micropixel::Assert(game_over_panel_.label(1U).SetText(update, score.c_str()).has_value(),
                           "blocks: game over score invalid");
        Line lines;
        lines.AppendUint(model_.lines());
        micropixel::Assert(game_over_panel_.grid(0U).SetText(update, 1U, 0U, lines.c_str()).has_value(),
                           "blocks: game over lines invalid");
        Line level;
        level.AppendUint(model_.level());
        micropixel::Assert(game_over_panel_.grid(0U).SetText(update, 1U, 1U, level.c_str()).has_value(),
                           "blocks: game over level invalid");
        micropixel::Assert(game_over_panel_.Layout(update).has_value(), "blocks: game over layout failed");
        game_over_panel_.text_button(0U).Sync(update);
        return;
    }
    micropixel::Assert(action_button_.SetBounds(update, kStartButtonRect).has_value(),
                       "blocks: text button bounds invalid");
    micropixel::Assert(
        action_button_
            .SetText(update, strings_.Get(screen_ == Screen::kMenu ? blocks_strings::Id::kActionStart
                                                                   : blocks_strings::Id::kActionContinue))
            .has_value(),
        "blocks: text button text invalid");
}

void BlocksGame::Render() {
    for (const micropixel::StreamingTexture& surface : playfield_surfaces_) {
        micropixel::Assert(surface.valid(), "blocks: playfield surface missing");
    }
    InitializeScene();
    SyncPlayfield();
    const Theme& theme = ThemeForLevel(model_.level());
    auto presented = scene_.Update([&](micropixel::SceneUpdate& update) {
        RenderHeader(update, theme);
        RenderSidebar(update, theme);
        const int32_t center = kSidebarPanelRects[0].x + kSidebarPanelRects[0].width / 2;
        RenderMiniPiece(update, 0U, model_.held(), center, kSidebarPanelRects[0].y + 54, !model_.hold_available(),
                        model_.has_hold());
        RenderMiniPiece(update, 12U, model_.next(), center, kSidebarPanelRects[1].y + 54, false, true);
        RenderStatusEffect(update, theme);
        RenderOverlay(update);
    });
    micropixel::Assert(presented.has_value(), "blocks: scene update failed");
}

}  // namespace blocks
