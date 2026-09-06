#include "apps/maze-break/game/level.hpp"

namespace maze_break::game {

// clang-format off
const char* const kLevelRows[kMapHeight] = {
    "################################",
    "#......#.......%%%%%%%%%%%%%%%%#",
    "#.S....#.......%.......T......%#",
    "#......D.......%..............%#",
    "#......#.......%...B......B...%#",
    "#..T...#.......%..............%#",
    "########.......%.....E....E...%#",
    "#......#.......%..............%#",
    "#..A...#..E....%..H...........%#",
    "#......D.......%%%%D%%%%%%%%%%%#",
    "#......#...............#.......#",
    "########...............#.......#",
    "====================D==#..E.A..#",
    "=....=....=....=......=#.......#",
    "=.E..=..T.=..E.=......=#.......#",
    "=....D....D....D......=#...T...#",
    "=....=....=....=......=####.####",
    "==D=====D=====D=......=&&&&D&&&&",
    "=.....................=&.......&",
    "=..T......A.......T...=&..E....&",
    "=.....................D........&",
    "=..............H......=&.......&",
    "=...E......B....B.....=&&&&&&D&&",
    "==========D===========D........&",
    "WWWWWWWWWW.WWWWWWWWWWWW&.......&",
    "W.......W.............W&..E.E..&",
    "W..A.H..D......T......W&.......&",
    "W.......W.............W&..H....&",
    "W..T....W.....E..E....W&&&&D&&&&",
    "W.......W.............D.......X#",
    "W.......W......E......W.......X#",
    "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW",
};
// clang-format on

Tile TileFromSymbol(char symbol) {
    switch (symbol) {
        case '#':
            return Tile::kBrick;
        case '%':
            return Tile::kStone;
        case '=':
            return Tile::kTech;
        case '&':
            return Tile::kFlesh;
        case 'W':
            return Tile::kWood;
        case 'D':
            return Tile::kDoor;
        case 'X':
            return Tile::kExit;
        default:
            return Tile::kFloor;
    }
}

bool IsWall(Tile tile) { return tile != Tile::kFloor && tile != Tile::kDoor; }

gfx::TextureId WallTexture(Tile tile) {
    switch (tile) {
        case Tile::kBrick:
            return gfx::kTexBrick;
        case Tile::kStone:
            return gfx::kTexStone;
        case Tile::kTech:
            return gfx::kTexTech;
        case Tile::kFlesh:
            return gfx::kTexFlesh;
        case Tile::kWood:
            return gfx::kTexWood;
        case Tile::kDoor:
            return gfx::kTexDoor;
        case Tile::kExit:
            return gfx::kTexExit;
        default:
            return gfx::kTexBrick;
    }
}

}  // namespace maze_break::game
