#ifndef MICROPIXEL_APPS_MAZE_BREAK_GAME_WORLD_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GAME_WORLD_HPP

#include <stdint.h>

#include "apps/maze-evil/audio/sound_ids.hpp"
#include "apps/maze-evil/game/level.hpp"
#include "apps/maze-evil/gfx/sprites.hpp"
#include "apps/maze-evil/rc_math.hpp"
#include "sdk/raycast.hpp"

namespace maze_break::game {

struct Controls {
    float forward{};  // -1..1
    float strafe{};   // -1..1, positive = right
    float turn{};     // radians for this frame, positive = clockwise
    bool fire{};
};

struct Player {
    float x{}, y{};
    float angle{};
    float dir_x{}, dir_y{};
    float plane_x{}, plane_y{};
    int health{100};
    int ammo{24};
    float fire_cooldown{};
    float recoil{};        // 0..1, decays after a shot
    float bob_phase{};     // walking bob
    float damage_flash{};  // seconds of red tint left
    float speed{};         // current planar speed for bobbing
};

enum class ImpState : uint8_t { kIdle, kChase, kAttack, kPain, kDead };

struct Imp {
    float x{}, y{};
    int health{};
    ImpState state{ImpState::kIdle};
    float timer{};
    float anim{};
    float attack_cooldown{};
    bool fired{};
};

struct Fireball {
    bool alive{};
    float x{}, y{};
    float vx{}, vy{};
    float age{};
};

struct Item {
    enum class Kind : uint8_t { kMedkit, kAmmo };
    bool alive{};
    Kind kind{};
    float x{}, y{};
};

struct Decoration {
    gfx::SpriteId sprite{};
    bool solid{};
    bool animated{};
    float x{}, y{};
};

struct Door {
    enum class State : uint8_t { kClosed, kOpening, kOpen, kClosing };
    int x{}, y{};
    float open{};  // 0 closed .. 1 fully open (slab raised into the ceiling)
    State state{State::kClosed};
    float timer{};
};

// One renderable billboard.
struct Thing {
    float x{}, y{};
    gfx::SpriteId sprite{};
    float height{};  // fraction of wall height
    float lift{};    // fraction of wall height above the floor
};

enum class Phase : uint8_t { kPlaying, kDead, kWon };

class World {
   public:
    static constexpr int kMaxImps = 24;
    static constexpr int kMaxFireballs = 16;
    static constexpr int kMaxItems = 16;
    static constexpr int kMaxDecorations = 24;
    static constexpr int kMaxDoors = 24;
    static constexpr int kMaxThings = kMaxImps + kMaxFireballs + kMaxItems + kMaxDecorations;

    void Reset();
    void Update(float dt, const Controls& controls);
    // Deterministic AI for --benchmark: reseeds the wobble/cooldown RNG.
    void SeedRng(uint32_t seed) { rng_ = math::Rng{seed}; }

    const Player& player() const { return player_; }
    Phase phase() const { return phase_; }
    int kills() const { return kills_; }
    int total_imps() const { return imp_count_; }
    const char* message() const { return message_timer_ > 0.0F ? message_ : nullptr; }
    bool muzzle_flash() const { return player_.recoil > 0.75F; }

    Tile TileAt(int x, int y) const;
    // 0 for non-door tiles; otherwise how far the slab has risen.
    float DoorOpen(int x, int y) const;
    // The level as Raycaster cells: walls carry their texture slot, doors are
    // slabs whose `open` follows Door::open. Rewritten by Reset() and
    // UpdateDoors(); valid until the next Reset().
    micropixel::RaycastGrid grid() const { return {&cells_[0][0], kMapWidth, kMapHeight}; }
    // Fills renderable billboards; returns the count.
    int CollectThings(Thing* out, int capacity) const;
    // Drains sound events raised since the last call; returns the count. At
    // most kMaxPendingSounds are queued between calls; the caller's array
    // should hold that many so none are dropped.
    static constexpr int kMaxPendingSounds = 16;
    int TakeSounds(audio::SoundEvent* out, int capacity);

   private:
    void UpdatePlayer(float dt, const Controls& controls);
    void UpdateImps(float dt);
    void UpdateFireballs(float dt);
    void UpdateDoors(float dt);
    void UpdateItems();
    void CheckExit();
    void Shoot();
    void DamagePlayer(int amount);
    void ShowMessage(const char* text, float seconds);
    // Sound at the player's position (full gain) or attenuated by distance.
    void Emit(audio::SoundId id);
    void EmitAt(audio::SoundId id, float x, float y);

    bool BlockedAt(int tile_x, int tile_y) const;
    bool ActorBlocked(float x, float y, float radius) const;
    bool MoveActor(float& x, float& y, float dx, float dy, float radius) const;
    bool LineOfSight(float x0, float y0, float x1, float y1) const;
    Door* DoorAt(int x, int y);
    const Door* DoorAt(int x, int y) const;
    void SpawnFireball(float x, float y, float target_x, float target_y);

    Tile tiles_[kMapHeight][kMapWidth]{};
    micropixel::RaycastCell cells_[kMapHeight][kMapWidth]{};
    Player player_{};
    Imp imps_[kMaxImps]{};
    int imp_count_{};
    Fireball fireballs_[kMaxFireballs]{};
    Item items_[kMaxItems]{};
    int item_count_{};
    Decoration decorations_[kMaxDecorations]{};
    int decoration_count_{};
    Door doors_[kMaxDoors]{};
    int door_count_{};
    int kills_{};
    Phase phase_{Phase::kPlaying};
    float time_{};
    const char* message_{};
    float message_timer_{};
    bool fire_was_down_{};
    math::Rng rng_{};
    audio::SoundEvent pending_sounds_[kMaxPendingSounds]{};
    int pending_sound_count_{};
};

}  // namespace maze_break::game

#endif
