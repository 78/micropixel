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
constexpr micropixel::Point kTitlePosition{47, 8};
constexpr micropixel::Point kLevelPosition{460, 12};
constexpr micropixel::Point kScoreLabelPosition{545, 12};
constexpr micropixel::Point kScoreValuePosition{545, 33};
constexpr micropixel::Point kBestLabelPosition{645, 12};
constexpr micropixel::Point kBestValuePosition{635, 33};
constexpr int32_t kStatusX = 428;
constexpr int32_t kComboBarX = 380;
constexpr int32_t kHudEdgePadding = 12;
constexpr int32_t kGameOverContentOffsetY = -32;

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

void SetSolid(micropixel::SpriteBatch& batch, micropixel::SceneUpdate& update, uint16_t id, micropixel::Rect rect,
              micropixel::Color color, bool visible = true) {
    batch.SetInstance(update, id, {.destination = rect, .color = color, .opacity = 255U, .visible = visible});
}

}  // namespace

void BlocksGame::InitializePlayfieldSurfaces() {
    static_assert(kBoardRows % kPlayfieldSurfaceCount == 0U);
    for (uint32_t index = 0U; index < kPlayfieldSurfaceCount; ++index) {
        auto result = renderer_.CreateStreamingTexture({static_cast<uint32_t>(kPlayfieldWidth), kSurfaceHeight},
                                                       micropixel::PixelFormat::kBgr888);
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
    const uint32_t offset = (y * static_cast<uint32_t>(kCellPitch) + x) * 3U;
    cell_pixels_[offset] = color.blue;
    cell_pixels_[offset + 1U] = color.green;
    cell_pixels_[offset + 2U] = color.red;
}

void BlocksGame::RasterizeCell(uint32_t column, uint32_t row, uint8_t visual) {
    const uint32_t origin_x = column * static_cast<uint32_t>(kCellPitch);
    const uint32_t origin_y = row * static_cast<uint32_t>(kCellPitch);
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
                if (inner &&
                    ((global_x != 0 && global_x % kCellPitch == 0) || (global_y != 0 && global_y % kCellPitch == 0))) {
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
                                           cell_pixels_, sizeof(cell_pixels_), static_cast<uint32_t>(kCellPitch) * 3U)
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
    micropixel::Assert(board_texture_.valid() && start_button_texture_.valid() && restart_button_texture_.valid(),
                       "blocks: retained scene textures missing");
    layer_ = scene_.CreateLayer(
        {.clip = {0, 0, static_cast<int32_t>(renderer_info_.width()), static_cast<int32_t>(renderer_info_.height())}});
    board_node_ = scene_.CreateSprite(board_texture_, {kBoardX, kBoardY, kBoardAssetWidth, kBoardAssetHeight},
                                      {0, 0, kBoardAssetWidth, kBoardAssetHeight}, layer_);
    for (uint32_t index = 0U; index < kPlayfieldSurfaceCount; ++index) {
        const int32_t y = kBoardY + static_cast<int32_t>(index * kSurfaceHeight);
        playfield_nodes_[index] = scene_.CreateSurfaceNode(
            playfield_surfaces_[index], {kBoardX, y, kPlayfieldWidth, static_cast<int32_t>(kSurfaceHeight)},
            {0, 0, kPlayfieldWidth, static_cast<int32_t>(kSurfaceHeight)}, layer_);
    }
    mini_piece_batch_ = scene_.CreateSpriteBatch(24U, layer_);
    status_batch_ = scene_.CreateSpriteBatch(2U, layer_);
    for (micropixel::LabelNode& label : header_labels_) {
        label = scene_.CreateLabel({0, 0}, " ", micropixel::Color::White(), micropixel::SystemFont::kSmall, layer_);
    }
    for (micropixel::LabelNode& label : sidebar_labels_) {
        label = scene_.CreateLabel({0, 0}, " ", micropixel::Color::White(), micropixel::SystemFont::kSmall, layer_);
    }
    status_label_ =
        scene_.CreateLabel({428, 31}, " ", micropixel::Color::White(), micropixel::SystemFont::kSmall, layer_, true);
    overlay_node_ = scene_.CreateShape({kBoardX, kBoardY, kBoardAssetWidth, kBoardAssetHeight},
                                       micropixel::Color::Black(), layer_, kOverlayOpacity);
    button_node_ = scene_.CreateSprite(start_button_texture_, kStartButtonRect,
                                       {0, 0, kActionButtonWidth, kActionButtonHeight}, layer_);
    for (micropixel::LabelNode& label : overlay_labels_) {
        label = scene_.CreateLabel({kScreenCenterX, 300}, " ", micropixel::Color::White(),
                                   micropixel::SystemFont::kLarge, layer_, true);
    }
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
    SetLabel(header_labels_[0], update, SafeTitlePosition(kTitlePosition, renderer_info_),
             strings_.Get(blocks_strings::Id::kAppTitle), AsColor(theme.text), micropixel::SystemFont::kTitle);
    Line level;
    level.Append(strings_.Get(blocks_strings::Id::kLabelLevelShort));
    level.AppendUint(model_.level());
    SetLabel(header_labels_[1], update, ShiftLeftOfSafeRight(kLevelPosition, renderer_info_), level.c_str(),
             AsColor(theme.text), micropixel::SystemFont::kSmall);
    SetLabel(header_labels_[2], update, ShiftLeftOfSafeRight(kScoreLabelPosition, renderer_info_),
             strings_.Get(blocks_strings::Id::kLabelScore), micropixel::Color::Rgb(115U, 115U, 115U),
             micropixel::SystemFont::kSmall);
    Line score;
    score.AppendPadded4(model_.score());
    SetLabel(header_labels_[3], update, ShiftLeftOfSafeRight(kScoreValuePosition, renderer_info_), score.c_str(),
             micropixel::Color::White(), micropixel::SystemFont::kLarge);
    SetLabel(header_labels_[4], update, ShiftLeftOfSafeRight(kBestLabelPosition, renderer_info_),
             strings_.Get(blocks_strings::Id::kLabelBest), micropixel::Color::Rgb(115U, 115U, 115U),
             micropixel::SystemFont::kSmall);
    Line best;
    best.AppendPadded4(best_score_ > model_.score() ? best_score_ : model_.score());
    SetLabel(header_labels_[5], update, ShiftLeftOfSafeRight(kBestValuePosition, renderer_info_), best.c_str(),
             AsColor(kBestScoreColor), micropixel::SystemFont::kLarge);
}

void BlocksGame::RenderSidebar(micropixel::SceneUpdate& update, const Theme& theme) {
    const int32_t left = kBoardX + kSidebarX + 18;
    const micropixel::Color muted = micropixel::Color::Rgb(115U, 115U, 115U);
    SetLabel(sidebar_labels_[0], update, {left, kBoardY + 14}, strings_.Get(blocks_strings::Id::kLabelHold),
             model_.hold_available() ? AsColor(theme.text) : muted, micropixel::SystemFont::kMedium);
    SetLabel(sidebar_labels_[1], update, {left, kBoardY + 156}, strings_.Get(blocks_strings::Id::kLabelNext),
             AsColor(theme.text), micropixel::SystemFont::kMedium);
    Line level;
    level.Append(strings_.Get(blocks_strings::Id::kLabelLevelPrefix));
    level.AppendUint(model_.level());
    SetLabel(sidebar_labels_[2], update, {left, kBoardY + 310}, level.c_str(), AsColor(theme.text),
             micropixel::SystemFont::kLarge);
    Line lines;
    lines.Append(strings_.Get(blocks_strings::Id::kLabelLinesPrefix));
    lines.AppendUint(model_.lines());
    SetLabel(sidebar_labels_[3], update, {left, kBoardY + 426}, lines.c_str(), AsColor(theme.text),
             micropixel::SystemFont::kLarge);
    SetLabel(sidebar_labels_[4], update, {left, kBoardY + 548}, strings_.Get(blocks_strings::Id::kHintTapRotate), muted,
             micropixel::SystemFont::kSmall);
    SetLabel(sidebar_labels_[5], update, {left, kBoardY + 574}, strings_.Get(blocks_strings::Id::kHintSwipeAnywhere),
             muted, micropixel::SystemFont::kSmall);
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
        const int32_t safe_right = static_cast<int32_t>(renderer_info_.safe_area_insets().right);
        SetSolid(status_batch_, update, 0U, {kComboBarX - safe_right, 54, 96, 4},
                 micropixel::Color::Rgb(38U, 38U, 38U));
        const uint32_t width = model_.combo() > 5U ? 96U : model_.combo() * 16U;
        SetSolid(status_batch_, update, 1U, {kComboBarX - safe_right, 54, static_cast<int32_t>(width), 4},
                 micropixel::Color::Rgb(251U, 191U, 36U));
        Line combo;
        combo.Append(strings_.Get(blocks_strings::Id::kEffectComboPrefix));
        combo.AppendUint(model_.combo());
        status_label_.SetCentered(update, true);
        SetLabel(status_label_, update, {kStatusX - safe_right, 31}, combo.c_str(),
                 micropixel::Color::Rgb(251U, 191U, 36U), micropixel::SystemFont::kSmall);
    }
}

void BlocksGame::RenderOverlay(micropixel::SceneUpdate& update) {
    for (micropixel::LabelNode& label : overlay_labels_) {
        label.SetVisible(update, false);
    }
    if (screen_ == Screen::kPlaying) {
        overlay_node_.SetVisible(update, false);
        button_node_.SetVisible(update, false);
        return;
    }
    const bool game_over = screen_ == Screen::kGameOver;
    overlay_node_.SetVisible(update, true);
    overlay_node_.SetRect(update, game_over ? kGameOverOverlayRect
                                            : micropixel::Rect{kBoardX, kBoardY, kBoardAssetWidth, kBoardAssetHeight});
    overlay_node_.SetColor(update, game_over ? micropixel::Color::Rgb(69U, 10U, 10U) : micropixel::Color::Black());
    overlay_node_.SetOpacity(update, kOverlayOpacity);
    button_node_.SetVisible(update, true);
    button_node_.SetTexture(update, game_over ? restart_button_texture_ : start_button_texture_);
    button_node_.SetDestination(update, game_over ? kRestartButtonRect : kStartButtonRect);
    button_node_.SetOpacity(update, screen_button_.pressed() ? 160U : 255U);
    uint16_t slot = 0U;
    const auto label = [&](int32_t x, int32_t y, const char* text, micropixel::Color color,
                           micropixel::SystemFont font) {
        SetLabel(overlay_labels_[slot++], update, {x, y}, text, color, font);
    };
    if (!game_over) {
        label(kScreenCenterX, ActionButtonTextY(kStartButtonRect),
              strings_.Get(screen_ == Screen::kMenu ? blocks_strings::Id::kActionStart
                                                    : blocks_strings::Id::kActionContinue),
              micropixel::Color::Black(), kActionButtonFont);
        return;
    }
    label(kScreenCenterX, 260 + kGameOverContentOffsetY, strings_.Get(blocks_strings::Id::kGameOverTitle),
          micropixel::Color::Rgb(244U, 63U, 94U), micropixel::SystemFont::kLarge);
    Line score;
    score.AppendPadded4(model_.score());
    label(kScreenCenterX, 312 + kGameOverContentOffsetY, score.c_str(), micropixel::Color::White(),
          micropixel::SystemFont::kLarge);
    label(kScreenCenterX - 55, 356 + kGameOverContentOffsetY, strings_.Get(blocks_strings::Id::kLabelLines),
          micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
    label(kScreenCenterX + 55, 356 + kGameOverContentOffsetY, strings_.Get(blocks_strings::Id::kLabelLevel),
          micropixel::Color::Rgb(115U, 115U, 115U), micropixel::SystemFont::kSmall);
    Line lines;
    lines.AppendUint(model_.lines());
    label(kScreenCenterX - 55, 390 + kGameOverContentOffsetY, lines.c_str(), micropixel::Color::White(),
          micropixel::SystemFont::kMedium);
    Line level;
    level.AppendUint(model_.level());
    label(kScreenCenterX + 55, 390 + kGameOverContentOffsetY, level.c_str(), micropixel::Color::White(),
          micropixel::SystemFont::kMedium);
    label(kScreenCenterX, ActionButtonTextY(kRestartButtonRect), strings_.Get(blocks_strings::Id::kActionRestart),
          micropixel::Color::Rgb(69U, 10U, 10U), kActionButtonFont);
}

void BlocksGame::Render() {
    for (const micropixel::StreamingTexture& surface : playfield_surfaces_) {
        micropixel::Assert(surface.valid(), "blocks: playfield surface missing");
    }
    InitializeScene();
    SyncPlayfield();
    const Theme& theme = ThemeForLevel(model_.level());
    auto update = scene_.BeginUpdate();
    RenderHeader(update, theme);
    RenderSidebar(update, theme);
    const int32_t center = kBoardX + kSidebarX + kSidebarWidth / 2;
    RenderMiniPiece(update, 0U, model_.held(), center, kBoardY + 54, !model_.hold_available(), model_.has_hold());
    RenderMiniPiece(update, 12U, model_.next(), center, kBoardY + 196, false, true);
    RenderStatusEffect(update, theme);
    RenderOverlay(update);
    micropixel::Assert(update.Present().has_value(), "blocks: scene update failed");
}

}  // namespace blocks
