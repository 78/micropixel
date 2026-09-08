#include "apps/maze-evil/game/world.hpp"

#include "apps/maze-evil/gfx/textures.hpp"
#include "apps/maze-evil/rc_math.hpp"

namespace maze_break::game {
namespace {

constexpr float kPi = math::kPi;
constexpr float kPlayerRadius = 0.28F;
constexpr float kImpRadius = 0.3F;
constexpr float kMoveSpeed = 3.2F;
constexpr float kStrafeSpeed = 2.6F;
constexpr float kImpSpeed = 1.5F;
constexpr float kImpHealth = 100;
constexpr int kShotDamage = 34;
constexpr float kShotCooldown = 0.55F;
constexpr float kFireballSpeed = 5.5F;
constexpr int kFireballDamage = 14;
constexpr int kMeleeDamage = 9;
constexpr float kDoorSpeed = 1.0F / 0.6F;
constexpr float kDoorPassable = 0.7F;
constexpr float kDoorHold = 2.5F;

float Length(float x, float y) { return math::Sqrt(x * x + y * y); }

}  // namespace

void World::Reset() {
    imp_count_ = 0;
    item_count_ = 0;
    decoration_count_ = 0;
    door_count_ = 0;
    kills_ = 0;
    phase_ = Phase::kPlaying;
    time_ = 0.0F;
    message_ = nullptr;
    message_timer_ = 0.0F;
    fire_was_down_ = true;  // The tap that restarted must not fire.
    pending_sound_count_ = 0;
    for (Fireball& fb : fireballs_) {
        fb = Fireball{};
    }
    player_ = Player{};
    player_.angle = 0.0F;

    for (int y = 0; y < kMapHeight; ++y) {
        for (int x = 0; x < kMapWidth; ++x) {
            const char symbol = kLevelRows[y][x];
            tiles_[y][x] = TileFromSymbol(symbol);
            const float cx = x + 0.5F;
            const float cy = y + 0.5F;
            switch (symbol) {
                case 'S':
                    player_.x = cx;
                    player_.y = cy;
                    break;
                case 'E':
                    if (imp_count_ < kMaxImps) {
                        imps_[imp_count_++] = Imp{.x = cx, .y = cy, .health = static_cast<int>(kImpHealth)};
                    }
                    break;
                case 'H':
                    if (item_count_ < kMaxItems) {
                        items_[item_count_++] = Item{.alive = true, .kind = Item::Kind::kMedkit, .x = cx, .y = cy};
                    }
                    break;
                case 'A':
                    if (item_count_ < kMaxItems) {
                        items_[item_count_++] = Item{.alive = true, .kind = Item::Kind::kAmmo, .x = cx, .y = cy};
                    }
                    break;
                case 'T':
                    if (decoration_count_ < kMaxDecorations) {
                        decorations_[decoration_count_++] =
                            Decoration{.sprite = gfx::kSprTorchA, .solid = false, .animated = true, .x = cx, .y = cy};
                    }
                    break;
                case 'B':
                    if (decoration_count_ < kMaxDecorations) {
                        decorations_[decoration_count_++] =
                            Decoration{.sprite = gfx::kSprBarrel, .solid = true, .animated = false, .x = cx, .y = cy};
                    }
                    break;
                case 'D':
                    if (door_count_ < kMaxDoors) {
                        doors_[door_count_++] = Door{.x = x, .y = y};
                    }
                    break;
                default:
                    break;
            }
        }
    }
    // Face into the room: the start room opens to the east.
    player_.angle = 0.0F;
    player_.dir_x = math::Cos(player_.angle);
    player_.dir_y = math::Sin(player_.angle);
    player_.plane_x = -player_.dir_y * 0.66F;
    player_.plane_y = player_.dir_x * 0.66F;
    for (int y = 0; y < kMapHeight; ++y) {
        for (int x = 0; x < kMapWidth; ++x) {
            const Tile tile = tiles_[y][x];
            cells_[y][x] = tile == Tile::kDoor ? micropixel::RaycastCell::Slab(gfx::kTexDoor, 0.0F)
                           : IsWall(tile)      ? micropixel::RaycastCell::Wall(static_cast<uint8_t>(WallTexture(tile)))
                                               : micropixel::RaycastCell::Empty();
        }
    }
    ShowMessage("FIND THE EXIT. KILL EVERY IMP.", 4.0F);
}

Tile World::TileAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= kMapWidth || y >= kMapHeight) {
        return Tile::kBrick;
    }
    return tiles_[y][x];
}

float World::DoorOpen(int x, int y) const {
    const Door* door = DoorAt(x, y);
    return door == nullptr ? 0.0F : door->open;
}

Door* World::DoorAt(int x, int y) {
    for (int i = 0; i < door_count_; ++i) {
        if (doors_[i].x == x && doors_[i].y == y) {
            return &doors_[i];
        }
    }
    return nullptr;
}

const Door* World::DoorAt(int x, int y) const {
    for (int i = 0; i < door_count_; ++i) {
        if (doors_[i].x == x && doors_[i].y == y) {
            return &doors_[i];
        }
    }
    return nullptr;
}

bool World::BlockedAt(int tile_x, int tile_y) const {
    const Tile tile = TileAt(tile_x, tile_y);
    if (tile == Tile::kDoor) {
        return DoorOpen(tile_x, tile_y) < kDoorPassable;
    }
    return IsWall(tile);
}

bool World::ActorBlocked(float x, float y, float radius) const {
    const int min_x = math::FloorInt(x - radius);
    const int max_x = math::FloorInt(x + radius);
    const int min_y = math::FloorInt(y - radius);
    const int max_y = math::FloorInt(y + radius);
    for (int ty = min_y; ty <= max_y; ++ty) {
        for (int tx = min_x; tx <= max_x; ++tx) {
            if (BlockedAt(tx, ty)) {
                return true;
            }
        }
    }
    for (int i = 0; i < decoration_count_; ++i) {
        const Decoration& d = decorations_[i];
        if (d.solid && Length(d.x - x, d.y - y) < radius + 0.3F) {
            return true;
        }
    }
    return false;
}

bool World::MoveActor(float& x, float& y, float dx, float dy, float radius) const {
    bool moved = false;
    if (!ActorBlocked(x + dx, y, radius)) {
        x += dx;
        moved = true;
    }
    if (!ActorBlocked(x, y + dy, radius)) {
        y += dy;
        moved = true;
    }
    return moved;
}

bool World::LineOfSight(float x0, float y0, float x1, float y1) const {
    // Grid DDA from (x0,y0) to (x1,y1); closed doors and walls block.
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    int map_x = math::FloorInt(x0);
    int map_y = math::FloorInt(y0);
    const int end_x = math::FloorInt(x1);
    const int end_y = math::FloorInt(y1);
    const float delta_x = dx == 0.0F ? 1e30F : math::Fabs(1.0F / dx);
    const float delta_y = dy == 0.0F ? 1e30F : math::Fabs(1.0F / dy);
    const int step_x = dx < 0.0F ? -1 : 1;
    const int step_y = dy < 0.0F ? -1 : 1;
    float side_x = dx < 0.0F ? (x0 - map_x) * delta_x : (map_x + 1.0F - x0) * delta_x;
    float side_y = dy < 0.0F ? (y0 - map_y) * delta_y : (map_y + 1.0F - y0) * delta_y;
    for (int guard = 0; guard < 128; ++guard) {
        if (map_x == end_x && map_y == end_y) {
            return true;
        }
        if (side_x < side_y) {
            side_x += delta_x;
            map_x += step_x;
        } else {
            side_y += delta_y;
            map_y += step_y;
        }
        const Tile tile = TileAt(map_x, map_y);
        if (IsWall(tile) || (tile == Tile::kDoor && DoorOpen(map_x, map_y) < 0.5F)) {
            return false;
        }
    }
    return false;
}

void World::ShowMessage(const char* text, float seconds) {
    message_ = text;
    message_timer_ = seconds;
}

void World::Emit(audio::SoundId id) {
    if (pending_sound_count_ < kMaxPendingSounds) {
        pending_sounds_[pending_sound_count_++] = audio::SoundEvent{id, 255U};
    }
}

void World::EmitAt(audio::SoundId id, float x, float y) {
    const float distance = Length(x - player_.x, y - player_.y);
    // Inverse-distance rolloff; inaudible beyond ~14 tiles.
    const float gain = distance > 14.0F ? 0.0F : 1.0F / (1.0F + distance * 0.35F);
    if (gain > 0.03F && pending_sound_count_ < kMaxPendingSounds) {
        pending_sounds_[pending_sound_count_++] = audio::SoundEvent{id, static_cast<uint8_t>(gain * 255.0F)};
    }
}

int World::TakeSounds(audio::SoundEvent* out, int capacity) {
    const int count = pending_sound_count_ < capacity ? pending_sound_count_ : capacity;
    for (int i = 0; i < count; ++i) {
        out[i] = pending_sounds_[i];
    }
    pending_sound_count_ = 0;
    return count;
}

void World::Update(float dt, const Controls& controls) {
    time_ += dt;
    if (message_timer_ > 0.0F) {
        message_timer_ -= dt;
    }
    if (phase_ != Phase::kPlaying) {
        // Any tap restarts once the previous press is released.
        if (controls.fire && !fire_was_down_) {
            Reset();
        }
        fire_was_down_ = controls.fire;
        return;
    }
    UpdatePlayer(dt, controls);
    UpdateImps(dt);
    UpdateFireballs(dt);
    UpdateDoors(dt);
    UpdateItems();
    CheckExit();
    for (int i = 0; i < decoration_count_; ++i) {
        if (decorations_[i].animated) {
            decorations_[i].sprite = gfx::kTorchFrames[static_cast<int>(time_ * 8.0F) & 3];
        }
    }
    fire_was_down_ = controls.fire;
}

void World::UpdatePlayer(float dt, const Controls& controls) {
    Player& p = player_;
    p.angle += controls.turn;
    if (p.angle > kPi) {
        p.angle -= 2.0F * kPi;
    } else if (p.angle < -kPi) {
        p.angle += 2.0F * kPi;
    }
    p.dir_x = math::Cos(p.angle);
    p.dir_y = math::Sin(p.angle);
    p.plane_x = -p.dir_y * 0.66F;
    p.plane_y = p.dir_x * 0.66F;

    const float forward = controls.forward * kMoveSpeed * dt;
    const float strafe = controls.strafe * kStrafeSpeed * dt;
    const float dx = p.dir_x * forward - p.dir_y * strafe;
    const float dy = p.dir_y * forward + p.dir_x * strafe;
    MoveActor(p.x, p.y, dx, dy, kPlayerRadius);
    p.speed = Length(dx, dy) / (dt > 0.0F ? dt : 1.0F);
    if (p.speed > 0.2F) {
        p.bob_phase += dt * 9.0F;
    }

    if (p.fire_cooldown > 0.0F) {
        p.fire_cooldown -= dt;
    }
    if (p.recoil > 0.0F) {
        p.recoil -= dt * 4.0F;
        if (p.recoil < 0.0F) {
            p.recoil = 0.0F;
        }
    }
    if (p.damage_flash > 0.0F) {
        p.damage_flash -= dt;
    }
    if (controls.fire && p.fire_cooldown <= 0.0F) {
        Shoot();
    }
}

void World::Shoot() {
    Player& p = player_;
    p.fire_cooldown = kShotCooldown;
    if (p.ammo <= 0) {
        ShowMessage("OUT OF SHELLS", 1.0F);
        Emit(audio::SoundId::kEmptyClick);
        return;
    }
    --p.ammo;
    p.recoil = 1.0F;
    Emit(audio::SoundId::kShotgun);

    // Hitscan: the closest imp inside a narrow cone with clear line of sight.
    Imp* target = nullptr;
    float best = 1e9F;
    for (int i = 0; i < imp_count_; ++i) {
        Imp& imp = imps_[i];
        if (imp.state == ImpState::kDead) {
            continue;
        }
        const float vx = imp.x - p.x;
        const float vy = imp.y - p.y;
        const float dist = Length(vx, vy);
        if (dist < 0.01F || dist > 14.0F) {
            continue;
        }
        const float along = (vx * p.dir_x + vy * p.dir_y) / dist;
        const float tolerance = math::Atan(kImpRadius / dist) + 0.03F;  // shotgun spread
        if (along < math::Cos(tolerance)) {
            continue;
        }
        if (dist < best && LineOfSight(p.x, p.y, imp.x, imp.y)) {
            best = dist;
            target = &imp;
        }
    }
    if (target == nullptr) {
        return;
    }
    // Damage falls off with range like a shotgun.
    const int damage = best < 3.0F ? kShotDamage + 16 : (best < 7.0F ? kShotDamage : kShotDamage - 10);
    target->health -= damage;
    if (target->health <= 0) {
        target->state = ImpState::kDead;
        target->timer = 0.0F;
        ++kills_;
        EmitAt(audio::SoundId::kImpDeath, target->x, target->y);
        if (kills_ == imp_count_) {
            ShowMessage("ALL IMPS DEAD. GET TO THE EXIT!", 3.0F);
        }
    } else {
        target->state = ImpState::kPain;
        target->timer = 0.35F;
        EmitAt(audio::SoundId::kImpPain, target->x, target->y);
    }
}

void World::DamagePlayer(int amount) {
    player_.health -= amount;
    player_.damage_flash = 0.35F;
    if (player_.health <= 0) {
        player_.health = 0;
        phase_ = Phase::kDead;
        ShowMessage("YOU DIED. TAP TO RETRY", 60.0F);
        Emit(audio::SoundId::kDie);
    } else {
        Emit(audio::SoundId::kPlayerPain);
    }
}

void World::SpawnFireball(float x, float y, float target_x, float target_y) {
    for (Fireball& fb : fireballs_) {
        if (fb.alive) {
            continue;
        }
        float vx = target_x - x;
        float vy = target_y - y;
        const float len = Length(vx, vy);
        if (len < 0.01F) {
            return;
        }
        // Slight inaccuracy so the player can dodge at range.
        const float wobble = (rng_.Unit() - 0.5F) * 0.12F;
        const float c = math::Cos(wobble);
        const float s = math::Sin(wobble);
        const float nx = (vx * c - vy * s) / len;
        const float ny = (vx * s + vy * c) / len;
        fb = Fireball{.alive = true,
                      .x = x + nx * 0.4F,
                      .y = y + ny * 0.4F,
                      .vx = nx * kFireballSpeed,
                      .vy = ny * kFireballSpeed,
                      .age = 0.0F};
        EmitAt(audio::SoundId::kImpFireball, x, y);
        return;
    }
}

void World::UpdateImps(float dt) {
    const Player& p = player_;
    for (int i = 0; i < imp_count_; ++i) {
        Imp& imp = imps_[i];
        if (imp.state == ImpState::kDead) {
            continue;
        }
        const float vx = p.x - imp.x;
        const float vy = p.y - imp.y;
        const float dist = Length(vx, vy);
        imp.attack_cooldown -= dt;

        switch (imp.state) {
            case ImpState::kIdle:
                if (dist < 11.0F && LineOfSight(imp.x, imp.y, p.x, p.y)) {
                    imp.state = ImpState::kChase;
                    EmitAt(audio::SoundId::kImpAlert, imp.x, imp.y);
                }
                break;
            case ImpState::kPain:
                imp.timer -= dt;
                if (imp.timer <= 0.0F) {
                    imp.state = ImpState::kChase;
                }
                break;
            case ImpState::kChase: {
                imp.anim += dt;
                const bool sees = LineOfSight(imp.x, imp.y, p.x, p.y);
                if (imp.attack_cooldown <= 0.0F && sees && dist < 8.0F) {
                    imp.state = ImpState::kAttack;
                    imp.timer = 0.0F;
                    imp.fired = false;
                    break;
                }
                if (dist > 1.1F) {
                    const float step = kImpSpeed * dt;
                    // Home in on the player; try the axis-aligned fallback when blocked.
                    if (!MoveActor(imp.x, imp.y, vx / dist * step, vy / dist * step, kImpRadius)) {
                        MoveActor(imp.x, imp.y, (vx > 0 ? step : -step), 0.0F, kImpRadius);
                    }
                    // Keep imps from stacking into each other.
                    for (int j = 0; j < imp_count_; ++j) {
                        if (j == i || imps_[j].state == ImpState::kDead) {
                            continue;
                        }
                        const float sx = imp.x - imps_[j].x;
                        const float sy = imp.y - imps_[j].y;
                        const float sd = Length(sx, sy);
                        if (sd > 0.001F && sd < 0.6F) {
                            MoveActor(imp.x, imp.y, sx / sd * step * 0.5F, sy / sd * step * 0.5F, kImpRadius);
                        }
                    }
                }
                break;
            }
            case ImpState::kAttack:
                imp.timer += dt;
                if (!imp.fired && imp.timer >= 0.3F) {
                    imp.fired = true;
                    if (dist < 1.4F) {
                        Emit(audio::SoundId::kImpMelee);
                        DamagePlayer(kMeleeDamage);
                    } else {
                        SpawnFireball(imp.x, imp.y, p.x, p.y);
                    }
                }
                if (imp.timer >= 0.65F) {
                    imp.state = ImpState::kChase;
                    imp.attack_cooldown = 1.4F + rng_.Unit() * 1.0F;
                }
                break;
            case ImpState::kDead:
                break;
        }
    }
}

void World::UpdateFireballs(float dt) {
    for (Fireball& fb : fireballs_) {
        if (!fb.alive) {
            continue;
        }
        fb.age += dt;
        fb.x += fb.vx * dt;
        fb.y += fb.vy * dt;
        const int tx = math::FloorInt(fb.x);
        const int ty = math::FloorInt(fb.y);
        if (fb.age > 4.0F || BlockedAt(tx, ty)) {
            fb.alive = false;
            if (fb.age <= 4.0F) {
                EmitAt(audio::SoundId::kFireballExplode, fb.x, fb.y);
            }
            continue;
        }
        if (Length(fb.x - player_.x, fb.y - player_.y) < 0.45F) {
            fb.alive = false;
            Emit(audio::SoundId::kFireballExplode);
            DamagePlayer(kFireballDamage);
        }
    }
}

void World::UpdateDoors(float dt) {
    for (int i = 0; i < door_count_; ++i) {
        Door& door = doors_[i];
        const float cx = door.x + 0.5F;
        const float cy = door.y + 0.5F;
        bool wants_open = Length(player_.x - cx, player_.y - cy) < 1.4F;
        bool occupied = Length(player_.x - cx, player_.y - cy) < 0.8F;
        for (int j = 0; j < imp_count_ && !wants_open; ++j) {
            const Imp& imp = imps_[j];
            if (imp.state == ImpState::kChase || imp.state == ImpState::kAttack) {
                const float d = Length(imp.x - cx, imp.y - cy);
                wants_open = d < 1.2F;
                occupied = occupied || d < 0.8F;
            }
        }
        switch (door.state) {
            case Door::State::kClosed:
                if (wants_open) {
                    door.state = Door::State::kOpening;
                    EmitAt(audio::SoundId::kDoorOpen, cx, cy);
                }
                break;
            case Door::State::kOpening:
                door.open += kDoorSpeed * dt;
                if (door.open >= 1.0F) {
                    door.open = 1.0F;
                    door.state = Door::State::kOpen;
                    door.timer = kDoorHold;
                }
                break;
            case Door::State::kOpen:
                if (wants_open) {
                    door.timer = kDoorHold;
                } else {
                    door.timer -= dt;
                    if (door.timer <= 0.0F && !occupied) {
                        door.state = Door::State::kClosing;
                        EmitAt(audio::SoundId::kDoorClose, cx, cy);
                    }
                }
                break;
            case Door::State::kClosing:
                if (wants_open || occupied) {
                    door.state = Door::State::kOpening;
                    break;
                }
                door.open -= kDoorSpeed * dt;
                if (door.open <= 0.0F) {
                    door.open = 0.0F;
                    door.state = Door::State::kClosed;
                }
                break;
        }
        cells_[door.y][door.x] = micropixel::RaycastCell::Slab(gfx::kTexDoor, door.open);
    }
}

void World::UpdateItems() {
    for (int i = 0; i < item_count_; ++i) {
        Item& item = items_[i];
        if (!item.alive || Length(item.x - player_.x, item.y - player_.y) > 0.6F) {
            continue;
        }
        if (item.kind == Item::Kind::kMedkit) {
            if (player_.health >= 100) {
                continue;
            }
            player_.health = player_.health + 25 > 100 ? 100 : player_.health + 25;
            ShowMessage("PICKED UP A MEDKIT", 1.2F);
            Emit(audio::SoundId::kPickupHealth);
        } else {
            if (player_.ammo >= 50) {
                continue;
            }
            player_.ammo = player_.ammo + 10 > 50 ? 50 : player_.ammo + 10;
            ShowMessage("PICKED UP SHELLS", 1.2F);
            Emit(audio::SoundId::kPickupAmmo);
        }
        item.alive = false;
    }
}

void World::CheckExit() {
    const int px = math::FloorInt(player_.x);
    const int py = math::FloorInt(player_.y);
    bool touching = false;
    for (int dy = -1; dy <= 1 && !touching; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (TileAt(px + dx, py + dy) == Tile::kExit) {
                // Require the player to actually press against the exit face.
                const float ex = px + dx + 0.5F;
                const float ey = py + dy + 0.5F;
                if (Length(ex - player_.x, ey - player_.y) < 0.85F) {
                    touching = true;
                    break;
                }
            }
        }
    }
    if (!touching) {
        return;
    }
    if (kills_ >= imp_count_) {
        phase_ = Phase::kWon;
        ShowMessage("AREA CLEARED! TAP TO PLAY AGAIN", 60.0F);
        Emit(audio::SoundId::kWin);
    } else if (message_timer_ <= 0.0F) {
        ShowMessage("THE EXIT IS SEALED. KILL EVERY IMP", 2.0F);
        Emit(audio::SoundId::kExitSealed);
    }
}

int World::CollectThings(Thing* out, int capacity) const {
    int count = 0;
    auto push = [&](float x, float y, gfx::SpriteId sprite, float height, float lift) {
        if (count < capacity) {
            out[count++] = Thing{x, y, sprite, height, lift};
        }
    };
    for (int i = 0; i < decoration_count_; ++i) {
        const Decoration& d = decorations_[i];
        if (d.sprite == gfx::kSprBarrel) {
            push(d.x, d.y, d.sprite, 0.55F, 0.0F);
        } else {
            push(d.x, d.y, d.sprite, 0.9F, 0.05F);
        }
    }
    for (int i = 0; i < item_count_; ++i) {
        if (items_[i].alive) {
            push(items_[i].x, items_[i].y, items_[i].kind == Item::Kind::kMedkit ? gfx::kSprMedkit : gfx::kSprAmmo,
                 0.3F, 0.0F);
        }
    }
    for (int i = 0; i < imp_count_; ++i) {
        const Imp& imp = imps_[i];
        gfx::SpriteId sprite = gfx::kSprImpWalkA;
        switch (imp.state) {
            case ImpState::kIdle:
                sprite = gfx::kSprImpWalkA;
                break;
            case ImpState::kChase:
                sprite = (static_cast<int>(imp.anim * 5.0F) & 1) ? gfx::kSprImpWalkB : gfx::kSprImpWalkA;
                break;
            case ImpState::kAttack:
                sprite = gfx::kSprImpAttack;
                break;
            case ImpState::kPain:
                sprite = gfx::kSprImpPain;
                break;
            case ImpState::kDead:
                sprite = gfx::kSprImpDead;
                break;
        }
        push(imp.x, imp.y, sprite, 0.85F, 0.0F);
    }
    for (const Fireball& fb : fireballs_) {
        if (fb.alive) {
            push(fb.x, fb.y, (static_cast<int>(fb.age * 12.0F) & 1) ? gfx::kSprFireballB : gfx::kSprFireballA, 0.3F,
                 0.35F);
        }
    }
    return count;
}

}  // namespace maze_break::game
