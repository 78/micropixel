#include "apps/tilt/tilt_game.hpp"

namespace tilt {
namespace {

constexpr uint16_t kStarInstanceOffset = 0U;
constexpr uint16_t kPitInstance = 3U;
constexpr uint16_t kBumperInstance = 4U;
constexpr uint16_t kObjectBatchCapacity = 5U;
constexpr uint16_t kIceTileInstance = 0U;
constexpr uint16_t kAirflowTileInstanceOffset = 1U;
constexpr uint16_t kWallTileInstanceOffset = kAirflowTileInstanceOffset + kMaximumFanCount;
constexpr uint16_t kBoardTileBatchCapacity = kWallTileInstanceOffset + kMaximumVisualWallBlockCount;
constexpr int32_t kWallBlockLength = 20;
constexpr int32_t kMinimumWallBlockLength = 9;

struct SpritePlacement final {
    micropixel::Rect destination{};
    micropixel::Rect source{};
};

uint32_t WallBlockCount(VisualWallFeature wall) {
    const int32_t length = wall.horizontal != 0U ? wall.rect.width : wall.rect.height;
    return length < kMinimumWallBlockLength ? 0U
                                            : static_cast<uint32_t>((length + kWallBlockLength - 1) / kWallBlockLength);
}

WallRect WallBlock(VisualWallFeature wall, uint32_t block_index, uint32_t block_count) {
    const int32_t length = wall.horizontal != 0U ? wall.rect.width : wall.rect.height;
    const int32_t base_length = length / static_cast<int32_t>(block_count);
    const int32_t longer_block_count = length % static_cast<int32_t>(block_count);
    const int32_t index = static_cast<int32_t>(block_index);
    const int32_t offset = index * base_length + (index < longer_block_count ? index : longer_block_count);
    const int32_t block_length = base_length + (index < longer_block_count ? 1 : 0);
    if (wall.horizontal != 0U) {
        return {static_cast<int16_t>(wall.rect.x + offset), wall.rect.y, static_cast<int16_t>(block_length),
                wall.rect.height};
    }
    return {wall.rect.x, static_cast<int16_t>(wall.rect.y + offset), wall.rect.width,
            static_cast<int16_t>(block_length)};
}

micropixel::Rect ClampToViewport(micropixel::Rect rect) {
    const int32_t maximum_x = static_cast<int32_t>(kScreenWidth) - rect.width;
    const int32_t maximum_y = static_cast<int32_t>(kScreenHeight) - rect.height;
    rect.x = rect.x < 0 ? 0 : (rect.x > maximum_x ? maximum_x : rect.x);
    rect.y = rect.y < 0 ? 0 : (rect.y > maximum_y ? maximum_y : rect.y);
    return rect;
}

micropixel::Rect ClipToViewport(micropixel::Rect rect) {
    const int32_t right = rect.x + rect.width;
    const int32_t bottom = rect.y + rect.height;
    const int32_t clipped_left = rect.x < 0 ? 0 : rect.x;
    const int32_t clipped_top = rect.y < 0 ? 0 : rect.y;
    const int32_t clipped_right =
        right > static_cast<int32_t>(kScreenWidth) ? static_cast<int32_t>(kScreenWidth) : right;
    const int32_t clipped_bottom =
        bottom > static_cast<int32_t>(kScreenHeight) ? static_cast<int32_t>(kScreenHeight) : bottom;
    return {clipped_left, clipped_top, clipped_right - clipped_left, clipped_bottom - clipped_top};
}

SpritePlacement ClipSprite(micropixel::Rect destination, micropixel::Rect source, micropixel::Rect clip) {
    const int32_t left = destination.x < clip.x ? clip.x : destination.x;
    const int32_t top = destination.y < clip.y ? clip.y : destination.y;
    const int32_t destination_right = destination.x + destination.width;
    const int32_t destination_bottom = destination.y + destination.height;
    const int32_t clip_right = clip.x + clip.width;
    const int32_t clip_bottom = clip.y + clip.height;
    const int32_t right = destination_right > clip_right ? clip_right : destination_right;
    const int32_t bottom = destination_bottom > clip_bottom ? clip_bottom : destination_bottom;
    if (destination.width <= 0 || destination.height <= 0 || right <= left || bottom <= top) {
        return {{clip.x, clip.y, 1, 1}, source};
    }
    const int32_t source_left = source.x + (left - destination.x) * source.width / destination.width;
    const int32_t source_top = source.y + (top - destination.y) * source.height / destination.height;
    const int32_t source_right = source.x + (right - destination.x) * source.width / destination.width;
    const int32_t source_bottom = source.y + (bottom - destination.y) * source.height / destination.height;
    return {{left, top, right - left, bottom - top},
            {source_left, source_top, source_right - source_left, source_bottom - source_top}};
}

micropixel::Color FadeColor(micropixel::Color foreground, micropixel::Color background, uint32_t remaining,
                            uint32_t duration) {
    const uint32_t opacity = duration == 0U ? 0U : (remaining * 255U) / duration;
    return micropixel::Color::Mix(foreground, background, static_cast<uint8_t>(opacity > 255U ? 255U : opacity));
}

Line FormatLevelLabel(uint32_t level_index) {
    Line label;
    label.Append("LEVEL ");
    const uint32_t display_level = level_index + 1U;
    if (display_level < 100U) {
        label.Append("0");
    }
    if (display_level < 10U) {
        label.Append("0");
    }
    label.AppendUint(display_level);
    return label;
}

}  // namespace

micropixel::Rect TiltGame::FrameSource(ObjectFrame frame) const {
    const uint32_t index = static_cast<uint32_t>(frame);
    return {static_cast<int32_t>((index % kObjectSheetColumns) * object_frame_pixels_),
            static_cast<int32_t>((index / kObjectSheetColumns) * object_frame_pixels_),
            static_cast<int32_t>(object_frame_pixels_), static_cast<int32_t>(object_frame_pixels_)};
}

micropixel::Rect TiltGame::BoardTileFrameSource(BoardTileFrame frame) const {
    const uint32_t index = static_cast<uint32_t>(frame);
    return {static_cast<int32_t>((index % kBoardTileSheetColumns) * board_tile_frame_pixels_),
            static_cast<int32_t>((index / kBoardTileSheetColumns) * board_tile_frame_pixels_),
            static_cast<int32_t>(board_tile_frame_pixels_), static_cast<int32_t>(board_tile_frame_pixels_)};
}

micropixel::Rect TiltGame::HudFrameSource(uint32_t frame) const {
    return {static_cast<int32_t>(frame * hud_frame_pixels_), 0, static_cast<int32_t>(hud_frame_pixels_),
            static_cast<int32_t>(hud_frame_pixels_)};
}

micropixel::Rect TiltGame::FanFrameSource(uint32_t frame) const {
    return {static_cast<int32_t>(frame * fan_frame_pixels_), 0, static_cast<int32_t>(fan_frame_pixels_),
            static_cast<int32_t>(fan_frame_pixels_)};
}

micropixel::Rect TiltGame::MechanicFrameSource(MechanicFrame frame) const {
    const uint32_t index = static_cast<uint32_t>(frame);
    return {static_cast<int32_t>((index % kMechanicSheetColumns) * mechanic_frame_pixels_),
            static_cast<int32_t>((index / kMechanicSheetColumns) * mechanic_frame_pixels_),
            static_cast<int32_t>(mechanic_frame_pixels_), static_cast<int32_t>(mechanic_frame_pixels_)};
}

micropixel::Rect TiltGame::ObjectDestination(PointF center) {
    constexpr int32_t half = static_cast<int32_t>(kObjectFrameLogicalPixels / 2U);
    return ClampToViewport({kBoardX + RoundToInt(center.x) - half, kBoardY + RoundToInt(center.y) - half,
                            static_cast<int32_t>(kObjectFrameLogicalPixels),
                            static_cast<int32_t>(kObjectFrameLogicalPixels)});
}

micropixel::Rect TiltGame::BoardWallDestination(WallRect block) {
    return {kBoardX + block.x, kBoardY + block.y, block.width, block.height};
}

micropixel::Rect TiltGame::BoardIceDestination(RectFeature rect) {
    constexpr int32_t padding = 8;
    return {kBoardX + rect.x - padding, kBoardY + rect.y - padding, rect.width + padding * 2,
            rect.height + padding * 2};
}

micropixel::Rect TiltGame::BoardAirflowDestination(FanFeature fan) {
    return {kBoardX + fan.x - fan.radius, kBoardY + fan.y - fan.radius, fan.radius * 2, fan.radius * 2};
}

BoardTileFrame TiltGame::AirflowFrame(FanFeature fan) {
    const int32_t x = fan.force_x;
    const int32_t y = fan.force_y;
    const int32_t absolute_x = x < 0 ? -x : x;
    const int32_t absolute_y = y < 0 ? -y : y;
    if (absolute_x * 2 < absolute_y) {
        return y < 0 ? BoardTileFrame::kAirflowNorth : BoardTileFrame::kAirflowSouth;
    }
    if (absolute_y * 2 < absolute_x) {
        return x < 0 ? BoardTileFrame::kAirflowWest : BoardTileFrame::kAirflowEast;
    }
    if (x >= 0 && y < 0) {
        return BoardTileFrame::kAirflowNorthEast;
    }
    if (x >= 0 && y >= 0) {
        return BoardTileFrame::kAirflowSouthEast;
    }
    if (x < 0 && y >= 0) {
        return BoardTileFrame::kAirflowSouthWest;
    }
    return BoardTileFrame::kAirflowNorthWest;
}

micropixel::Rect TiltGame::MechanicDestination(WallRect rect) {
    constexpr int32_t half_frame = static_cast<int32_t>(kObjectFrameLogicalPixels / 2U);
    if (rect.width >= rect.height) {
        return {kBoardX + rect.x - 16, kBoardY + rect.y + rect.height / 2 - half_frame, rect.width + 32,
                static_cast<int32_t>(kObjectFrameLogicalPixels)};
    }
    return {kBoardX + rect.x + rect.width / 2 - half_frame, kBoardY + rect.y - 16,
            static_cast<int32_t>(kObjectFrameLogicalPixels), rect.height + 32};
}

void TiltGame::InitializeScene() {
    if (scene_initialized_) {
        return;
    }
    micropixel::Assert(renderer_info_.max_batch_instances() >= kBoardTileBatchCapacity + kObjectBatchCapacity +
                                                                   kMaximumFanCount + 2U + kTrailCapacity +
                                                                   kParticleCapacity + kStarCount,
                       "tilt: retained batch capacity unavailable");
    const micropixel::Point content_offset{
        renderer_info_.width() > kScreenWidth ? static_cast<int32_t>((renderer_info_.width() - kScreenWidth) / 2U) : 0,
        renderer_info_.height() > kScreenHeight ? static_cast<int32_t>((renderer_info_.height() - kScreenHeight) / 2U)
                                                : 0};
    root_container_ =
        scene_.CreateContainer({.clip = {0, 0, static_cast<int32_t>(kScreenWidth), static_cast<int32_t>(kScreenHeight)},
                                .translation = content_offset});
    game_container_ = root_container_.CreateContainer({.clip = kBoardRect, .z_order = 0});
    hud_container_ = root_container_.CreateContainer(
        {.clip = {0, 0, static_cast<int32_t>(kScreenWidth), kHudHeight}, .z_order = 10});
    modal_container_ = root_container_.CreateContainer(
        {.clip = {0, 0, static_cast<int32_t>(kScreenWidth), static_cast<int32_t>(kScreenHeight)}, .z_order = 20});

    board_base_node_ = game_container_.CreateSprite(
        board_base_texture_, kBoardRect,
        {0, 0, static_cast<int32_t>(board_base_texture_.width()), static_cast<int32_t>(board_base_texture_.height())});
    board_tile_batch_ = game_container_.CreateSpriteBatch(board_tile_texture_, kBoardTileBatchCapacity);
    board_frame_node_ = game_container_.CreateSprite(board_frame_texture_, kBoardRect,
                                                     {0, 0, static_cast<int32_t>(board_frame_texture_.width()),
                                                      static_cast<int32_t>(board_frame_texture_.height())});
    object_batch_ = game_container_.CreateSpriteBatch(object_texture_, kObjectBatchCapacity);
    fan_batch_ = game_container_.CreateSpriteBatch(fan_texture_, kMaximumFanCount);
    moving_wall_node_ = game_container_.CreateSprite(
        mechanic_texture_,
        {0, 0, static_cast<int32_t>(kObjectFrameLogicalPixels), static_cast<int32_t>(kObjectFrameLogicalPixels)},
        MechanicFrameSource(MechanicFrame::kBlockerHorizontal));
    gate_node_ = game_container_.CreateSprite(
        mechanic_texture_,
        {0, 0, static_cast<int32_t>(kObjectFrameLogicalPixels), static_cast<int32_t>(kObjectFrameLogicalPixels)},
        MechanicFrameSource(MechanicFrame::kGateHorizontal));
    pressure_gate_node_ = game_container_.CreateSprite(
        mechanic_texture_,
        {0, 0, static_cast<int32_t>(kObjectFrameLogicalPixels), static_cast<int32_t>(kObjectFrameLogicalPixels)},
        MechanicFrameSource(MechanicFrame::kGateHorizontal));
    pressure_plate_node_ = game_container_.CreateSprite(
        mechanic_texture_,
        {0, 0, static_cast<int32_t>(kObjectFrameLogicalPixels), static_cast<int32_t>(kObjectFrameLogicalPixels)},
        MechanicFrameSource(MechanicFrame::kPressurePlateOff));
    portal_batch_ = game_container_.CreateSpriteBatch(mechanic_texture_, 2U);
    trail_batch_ = game_container_.CreateSpriteBatch(kTrailCapacity);
    marble_node_ = game_container_.CreateSprite(object_texture_, ObjectDestination(model_.ball()),
                                                FrameSource(ObjectFrame::kMarble));
    goal_node_ = game_container_.CreateSprite(
        object_texture_,
        ObjectDestination({static_cast<float>(model_.level().goal.x), static_cast<float>(model_.level().goal.y)}),
        FrameSource(ObjectFrame::kGoal0));
    particle_batch_ = game_container_.CreateSpriteBatch(kParticleCapacity);

    const int32_t safe_left = static_cast<int32_t>(renderer_info_.safe_area_insets().left);
    title_node_ = hud_container_.CreateSprite(
        title_texture_, {safe_left + 20, 12, 240, 60},
        {0, 0, static_cast<int32_t>(title_texture_.width()), static_cast<int32_t>(title_texture_.height())});
    star_panel_ = hud_container_.CreateRoundedRect({312, 12, 174, 60}, {.fill = micropixel::Color::Rgb(8U, 18U, 28U),
                                                                        .stroke = micropixel::Color::Rgb(35U, 62U, 82U),
                                                                        .radius = 28,
                                                                        .stroke_width = 3});
    hud_star_batch_ = hud_container_.CreateSpriteBatch(hud_texture_, kStarCount);
    tilt_panel_ = hud_container_.CreateRoundedRect({626, 10, 76, 64}, {.fill = micropixel::Color::Rgb(8U, 18U, 28U),
                                                                       .stroke = micropixel::Color::Rgb(35U, 62U, 82U),
                                                                       .radius = 30,
                                                                       .stroke_width = 3});
    tilt_icon_node_ = hud_container_.CreateSprite(hud_texture_, {634, 12, 60, 60}, HudFrameSource(2U));
    level_label_ = hud_container_.CreateLabel({550, 9}, "LEVEL 001", micropixel::Color::White(),
                                              micropixel::SystemFont::kSmall, true);
    time_label_ = hud_container_.CreateLabel({550, 39}, "00:00", micropixel::Color::Rgb(45U, 226U, 230U),
                                             micropixel::SystemFont::kMedium, true);

    modal_dim_ = modal_container_.CreateShape(kBoardRect, micropixel::Color::Black(), 176U);
    modal_title_ = modal_container_.CreateLabel({360, 286}, "JUICY TILT", micropixel::Color::White(),
                                                micropixel::SystemFont::kLarge, true);
    modal_subtitle_ =
        modal_container_.CreateLabel({360, 650}, "TAP LOGO TO PAUSE", micropixel::Color::Rgb(130U, 158U, 178U),
                                     micropixel::SystemFont::kSmall, true);
    action_button_ = modal_container_.CreateTextButton({.bounds = kActionButtonRect,
                                                        .text = "START",
                                                        .style = {.background = micropixel::Color::Rgb(45U, 226U, 230U),
                                                                  .text = micropixel::Color::Rgb(5U, 18U, 23U),
                                                                  .feedback = micropixel::Color::White(),
                                                                  .font = micropixel::SystemFont::kLarge,
                                                                  .corner_radius = 30U},
                                                        .hit_padding = 8U});
    secondary_button_ =
        modal_container_.CreateTextButton({.bounds = kSecondaryButtonRect,
                                           .text = "RUN FROM 01",
                                           .style = {.background = micropixel::Color::Rgb(16U, 35U, 48U),
                                                     .text = micropixel::Color::Rgb(45U, 226U, 230U),
                                                     .feedback = micropixel::Color::White(),
                                                     .font = micropixel::SystemFont::kMedium,
                                                     .corner_radius = 24U},
                                           .hit_padding = 8U});
    previous_level_button_ =
        modal_container_.CreateTextButton({.bounds = kPreviousLevelButtonRect,
                                           .text = micropixel::symbols::kLeft,
                                           .style = {.background = micropixel::Color::Rgb(16U, 35U, 48U),
                                                     .text = micropixel::Color::White(),
                                                     .feedback = micropixel::Color::Rgb(45U, 226U, 230U),
                                                     .font = micropixel::SystemFont::kLarge,
                                                     .corner_radius = 28U},
                                           .hit_padding = 8U});
    next_level_button_ =
        modal_container_.CreateTextButton({.bounds = kNextLevelButtonRect,
                                           .text = micropixel::symbols::kRight,
                                           .style = {.background = micropixel::Color::Rgb(16U, 35U, 48U),
                                                     .text = micropixel::Color::White(),
                                                     .feedback = micropixel::Color::Rgb(45U, 226U, 230U),
                                                     .font = micropixel::SystemFont::kLarge,
                                                     .corner_radius = 28U},
                                           .hit_padding = 8U});
    scene_initialized_ = true;
}

void TiltGame::RenderObjects(micropixel::SceneUpdate& update) {
    if (rendered_level_index_ != model_.level_index()) {
        rendered_level_index_ = model_.level_index();
        const LevelData& level = model_.level();
        const bool ice_visible = level.ice.width > 0 && level.ice.height > 0;
        board_tile_batch_.SetInstance(update, kIceTileInstance,
                                      {.destination = BoardIceDestination(level.ice),
                                       .source = BoardTileFrameSource(BoardTileFrame::kIce),
                                       .color = micropixel::Color::White(),
                                       .opacity = 255U,
                                       .visible = ice_visible});
        for (uint16_t index = 0U; index < kMaximumFanCount; ++index) {
            const bool visible = index < level.fan_count;
            const FanFeature fan = visible ? level.fans[index] : FanFeature{};
            const SpritePlacement airflow =
                ClipSprite(BoardAirflowDestination(fan), BoardTileFrameSource(AirflowFrame(fan)), kBoardRect);
            board_tile_batch_.SetInstance(update, static_cast<uint16_t>(kAirflowTileInstanceOffset + index),
                                          {.destination = airflow.destination,
                                           .source = airflow.source,
                                           .color = micropixel::Color::White(),
                                           .opacity = 190U,
                                           .visible = visible});
        }
        uint16_t wall_block_index = 0U;
        for (uint32_t wall_index = 0U; wall_index < level.visual_wall_count; ++wall_index) {
            const VisualWallFeature wall = level.visual_walls[wall_index];
            const uint32_t block_count = WallBlockCount(wall);
            for (uint32_t block_index = 0U; block_index < block_count; ++block_index) {
                micropixel::Assert(wall_block_index < kMaximumVisualWallBlockCount,
                                   "tilt: level exceeds visual wall block capacity");
                const WallRect block = WallBlock(wall, block_index, block_count);
                board_tile_batch_.SetInstance(update, static_cast<uint16_t>(kWallTileInstanceOffset + wall_block_index),
                                              {.destination = BoardWallDestination(block),
                                               .source = BoardTileFrameSource(BoardTileFrame::kWallBlock),
                                               .color = micropixel::Color::White(),
                                               .opacity = 255U,
                                               .visible = true});
                ++wall_block_index;
            }
        }
        for (uint32_t joint_index = 0U; joint_index < level.wall_joint_count; ++joint_index) {
            micropixel::Assert(wall_block_index < kMaximumVisualWallBlockCount,
                               "tilt: level exceeds visual wall block capacity");
            const WallJointFeature joint = level.wall_joints[joint_index];
            const WallRect block{static_cast<int16_t>(joint.x - kWallBlockLength / 2),
                                 static_cast<int16_t>(joint.y - kWallBlockLength / 2), kWallBlockLength,
                                 kWallBlockLength};
            board_tile_batch_.SetInstance(update, static_cast<uint16_t>(kWallTileInstanceOffset + wall_block_index),
                                          {.destination = BoardWallDestination(block),
                                           .source = BoardTileFrameSource(BoardTileFrame::kWallBlock),
                                           .color = micropixel::Color::White(),
                                           .opacity = 255U,
                                           .visible = true});
            ++wall_block_index;
        }
        for (; wall_block_index < kMaximumVisualWallBlockCount; ++wall_block_index) {
            board_tile_batch_.SetInstance(update, static_cast<uint16_t>(kWallTileInstanceOffset + wall_block_index),
                                          {.destination = {},
                                           .source = BoardTileFrameSource(BoardTileFrame::kWallBlock),
                                           .color = micropixel::Color::White(),
                                           .opacity = 255U,
                                           .visible = false});
        }
    }
    const uint32_t animation_phase = static_cast<uint32_t>((animation_time_us_ / 150000U) % 4U);
    const ObjectFrame star_frame =
        static_cast<ObjectFrame>(static_cast<uint32_t>(ObjectFrame::kStar0) + animation_phase);
    for (uint32_t index = 0U; index < kStarCount; ++index) {
        object_batch_.SetInstance(update, static_cast<uint16_t>(kStarInstanceOffset + index),
                                  {.destination = ObjectDestination(model_.level().stars[index]),
                                   .source = FrameSource(star_frame),
                                   .color = micropixel::Color::White(),
                                   .opacity = 255U,
                                   .visible = !model_.star_collected(index)});
    }
    object_batch_.SetInstance(update, kPitInstance,
                              {.destination = ObjectDestination({static_cast<float>(model_.level().pit.x),
                                                                 static_cast<float>(model_.level().pit.y)}),
                               .source = FrameSource(ObjectFrame::kPit),
                               .color = micropixel::Color::White(),
                               .opacity = 255U,
                               .visible = model_.level().pit.radius > 0});
    ObjectFrame bumper_frame = ObjectFrame::kBumper0;
    if (bumper_flash_us_ != 0U) {
        bumper_frame = static_cast<ObjectFrame>(static_cast<uint32_t>(ObjectFrame::kBumper1) +
                                                static_cast<uint32_t>((animation_time_us_ / 60000U) % 2U));
    }
    object_batch_.SetInstance(update, kBumperInstance,
                              {.destination = ObjectDestination(model_.bumper_position()),
                               .source = FrameSource(bumper_frame),
                               .color = micropixel::Color::White(),
                               .opacity = 255U,
                               .visible = model_.level().bumper.radius > 0});
    for (uint16_t index = 0U; index < kMaximumFanCount; ++index) {
        const bool visible = index < model_.level().fan_count;
        const FanFeature fan = visible ? model_.level().fans[index] : FanFeature{};
        const bool active = visible && model_.fan_active(index);
        const uint32_t fan_frame =
            active ? static_cast<uint32_t>((animation_time_us_ / 35000U + index * 2U) % kFanFrameCount) : 0U;
        fan_batch_.SetInstance(
            update, index,
            {.destination = ObjectDestination({static_cast<float>(fan.x), static_cast<float>(fan.y)}),
             .source = FanFrameSource(fan_frame),
             .color = micropixel::Color::White(),
             .opacity = static_cast<uint8_t>(active ? 255U : 112U),
             .visible = visible});
    }
    const WallRect moving_wall = model_.moving_wall_rect();
    const bool moving_wall_visible = moving_wall.width != 0 && moving_wall.height != 0;
    moving_wall_node_.SetVisible(update, moving_wall_visible);
    if (moving_wall_visible) {
        moving_wall_node_.SetDestination(update, MechanicDestination(moving_wall));
        moving_wall_node_.SetSource(
            update, MechanicFrameSource(moving_wall.width >= moving_wall.height ? MechanicFrame::kBlockerHorizontal
                                                                                : MechanicFrame::kBlockerVertical));
    }
    const WallRect gate = model_.level().gate.rect;
    const bool gate_visible = gate.width != 0 && gate.height != 0;
    gate_node_.SetVisible(update, gate_visible);
    if (gate_visible) {
        gate_node_.SetDestination(update, MechanicDestination(gate));
        gate_node_.SetSource(update, MechanicFrameSource(gate.width >= gate.height ? MechanicFrame::kGateHorizontal
                                                                                   : MechanicFrame::kGateVertical));
        gate_node_.SetOpacity(update, model_.gate_open() ? 48U : 255U);
    }
    const PressureGateFeature pressure_gate = model_.level().pressure_gate;
    const bool pressure_visible = pressure_gate.plate.radius > 0;
    pressure_plate_node_.SetVisible(update, pressure_visible);
    pressure_gate_node_.SetVisible(update, pressure_gate.gate.width != 0 && pressure_gate.gate.height != 0);
    if (pressure_visible) {
        constexpr int32_t plate_size = 72;
        pressure_plate_node_.SetDestination(
            update, ClampToViewport({kBoardX + pressure_gate.plate.x - plate_size / 2,
                                     kBoardY + pressure_gate.plate.y - plate_size / 2, plate_size, plate_size}));
        pressure_plate_node_.SetSource(
            update, MechanicFrameSource(model_.pressure_gate_open() ? MechanicFrame::kPressurePlateOn
                                                                    : MechanicFrame::kPressurePlateOff));
        pressure_gate_node_.SetDestination(update, MechanicDestination(pressure_gate.gate));
        pressure_gate_node_.SetSource(update, MechanicFrameSource(pressure_gate.gate.width >= pressure_gate.gate.height
                                                                      ? MechanicFrame::kGateHorizontal
                                                                      : MechanicFrame::kGateVertical));
        pressure_gate_node_.SetOpacity(update, model_.pressure_gate_open() ? 38U : 255U);
    }
    const PortalPairFeature portals = model_.level().portals;
    constexpr int32_t portal_size = 62;
    const uint8_t portal_opacity[] = {214U, 236U, 255U, 236U};
    const CircleFeature portal_features[] = {portals.first, portals.second};
    for (uint16_t index = 0U; index < 2U; ++index) {
        const CircleFeature portal = portal_features[index];
        portal_batch_.SetInstance(
            update, index,
            {.destination = ClampToViewport({kBoardX + portal.x - portal_size / 2, kBoardY + portal.y - portal_size / 2,
                                             portal_size, portal_size}),
             .source = MechanicFrameSource(index == 0U ? MechanicFrame::kPortalB : MechanicFrame::kPortalA),
             .color = micropixel::Color::White(),
             .opacity = static_cast<uint8_t>(index == 0U ? portal_opacity[animation_phase] : 150U),
             .visible = portal.radius > 0});
    }
    goal_node_.SetSource(
        update, FrameSource(static_cast<ObjectFrame>(static_cast<uint32_t>(ObjectFrame::kGoal0) + animation_phase)));
    goal_node_.SetDestination(update, ObjectDestination({static_cast<float>(model_.level().goal.x),
                                                         static_cast<float>(model_.level().goal.y)}));
    goal_node_.SetOpacity(update, model_.goal_unlocked() ? 255U : 92U);
    marble_node_.SetDestination(update, ObjectDestination(model_.ball()));
}

void TiltGame::RenderEffects(micropixel::SceneUpdate& update) {
    constexpr micropixel::Color board_color = micropixel::Color::Rgb(7U, 17U, 26U);
    for (uint16_t index = 0U; index < kTrailCapacity; ++index) {
        const Trail& trail = trails_[index];
        if (!trail.active) {
            trail_batch_.SetInstanceVisible(update, index, false);
            continue;
        }
        const uint32_t remaining = 420000U - trail.age_us;
        const int32_t size = 7 + static_cast<int32_t>((remaining * 7U) / 420000U);
        const micropixel::Color color =
            FadeColor(micropixel::Color::Rgb(45U, 226U, 230U), board_color, remaining, 420000U);
        const micropixel::Rect destination =
            ClipToViewport({kBoardX + RoundToInt(trail.position.x) - size / 2,
                            kBoardY + RoundToInt(trail.position.y) - size / 2, size, size});
        trail_batch_.SetInstance(
            update, index,
            {.destination = destination, .color = color, .opacity = 255U, .visible = !destination.empty()});
    }
    for (uint16_t index = 0U; index < kParticleCapacity; ++index) {
        const Particle& particle = particles_[index];
        if (!particle.active) {
            particle_batch_.SetInstanceVisible(update, index, false);
            continue;
        }
        const uint32_t remaining = particle.duration_us - particle.age_us;
        const int32_t size = 4 + static_cast<int32_t>((remaining * 7U) / particle.duration_us);
        const micropixel::Color color =
            FadeColor(micropixel::Color::Rgb(255U, 190U, 45U), board_color, remaining, particle.duration_us);
        const micropixel::Rect destination =
            ClipToViewport({kBoardX + RoundToInt(particle.position.x) - size / 2,
                            kBoardY + RoundToInt(particle.position.y) - size / 2, size, size});
        particle_batch_.SetInstance(
            update, index,
            {.destination = destination, .color = color, .opacity = 255U, .visible = !destination.empty()});
    }
}

void TiltGame::FormatTime(uint64_t elapsed_us, char (&output)[6]) {
    uint32_t total_seconds = static_cast<uint32_t>(elapsed_us / 1000000U);
    total_seconds = total_seconds > 99U * 60U + 59U ? 99U * 60U + 59U : total_seconds;
    const uint32_t minutes = total_seconds / 60U;
    const uint32_t seconds = total_seconds % 60U;
    output[0] = static_cast<char>('0' + minutes / 10U);
    output[1] = static_cast<char>('0' + minutes % 10U);
    output[2] = ':';
    output[3] = static_cast<char>('0' + seconds / 10U);
    output[4] = static_cast<char>('0' + seconds % 10U);
    output[5] = '\0';
}

void TiltGame::RenderHud(micropixel::SceneUpdate& update) {
    const Vec2 tilt = input_.tilt();
    tilt_icon_node_.SetDestination(update, {634 + RoundToInt(tilt.x * 3.0F), 12 + RoundToInt(tilt.y * 3.0F), 60, 60});
    for (uint16_t index = 0U; index < kStarCount; ++index) {
        hud_star_batch_.SetInstance(update, index,
                                    {.destination = {321 + static_cast<int32_t>(index) * 54, 18, 48, 48},
                                     .source = HudFrameSource(index < model_.collected_stars() ? 0U : 1U),
                                     .color = micropixel::Color::White(),
                                     .opacity = 255U,
                                     .visible = true});
    }
    char time[6]{};
    FormatTime(model_.elapsed_us(), time);
    time_label_.SetText(update, time);
    const Line level = FormatLevelLabel(model_.level_index());
    level_label_.SetText(update, level.c_str());
}

void TiltGame::RenderModal(micropixel::SceneUpdate& update) {
    const bool visible = screen_ != Screen::kPlaying && screen_ != Screen::kCalibrating;
    modal_container_.SetVisible(update, visible);
    if (!visible) {
        return;
    }
    const bool menu = screen_ == Screen::kMenu;
    const bool has_previous_level = menu && selected_level_index_ != 0U;
    const bool has_next_level =
        menu && selected_level_index_ + 1U < kLevelCount && selected_level_index_ < progress_.unlocked_level_index;
    previous_level_button_.SetVisible(update, has_previous_level);
    next_level_button_.SetVisible(update, has_next_level);
    previous_level_button_.SetEnabled(update, has_previous_level);
    next_level_button_.SetEnabled(update, has_next_level);
    action_button_.SetVisible(update, true);
    if (screen_ == Screen::kMenu) {
        const Line selected_level = FormatLevelLabel(selected_level_index_);
        modal_title_.SetText(update, selected_level.c_str());
        modal_title_.SetColor(update, micropixel::Color::White());
        modal_subtitle_.SetPosition(update, {360, 650});
        modal_subtitle_.SetText(update, "TAP LOGO TO PAUSE");
        micropixel::Assert(action_button_.SetText(update, "START").has_value(), "tilt: START label failed");
        secondary_button_.SetVisible(update, selected_level_index_ != 0U);
        micropixel::Assert(secondary_button_.SetText(update, "RUN FROM 01").has_value(),
                           "tilt: run-from-first label failed");
    } else if (screen_ == Screen::kPaused) {
        modal_title_.SetText(update, "PAUSED");
        modal_title_.SetColor(update, micropixel::Color::White());
        modal_subtitle_.SetPosition(update, {360, 350});
        modal_subtitle_.SetText(update, "TIMER AND HAZARDS FROZEN");
        micropixel::Assert(action_button_.SetText(update, "RESUME").has_value(), "tilt: RESUME label failed");
        secondary_button_.SetVisible(update, true);
        micropixel::Assert(secondary_button_.SetText(update, "RESTART").has_value(), "tilt: RESTART label failed");
    } else if (screen_ == Screen::kComplete) {
        const bool final_level = model_.level_index() + 1U == kLevelCount;
        secondary_button_.SetVisible(update, final_level);
        modal_title_.SetText(update, final_level ? "ALL CLEAR" : "LEVEL CLEAR");
        modal_title_.SetColor(update, micropixel::Color::Rgb(255U, 176U, 32U));
        modal_subtitle_.SetPosition(update, {360, 350});
        char time[6]{};
        FormatTime(model_.elapsed_us(), time);
        Line summary;
        summary.Append("RANK ");
        summary.AppendUint(completed_rating_);
        summary.Append("/3   TIME ");
        summary.Append(time);
        modal_subtitle_.SetText(update, summary.c_str());
        micropixel::Assert(action_button_.SetText(update, final_level ? "RETRY" : "NEXT").has_value(),
                           "tilt: completion button label failed");
        if (final_level) {
            micropixel::Assert(secondary_button_.SetText(update, "RUN FROM 01").has_value(),
                               "tilt: final replay label failed");
        }
    } else {
        secondary_button_.SetVisible(update, false);
        modal_title_.SetText(update, "SENSOR REQUIRED");
        modal_title_.SetColor(update, micropixel::Color::Rgb(251U, 113U, 133U));
        modal_subtitle_.SetPosition(update, {360, 350});
        modal_subtitle_.SetText(update, "NO ACCELEROMETER FOUND");
        action_button_.SetVisible(update, false);
    }
    action_button_.Sync(update);
    secondary_button_.Sync(update);
    previous_level_button_.Sync(update);
    next_level_button_.Sync(update);
}

void TiltGame::Render() {
    InitializeScene();
    const auto presented = scene_.Update([&](micropixel::SceneUpdate& update) {
        RenderObjects(update);
        RenderEffects(update);
        RenderHud(update);
        RenderModal(update);
    });
    if (!presented.has_value()) {
        Line failure;
        failure.Append("tilt: scene update failed: ");
        failure.Append(presented.error().name());
        app_.log().Error(failure.c_str());
        micropixel::Panic("tilt: scene update failed");
    }
}

}  // namespace tilt
