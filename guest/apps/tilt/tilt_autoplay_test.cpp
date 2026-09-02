#include <cassert>
#include <cmath>
#include <cstdio>

#include "apps/tilt/tilt_model.hpp"

namespace {

constexpr uint32_t kGrid = 6U;
constexpr uint32_t kMaximumRouteCells = kGrid * kGrid * 4U;
constexpr float kCell = 90.0F;
constexpr float kOrigin = 30.0F;

struct Cell final {
    int row{};
    int column{};
};

struct Scenario final {
    uint64_t phase_offset_us{};
    float bias_x{};
    float bias_y{};
    float noise{};
    float response_scale{1.0F};
};

constexpr Scenario kCandidateScenarios[] = {
    {},
    {550000U, 0.025F, -0.020F, 0.015F, 0.96F},
    {1250000U, -0.030F, 0.025F, 0.020F, 0.92F},
};

float Center(int coordinate) { return kOrigin + static_cast<float>(coordinate) * kCell + kCell * 0.5F; }

bool Same(Cell first, Cell second) { return first.row == second.row && first.column == second.column; }

float Closest(float value, float minimum, float maximum) {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

bool StaticBlocked(const tilt::LevelData& level, float x, float y) {
    for (uint32_t index = 0U; index < level.wall_count; ++index) {
        const tilt::WallRect wall = level.walls[index];
        const float closest_x = Closest(x, static_cast<float>(wall.x), static_cast<float>(wall.x + wall.width));
        const float closest_y = Closest(y, static_cast<float>(wall.y), static_cast<float>(wall.y + wall.height));
        const float dx = x - closest_x;
        const float dy = y - closest_y;
        if (dx * dx + dy * dy < tilt::kBallRadius * tilt::kBallRadius) {
            return true;
        }
    }
    const float pit_dx = x - static_cast<float>(level.pit.x);
    const float pit_dy = y - static_cast<float>(level.pit.y);
    const float pit_radius = static_cast<float>(level.pit.radius) - tilt::kBallRadius * 0.35F + 3.0F;
    return pit_dx * pit_dx + pit_dy * pit_dy < pit_radius * pit_radius;
}

bool EdgePassable(const tilt::LevelData& level, Cell first, Cell second) {
    const float first_x = Center(first.column);
    const float first_y = Center(first.row);
    const float second_x = Center(second.column);
    const float second_y = Center(second.row);
    for (uint32_t sample = 0U; sample <= 30U; ++sample) {
        const float progress = static_cast<float>(sample) / 30.0F;
        if (StaticBlocked(level, first_x + (second_x - first_x) * progress,
                          first_y + (second_y - first_y) * progress)) {
            return false;
        }
    }
    return true;
}

Cell CellFor(tilt::PointF point) {
    return {static_cast<int>((point.y - kOrigin) / kCell), static_cast<int>((point.x - kOrigin) / kCell)};
}

uint32_t FindRoute(const tilt::LevelData& level, Cell start, Cell goal, Cell* route) {
    Cell queue[kGrid * kGrid]{};
    Cell previous[kGrid][kGrid]{};
    bool visited[kGrid][kGrid]{};
    uint32_t head = 0U;
    uint32_t tail = 0U;
    queue[tail++] = start;
    visited[start.row][start.column] = true;
    previous[start.row][start.column] = {-1, -1};
    constexpr int directions[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    while (head < tail) {
        const Cell current = queue[head++];
        if (Same(current, goal)) {
            break;
        }
        for (const auto& direction : directions) {
            const Cell next{current.row + direction[0], current.column + direction[1]};
            if (next.row < 0 || next.row >= static_cast<int>(kGrid) || next.column < 0 ||
                next.column >= static_cast<int>(kGrid) || visited[next.row][next.column] ||
                !EdgePassable(level, current, next)) {
                continue;
            }
            visited[next.row][next.column] = true;
            previous[next.row][next.column] = current;
            queue[tail++] = next;
        }
    }
    if (!visited[goal.row][goal.column]) {
        return 0U;
    }
    Cell reversed[kGrid * kGrid]{};
    uint32_t count = 0U;
    for (Cell cursor = goal; cursor.row >= 0; cursor = previous[cursor.row][cursor.column]) {
        reversed[count++] = cursor;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        route[index] = reversed[count - index - 1U];
    }
    return count;
}

bool AutoClear(uint32_t level_index, bool mastery, uint32_t& elapsed_ms, uint32_t& falls, uint32_t& wall_hits,
               Scenario scenario = {}, bool require_mastery_rating = true) {
    tilt::TiltModel model;
    model.Reset(level_index);
#ifdef MICROPIXEL_MODEL_TESTING
    model.SetDynamicPhaseOffsetForTesting(scenario.phase_offset_us);
#endif
    tilt::PointF route[kMaximumRouteCells]{};
    const tilt::PointF* authored_route = model.level().solution;
    uint32_t route_count = model.level().solution_count;
    if (route_count != 0U) {
        assert(route_count <= kMaximumRouteCells);
        for (uint32_t index = 0U; index < route_count; ++index) {
            route[index] = authored_route[index];
        }
    } else {
        Cell cell_route[kGrid * kGrid]{};
        const tilt::PointF goal_point{static_cast<float>(model.level().goal.x),
                                      static_cast<float>(model.level().goal.y)};
        route_count = FindRoute(model.level(), CellFor(model.level().start), CellFor(goal_point), cell_route);
        for (uint32_t index = 0U; index < route_count; ++index) {
            route[index] = {Center(cell_route[index].column), Center(cell_route[index].row)};
        }
    }
    if (route_count == 0U) {
        return false;
    }
    uint32_t waypoint = 1U;
    constexpr uint32_t maximum_frames = 60U * 180U;
    for (uint32_t frame = 0U; frame < maximum_frames; ++frame) {
        if (waypoint >= route_count) {
            waypoint = route_count - 1U;
        }
        const tilt::PointF target = route[waypoint];
        const tilt::PointF ball = model.ball();
        const tilt::Vec2 velocity = model.velocity();
        const float dx = target.x - ball.x;
        const float dy = target.y - ball.y;
        const float speed_squared = velocity.x * velocity.x + velocity.y * velocity.y;
        const tilt::CircleFeature portal = model.level().portals.first;
        const float target_portal_dx = target.x - static_cast<float>(portal.x);
        const float target_portal_dy = target.y - static_cast<float>(portal.y);
        const bool precise_portal_control =
            portal.radius > 0 &&
            target_portal_dx * target_portal_dx + target_portal_dy * target_portal_dy < 80.0F * 80.0F;
        const float arrival_radius = precise_portal_control ? 5.0F : 18.0F;
        const float arrival_speed = precise_portal_control ? 24.0F : 45.0F;
        if (dx * dx + dy * dy < arrival_radius * arrival_radius && speed_squared < arrival_speed * arrival_speed &&
            waypoint + 1U < route_count) {
            ++waypoint;
        }
        const float noise_x = __builtin_sinf(static_cast<float>(frame) * 0.37F) * scenario.noise;
        const float noise_y = __builtin_cosf(static_cast<float>(frame) * 0.29F) * scenario.noise;
        const tilt::Vec2 command{
            tilt::ClampFloat((dx * 0.030F - velocity.x * 0.008F) * scenario.response_scale + scenario.bias_x + noise_x,
                             -1.0F, 1.0F),
            tilt::ClampFloat((dy * 0.030F - velocity.y * 0.008F) * scenario.response_scale + scenario.bias_y + noise_y,
                             -1.0F, 1.0F),
        };
        const tilt::ModelOutcome outcome = model.Advance(16667U, command);
        wall_hits += outcome.wall_hit ? 1U : 0U;
        if (outcome.teleported) {
            std::printf("  hit portal trap waypoint=%u/%u destination=(%.1f,%.1f)\n", waypoint, route_count,
                        model.ball().x, model.ball().y);
            return false;
        }
        if (outcome.fell) {
            ++falls;
            if (mastery) {
                return false;
            }
            waypoint = 1U;
            if (falls > 5U) {
                return false;
            }
        }
        if (outcome.completed) {
            elapsed_ms = static_cast<uint32_t>(model.elapsed_us() / 1000U);
            return !mastery || (model.collected_stars() == tilt::kStarCount &&
                                (!require_mastery_rating || model.mastery_rating() == 3U));
        }
    }
    elapsed_ms = static_cast<uint32_t>(model.elapsed_us() / 1000U);
    std::printf(
        "  stuck waypoint=%u/%u ball=(%.1f,%.1f) velocity=(%.1f,%.1f) target=(%.1f,%.1f) "
        "stars=%u gate=%u\n",
        waypoint, route_count, model.ball().x, model.ball().y, model.velocity().x, model.velocity().y,
        route[waypoint].x, route[waypoint].y, model.collected_stars(), model.pressure_gate_open() ? 1U : 0U);
    return false;
}

}  // namespace

int main() {
#ifdef MICROPIXEL_CANDIDATE_EVALUATOR
    for (uint32_t level = 0U; level < tilt::kLevelCount; ++level) {
        for (uint32_t scenario_index = 0U;
             scenario_index < sizeof(kCandidateScenarios) / sizeof(kCandidateScenarios[0]); ++scenario_index) {
            uint32_t elapsed_ms = 0U;
            uint32_t falls = 0U;
            uint32_t wall_hits = 0U;
            const bool cleared =
                AutoClear(level, true, elapsed_ms, falls, wall_hits, kCandidateScenarios[scenario_index], false);
            std::printf("CANDIDATE %u scenario=%u clear=%u time=%u falls=%u walls=%u\n", level, scenario_index,
                        cleared ? 1U : 0U, elapsed_ms, falls, wall_hits);
        }
    }
    return 0;
#else
    for (uint32_t level = 0U; level < tilt::kLevelCount; ++level) {
        if (tilt::kLevels[level].solution_count == 0U) {
            continue;
        }
        uint32_t elapsed_ms = 0U;
        uint32_t falls = 0U;
        uint32_t wall_hits = 0U;
        const bool cleared = AutoClear(level, false, elapsed_ms, falls, wall_hits);
        std::printf("level %u autoplay: %s time=%ums falls=%u\n", level + 1U, cleared ? "clear" : "FAILED", elapsed_ms,
                    falls);
        assert(cleared);
        elapsed_ms = 0U;
        falls = 0U;
        wall_hits = 0U;
        const bool mastered = AutoClear(level, true, elapsed_ms, falls, wall_hits);
        std::printf("level %u mastery: %s time=%ums falls=%u\n", level + 1U, mastered ? "3/3" : "FAILED", elapsed_ms,
                    falls);
        assert(mastered);
    }
    return 0;
#endif
}
