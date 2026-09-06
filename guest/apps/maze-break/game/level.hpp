#ifndef MICROPIXEL_APPS_MAZE_BREAK_GAME_LEVEL_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GAME_LEVEL_HPP

#include <stdint.h>

#include "apps/maze-break/gfx/textures.hpp"

namespace maze_break::game {

inline constexpr int kMapWidth = 32;
inline constexpr int kMapHeight = 32;

// Tile legend for the ASCII level:
//   '#' brick   '%' stone   '=' tech   '&' flesh   'W' wood
//   'D' door    'X' exit    '.' floor  'S' player start
//   'E' imp     'H' medkit  'A' ammo   'T' torch   'B' barrel (solid)
extern const char* const kLevelRows[kMapHeight];

enum class Tile : uint8_t {
    kFloor = 0,
    kBrick,
    kStone,
    kTech,
    kFlesh,
    kWood,
    kDoor,
    kExit,
};

[[nodiscard]] Tile TileFromSymbol(char symbol);
[[nodiscard]] bool IsWall(Tile tile);
[[nodiscard]] gfx::TextureId WallTexture(Tile tile);

}  // namespace maze_break::game

#endif
