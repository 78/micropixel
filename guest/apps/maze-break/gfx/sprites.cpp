#include "apps/maze-break/gfx/sprites.hpp"

#include "apps/maze-break/gfx/palette.hpp"

namespace maze_break::gfx {
namespace {

// Character legend shared by every sprite. Lower case is the darker tone of a
// hue, upper case the brighter one.
struct LegendEntry {
    char symbol;
    Ramp ramp;
    int level;
};

constexpr LegendEntry kLegend[] = {
    {'k', kGray, 1},   {'d', kGray, 3},     {'g', kGray, 6},      {'G', kGray, 9},    {'l', kGray, 12},
    {'w', kWhite, 15}, {'a', kWarmGray, 6}, {'A', kWarmGray, 10}, {'r', kRed, 5},     {'R', kRed, 9},
    {'X', kRed, 13},   {'o', kOrange, 8},   {'O', kOrange, 12},   {'y', kYellow, 10}, {'Y', kYellow, 14},
    {'b', kBrown, 3},  {'B', kBrown, 8},    {'n', kBrown, 11},    {'h', kSkin, 5},    {'p', kSkin, 9},
    {'P', kSkin, 13},  {'e', kGreen, 6},    {'E', kGreen, 11},    {'c', kCyan, 8},    {'C', kCyan, 13},
    {'u', kBlue, 6},   {'U', kBlue, 10},    {'v', kPurple, 5},    {'V', kPurple, 9},  {'s', kSteel, 6},
    {'S', kSteel, 10}, {'m', kGold, 8},     {'M', kGold, 12},     {'t', kTeal, 7},    {'T', kTeal, 11},
};

uint8_t Decode(char symbol) {
    if (symbol == '.') {
        return kTransparent;
    }
    for (const LegendEntry& entry : kLegend) {
        if (entry.symbol == symbol) {
            return Index(entry.ramp, entry.level);
        }
    }
    return Index(kPurple, 15);  // Loud colour for typos in the art.
}

// clang-format off
const char* const kImpWalkA[32] = {
    "................................",
    "..........b..........b..........",
    ".........bb..........bb.........",
    ".........bbb........bbb.........",
    "..........bbbBBBBBBbbb..........",
    ".........bBBBBBBBBBBBBb.........",
    ".........BBBBnBBBBnBBBB.........",
    ".........BByyBBBBBByyBB.........",
    ".........BBYyBBBBBBYyBB.........",
    ".........BBBBBBBBBBBBBB.........",
    "..........BBBkkkkkkBBB..........",
    "..........BBkwkwkwkkBB..........",
    "...........BBBBBBBBBB...........",
    ".......bB..BBBBBBBBBB..Bb.......",
    "......bBBBBBBBBBBBBBBBBBBb......",
    ".....bBBBBBBBBBBBBBBBBBBBBb.....",
    ".....BBB.BBBBnBBBBBBBBB.BBB.....",
    "....BBB..BBBBnBBBBBBBBB..BBB....",
    "....BBB..BBBBnBBBBBBBBB..BBB....",
    "....BBb..BBBBBBBBBBBBBB..bBB....",
    "...bBBb..BBBBBBBBBBBBBB..bBBb...",
    "...bkkb...BBBBBBBBBBBB...bkkb...",
    "...kkk.....BBBBBBBBBB.....kkk...",
    "..k.k.k....BBBBBBBBBB....k.k.k..",
    "...........BBBB..BBBB...........",
    "..........BBBB....BBBB..........",
    "..........BBBB....BBBB..........",
    ".........BBBB......BBBB.........",
    ".........BBBB......BBBB.........",
    "........BBBB........BBBB........",
    "........kkkk........kkkk........",
    "................................",
};

const char* const kImpWalkB[32] = {
    "................................",
    "..........b..........b..........",
    ".........bb..........bb.........",
    ".........bbb........bbb.........",
    "..........bbbBBBBBBbbb..........",
    ".........bBBBBBBBBBBBBb.........",
    ".........BBBBnBBBBnBBBB.........",
    ".........BByyBBBBBByyBB.........",
    ".........BBYyBBBBBBYyBB.........",
    ".........BBBBBBBBBBBBBB.........",
    "..........BBBkkkkkkBBB..........",
    "..........BBkwkwkwkkBB..........",
    "...........BBBBBBBBBB...........",
    ".......bB..BBBBBBBBBB..Bb.......",
    "......bBBBBBBBBBBBBBBBBBBb......",
    ".....bBBBBBBBBBBBBBBBBBBBBb.....",
    "....BBB..BBBBnBBBBBBBBB..BBB....",
    "...BBB...BBBBnBBBBBBBBB...BBB...",
    "...BBB...BBBBnBBBBBBBBB...BBB...",
    "...BBb...BBBBBBBBBBBBBB...bBB...",
    "..bBBb...BBBBBBBBBBBBBB...bBBb..",
    "..bkkb....BBBBBBBBBBBB....bkkb..",
    "..kkk......BBBBBBBBBB......kkk..",
    ".k.k.k.....BBBBBBBBBB.....k.k.k.",
    "...........BBBB..BBBB...........",
    "..........BBBB....BBBB..........",
    ".........BBBB.....BBBB..........",
    "........BBBB......BBBB..........",
    ".......BBBB.......BBBB..........",
    "......BBBB.........BBBB.........",
    "......kkkk.........kkkk.........",
    "................................",
};

const char* const kImpAttack[32] = {
    "........................yy......",
    ".......................yOOy.....",
    "..........b..........b.yOYYOy...",
    ".........bb..........bbyOYwYOy..",
    ".........bbb........bbbyOYYOy...",
    "..........bbbBBBBBBbbb.byOOy....",
    ".........bBBBBBBBBBBBBb.byy.....",
    ".........BBBBnBBBBnBBBB..BB.....",
    ".........BByyBBBBBByyBB..BB.....",
    ".........BBYyBBBBBBYyBB..BB.....",
    ".........BBBBBBBBBBBBBB..BB.....",
    "..........BBBkkkkkkBBB...BB.....",
    "..........BBkwkwkwkkBB...BB.....",
    "...........BBBBBBBBBB...BBB.....",
    ".......bB..BBBBBBBBBBBBBBB......",
    "......bBBBBBBBBBBBBBBBBBB.......",
    ".....bBBBBBBBBBBBBBBBBBB........",
    ".....BBB.BBBBnBBBBBBBBB.........",
    "....BBB..BBBBnBBBBBBBBB.........",
    "....BBB..BBBBnBBBBBBBBB.........",
    "....BBb..BBBBBBBBBBBBBB.........",
    "...bBBb..BBBBBBBBBBBBBB.........",
    "...bkkb...BBBBBBBBBBBB..........",
    "...kkk.....BBBBBBBBBB...........",
    "..k.k.k....BBBBBBBBBB...........",
    "...........BBBB..BBBB...........",
    "..........BBBB....BBBB..........",
    "..........BBBB....BBBB..........",
    ".........BBBB......BBBB.........",
    ".........BBBB......BBBB.........",
    "........BBBB........BBBB........",
    "........kkkk........kkkk........",
};

const char* const kImpDead[32] = {
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    ".............b..b...............",
    "............bbBBbb....b.........",
    "..........bBBBBBBBb..bB.........",
    ".......bbBBBkwkBBBBbbBBb........",
    "....bbBBBBBBBBBBBBBBBBBBbb......",
    "...bBBBBBBBBBBBBBBBBBBBBBBb.....",
    "..bBBBBBBBBBBBBBBBBBBBBBBBBb....",
    ".bBBBrrRRrrBBBBBBBBBBBBBBBBBb...",
    ".bbbrrRRRRrrbbbbbbbbbbbbbbbbbb..",
    "..rrrrRRRRrrrr..........rr......",
    "................................",
};

const char* const kFireballA[16] = {
    "................",
    ".....yyy........",
    "....yOOOy.......",
    "...yOOYYOy......",
    "..yOOYYYYOy.....",
    "..yOYYwwYYOy....",
    "..yOYYwwYYOy....",
    "..yOOYYYYOy.....",
    "...yOOYYOy...r..",
    "....yOOOy...rr..",
    ".....yyy...rRr..",
    "..........rRRr..",
    ".........rRRRr..",
    ".........rRRr...",
    "..........rr....",
    "................",
};

const char* const kFireballB[16] = {
    "................",
    "....yyyy........",
    "...yOOOOy.......",
    "..yOOYYOOy......",
    ".yOOYYYYOOy.....",
    ".yOYYwwwYYOy....",
    ".yOYYwwwYYOy....",
    ".yOOYYYYOOy.....",
    "..yOOYYOOy......",
    "...yOOOOy....r..",
    "....yyyy....rr..",
    "...........rRr..",
    "..........rRRr..",
    "..........rRr...",
    "...........r....",
    "................",
};

const char* const kMedkit[16] = {
    "................",
    "................",
    "................",
    "..gggggggggggg..",
    ".gwwwwwwwwwwwwg.",
    ".gwwwwwRRwwwwwg.",
    ".gwwwwwRRwwwwwg.",
    ".gwwwRRRRRRwwwg.",
    ".gwwwRRRRRRwwwg.",
    ".gwwwwwRRwwwwwg.",
    ".gwwwwwRRwwwwwg.",
    ".gwwwwwwwwwwwwg.",
    ".gllllllllllllg.",
    "..gggggggggggg..",
    "................",
    "................",
};

const char* const kAmmo[16] = {
    "................",
    "................",
    "................",
    "...eeeeeeeeee...",
    "..eEEEEEEEEEEe..",
    "..eEyyEyyEyyEe..",
    "..eEyOEyOEyOEe..",
    "..eEyyEyyEyyEe..",
    "..eEEEEEEEEEEe..",
    "..eeeeeeeeeeee..",
    "..eEEEEEEEEEEe..",
    "..eEEEEEEEEEEe..",
    "..eeeeeeeeeeee..",
    "................",
    "................",
    "................",
};

const char* const kTorchA[32] = {
    "................",
    "................",
    "......y.........",
    ".....yOy........",
    ".....yOOy.......",
    "....yOYYOy......",
    "....yOYwYOy.....",
    "....yOYwYOy.....",
    "...yOOYYYOOy....",
    "...yOOYYYOOy....",
    "....yOOYOOy.....",
    ".....yOOOy......",
    "......yyy.......",
    "......ddd.......",
    ".....dGGGd......",
    ".....dGgGd......",
    "......dgd.......",
    "......dgd.......",
    "......dgd.......",
    "......dgd.......",
    "......dgd.......",
    ".....ddgdd......",
    "....dGGgGGd.....",
    "....dGGgGGd.....",
    ".....ddgdd......",
    "......dgd.......",
    "......dgd.......",
    "......ddd.......",
    "................",
    "................",
    "................",
    "................",
};

const char* const kTorchB[32] = {
    "................",
    "................",
    "................",
    ".......y........",
    "......yOy.......",
    ".....yOOOy......",
    "....yOYYYOy.....",
    "....yOYwwYOy....",
    "...yOOYwYOOy....",
    "...yOOYYYOOy....",
    "....yOOYOOy.....",
    ".....yOOOy......",
    "......yyy.......",
    "......ddd.......",
    ".....dGGGd......",
    ".....dGgGd......",
    "......dgd.......",
    "......dgd.......",
    "......dgd.......",
    "......dgd.......",
    "......dgd.......",
    ".....ddgdd......",
    "....dGGgGGd.....",
    "....dGGgGGd.....",
    ".....ddgdd......",
    "......dgd.......",
    "......dgd.......",
    "......ddd.......",
    "................",
    "................",
    "................",
    "................",
};

const char* const kBarrel[24] = {
    "....gggggggg....",
    "..ggeeeeeeeegg..",
    ".geeEEEEEEEEeeg.",
    ".gEEEEEEEEEEEEg.",
    ".gEEEEEEEEEEEEg.",
    ".geeeeeeeeeeeeg.",
    ".gEEEEEEEEEEEEg.",
    ".gEEEEEEEEEEEEg.",
    ".gEEEEEEEEEEEEg.",
    ".gEEEEEEEEEEEEg.",
    ".geeeeeeeeeeeeg.",
    ".gEEEEEEEEEEEEg.",
    ".gEEEEEEEEEEEEg.",
    ".gEEEEEEEEEEEEg.",
    ".gEEEEEEEEEEEEg.",
    ".geeeeeeeeeeeeg.",
    ".gEEEEEEEEEEEEg.",
    ".gEEEEEEEEEEEEg.",
    ".gEEEEEEEEEEEEg.",
    ".geeeeeeeeeeeeg.",
    ".gEEEEEEEEEEEEg.",
    ".geeeeeeeeeeeeg.",
    "..ggeeeeeeeegg..",
    "....gggggggg....",
};

// First-person pump shotgun seen from behind: barrel, wooden pump with the
// left hand, steel receiver, stock and the trigger hand. Rendered 2x.
const char* const kShotgun[48] = {
    "..........................dgddkkkkddgd..........................",
    "..........................dgddkkkkddgd..........................",
    "..........................dgddddddddgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dddddddddddd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dddddddddddd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "..........................dgGGlllGGdgd..........................",
    "......................bbbbbbbbbbbbbbbbbbbb......................",
    "......................bBBBBBBBBBBBBBBBBBBb......................",
    "......................bBnnnnnnnnnnnnnnnnBb......................",
    "............hhhhhhhhhhhhhhBBBBBBBBBBBBBBBb......................",
    "............hppppppppppphpphppphppphppphBb......................",
    "............hpPPPPPPPPPPppphppphppphppphBb......................",
    "............hpPPPPPPPPPPppphppphppphppphBb......................",
    "............hpppppppppppppphppphppphppphBb......................",
    "............hppppppppppphpphppphppphppphBb......................",
    "............hpppppppppppppphppphppphppphBb......................",
    "............hpppppppppppppphppphppphppphBb......................",
    "............hpppppppdddddddddddddddddddddddd....................",
    "............hhhhhhhhdggggggggggggggggggggggd....................",
    "....................dgGGGGGGGGGGGGGGGGGGGGgd....................",
    "....................dgGGGGGGGGGGGGGGkkkkkGgd....................",
    "....................dgggggggggggggggkkkkkggd....................",
    "....................dgggggggggggggggkkkkkghhhhhhhhhhhh..........",
    "....................dggggggggggggggggggggghpppppppppph..........",
    "....................dgddddddddddddddddddddhpPPPPPPPPph..........",
    "....................dggggggggggggggggggggghpPPPPPPPPph..........",
    "..................bBBBBBBBBBBBBBBBBBBBBBBBhpppppppppph..........",
    ".................bBBBBBBBBBBBBBBBBBBBBBBBBhpppppppppph..........",
    "................bBBBBBBBBBBBBBBBBBBBBBBBBBhpppppppppph..........",
    "...............bBBBBnnnnnnnnnnnnnnnnnnnnnnhpppppppppph..........",
    "..............bBBBBBBBBBBBBBBBBBBBBBBBBBBBhphhhhhhhhph..........",
    ".............bBBBBBBBBBBBBBBBBBBBBBBBBBBBBhpppppppppph..........",
    "............bBBBBBBBBBBBBBBBBBBBBBBBBBBBBBhpppppppppph..........",
    "...........bBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBhhhhhhhhhhhh..........",
};

const char* const kMuzzleFlash[20] = {
    "..........yy....yy..........",
    ".........yOy....yOy.........",
    "........yOOy.yy.yOOy........",
    "........yOOyyOOyyOOy........",
    ".....y..yOOYYOOYYOOy..y.....",
    "....yOy.yOYYYwwYYYOy.yOy....",
    "....yOOyyOYYwwwwYYOyyOOy....",
    ".....yOOOYYYwwwwYYYOOOy.....",
    "......yOOYYYwwwwYYYOOy......",
    "..yyyyOOOYYYwwwwYYYOOOyyyy..",
    ".yOOOOOOYYYYwwwwYYYYOOOOOOy.",
    ".yOOOOOOYYYYwwwwYYYYOOOOOOy.",
    "..yyyyOOOYYYwwwwYYYOOOyyyy..",
    "......yOOYYYwwwwYYYOOy......",
    ".....yOOOYYYwwwwYYYOOOy.....",
    "....yOOyyOYYwwwwYYOyyOOy....",
    "....yOy.yOYYYwwYYYOy.yOy....",
    ".....y..yOOYYOOYYOOy..y.....",
    "........yOOyyOOyyOOy........",
    ".........yy..yy..yy.........",
};
// clang-format on

struct SpriteSource {
    const char* const* rows;
    int width;
    int height;
};

constexpr SpriteSource kSources[kSprCount] = {
    {kImpWalkA, 32, 32}, {kImpWalkB, 32, 32},  {kImpAttack, 32, 32}, {kImpWalkA, 32, 32},  // pain = recoloured A
    {kImpDead, 32, 32},  {kFireballA, 16, 16}, {kFireballB, 16, 16}, {kMedkit, 16, 16},   {kAmmo, 16, 16},
    {kTorchA, 16, 32},   {kTorchB, 16, 32},    {kBarrel, 16, 24},    {kShotgun, 64, 48},  {kMuzzleFlash, 28, 20},
};

Sprite gSprites[kSprCount];

// All decoded sprite pixels live in one static pool; the sum of the source
// sizes is known at compile time.
constexpr int PoolBytes() {
    int total = 0;
    for (const SpriteSource& source : kSources) {
        total += source.width * source.height;
    }
    return total;
}

uint8_t gSpritePool[PoolBytes()];

}  // namespace

void BuildSprites() {
    int offset = 0;
    for (int id = 0; id < kSprCount; ++id) {
        const SpriteSource& source = kSources[id];
        uint8_t* pixels = gSpritePool + offset;
        offset += source.width * source.height;
        for (int y = 0; y < source.height; ++y) {
            const char* row = source.rows[y];
            for (int x = 0; x < source.width; ++x) {
                char symbol = row[x];
                if (id == kSprImpPain && symbol != '.') {
                    // Pain flash: brighten the body and turn the eyes red.
                    if (symbol == 'B') {
                        symbol = 'n';
                    } else if (symbol == 'b') {
                        symbol = 'B';
                    } else if (symbol == 'y' || symbol == 'Y') {
                        symbol = 'X';
                    }
                }
                pixels[y * source.width + x] = Decode(symbol);
            }
        }
        gSprites[id] = {source.width, source.height, pixels};
    }
}

const Sprite& SpriteFor(SpriteId id) { return gSprites[id < kSprCount ? id : kSprImpWalkA]; }

}  // namespace maze_break::gfx
