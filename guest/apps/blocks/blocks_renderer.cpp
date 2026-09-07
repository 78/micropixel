#include "apps/blocks/blocks_game.hpp"
#include "blocks_assets.hpp"

namespace blocks {
namespace {

constexpr uint8_t kGhostVisual = 0x40U;
constexpr uint8_t kActiveVisual = 0x80U;
constexpr uint8_t kFlashVisual = 0xe0U;
// Keep these on stable RGB565 gray levels. The menu's 50% black overlay
// preserves them as neutral 8/16-level grays instead of quantizing individual
// channels differently near black.
constexpr Rgb kBoardBackground{16U, 16U, 16U};
constexpr Rgb kBorderColor{38U, 38U, 38U};
constexpr Rgb kBestScoreColor{196U, 144U, 38U};

uint32_t LogicalStrokeWidth(uint32_t logical_extent, uint32_t physical_extent) {
    if (physical_extent == 0U) {
        return 1U;
    }
    const uint32_t width = (logical_extent + physical_extent - 1U) / physical_extent;
    return width == 0U ? 1U : width;
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

void BlocksGame::SyncPlayfield() {
    InitializeScene();
    const auto result = scene_.Update([&](micropixel::SceneUpdate& update) { UpdatePlayfield(update); });
    micropixel::Assert(result.has_value(), "blocks: playfield scene update failed");
}

void BlocksGame::UpdatePlayfield(micropixel::SceneUpdate& update) {
    for (uint32_t row = 0U; row < kBoardRows; ++row) {
        for (uint32_t column = 0U; column < kBoardColumns; ++column) {
            const uint16_t slot = static_cast<uint16_t>(row * kBoardColumns + column);
            const uint8_t visual = VisualCell(column, row);
            if (visual_cache_valid_ && visual_cells_[slot] == visual) {
                continue;
            }
            const bool flash = (visual & 0xe0U) == kFlashVisual;
            const uint8_t type = visual & 0x0fU;
            if (!flash && (type == 0U || type > kTetrominoCount)) {
                playfield_batch_.SetInstanceVisible(update, slot, false);
            } else {
                const bool ghost = (visual & kGhostVisual) != 0U && (visual & kActiveVisual) == 0U;
                const uint32_t theme = (model_.level() == 0U ? 0U : model_.level() - 1U) % kThemeCount;
                const micropixel::Rect source = flash ? micropixel::Rect{static_cast<int32_t>(visual & 0x07U) * 30,
                                                                         static_cast<int32_t>(4U + theme) * 30, 30, 30}
                                                      : micropixel::Rect{(type - 1) * 30, ghost ? 30 : 0, 30, 30};
                playfield_batch_.SetInstance(
                    update, slot,
                    {.destination = {kBoardX + static_cast<int32_t>(column) * kCellPitch,
                                     kBoardY + static_cast<int32_t>(row) * kCellPitch, kCellPitch, kCellPitch},
                     .source = source});
            }
            visual_cells_[slot] = visual;
        }
    }
    visual_cache_valid_ = true;
}

void BlocksGame::InitializeScene() {
    if (scene_initialized_) {
        return;
    }
    root_container_ = scene_.CreateContainer(
        {.clip = {0, 0, static_cast<int32_t>(kScreenWidth), static_cast<int32_t>(kScreenHeight)},
         .translation = {ContentOffsetX(renderer_info_.width()), ContentOffsetY(renderer_info_.height())}});
    auto atlas = app_.resources().LoadTexture(blocks_assets::playfield_atlas);
    micropixel::Assert(atlas.has_value(), "blocks: playfield atlas load failed");
    playfield_atlas_ = static_cast<micropixel::Texture&&>(atlas.value());
    const uint32_t stroke = LogicalStrokeWidth(renderer_info_.width(), renderer_info_.physical_width());
    auto background = app_.resources().LoadTexture(stroke > 1U ? blocks_assets::playfield_background_thick
                                                               : blocks_assets::playfield_background_thin);
    micropixel::Assert(background.has_value(), "blocks: playfield background load failed");
    playfield_background_ = static_cast<micropixel::Texture&&>(background.value());
    (void)root_container_.CreateSprite(playfield_background_, {kBoardX, kBoardY, kPlayfieldWidth, kPlayfieldHeight},
                                       {0, 0, kPlayfieldWidth, kPlayfieldHeight});
    playfield_batch_ = root_container_.CreateSpriteBatch(playfield_atlas_, kBoardColumns * kBoardRows);
    for (uint32_t index = 0U; index < kSidebarPanelCount; ++index) {
        sidebar_panels_[index] = root_container_.CreateRoundedRect(
            kSidebarPanelRects[index],
            {.fill = AsColor(kBoardBackground), .stroke = AsColor(kBorderColor), .radius = 12, .stroke_width = 3});
    }
    mini_piece_batch_ = root_container_.CreateSpriteBatch(playfield_atlas_, 8U);
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
    for (uint16_t index = 0U; index < 4U; ++index) {
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
    uint16_t output = 0U;
    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            if (!BlocksModel::ShapeOccupied(type, 0U, x, y)) {
                continue;
            }
            const int32_t cell_x = origin_x + static_cast<int32_t>(x) * pitch;
            const int32_t cell_y = origin_y + static_cast<int32_t>(y) * pitch;
            mini_piece_batch_.SetInstance(update, first_instance + output++,
                                          {.destination = {cell_x, cell_y, pitch, pitch},
                                           .source = {static_cast<int32_t>(type) * 30, muted ? 90 : 60, 24, 24}});
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
    InitializeScene();
    const Theme& theme = ThemeForLevel(model_.level());
    auto presented = scene_.Update([&](micropixel::SceneUpdate& update) {
        UpdatePlayfield(update);
        RenderHeader(update, theme);
        RenderSidebar(update, theme);
        const int32_t center = kSidebarPanelRects[0].x + kSidebarPanelRects[0].width / 2;
        RenderMiniPiece(update, 0U, model_.held(), center, kSidebarPanelRects[0].y + 54, !model_.hold_available(),
                        model_.has_hold());
        RenderMiniPiece(update, 4U, model_.next(), center, kSidebarPanelRects[1].y + 54, false, true);
        RenderStatusEffect(update, theme);
        RenderOverlay(update);
    });
    micropixel::Assert(presented.has_value(), "blocks: scene update failed");
}

}  // namespace blocks
