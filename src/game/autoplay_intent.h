// autoplay_intent.h — the interface between the Autoplay brain (pure) and the engine driver.
//
// BotView is a read-only snapshot the driver fills from live engine state each tick; BotIntent is
// the semantic output the driver translates into synthetic GameActions + a yaw/pitch write. Keeping
// them plain structs (no engine types beyond math) is what lets the brain unit-test without the Engine.
#pragma once
#include "core/types.h"
#include "core/math.h"

namespace Autoplay {

// One hostile the driver has already resolved from the entity pool (dead/friendly/prop/burrowed
// pre-filtered, matching CombatQuery). Positions are world-space; vel is XZ for lead.
struct BotTarget {
    Vec3 pos;         // AABB centre (aim point)
    Vec3 vel;         // for projectile lead
    f32  dist;        // to the bot's eye
    f32  hp;
    bool isBoss;
    bool hasLOS;      // width-aware LOS from the bot's eye already computed by the driver
};

// Effective ENGAGEMENT range for the doctrine band, from a weapon's authored range + projectile
// speed. Melee/hitscan weapons author a real range (4 m sword, 50 m pistol) and use it verbatim.
// PROJECTILE weapons author NONE — items.json gives them a projectile SPEED instead (the shot flies
// until it hits or its 3 s spawn lifetime expires; the tooltip prints "Proj Speed", not "Range"), so
// ItemDef::baseRange is 0 for every wand/bow/staff/crossbow in the game. Feeding that 0 into the
// doctrine collapses the whole band to zero and the bot NEVER fires from the FIGHT branch — the
// second half of the "sorcerers stuck on floor 1" bug (the first was gating fire on engageMin).
// So derive it from the projectile's real reach and CAP it: an uncapped 29 m/s bolt reaches 86 m,
// which would demand a 47 m kite floor inside a 15 m room. The cap is twice the brain's 12 m
// THREAT_RADIUS — past that the bot doesn't consider a target worth engaging anyway.
inline f32 botWeaponRange(f32 defRange, f32 projSpeed) {
    if (defRange > 0.01f) return defRange;
    constexpr f32 kProjLifetime = 3.0f;    // Combat::fireProjectile's spawn lifetime
    constexpr f32 kMaxEngage    = 24.0f;   // 2 x THREAT_RADIUS (autoplay_brain.cpp)
    const f32 reach = projSpeed * kProjLifetime;
    if (reach > kMaxEngage) return kMaxEngage;
    // A weapon with neither an authored range nor a projectile speed is a data hole; fall back to
    // the threat radius rather than 0, because a 0 here silently MUTES the bot (this whole comment
    // is that bug) and a bot that shoots at nothing is far cheaper than one that never shoots.
    if (reach < 1.0f) return 12.0f;
    return reach;
}

struct BotView {
    // self
    Vec3 pos;             // feet
    f32  yaw, pitch;      // current aim
    f32  eyeHeight;
    f32  hp, maxHp;
    f32  energy, maxEnergy;
    bool stunned, rolling, onGround;
    f32  dodgeCooldown;   // 0 = dodge ready
    bool potionReady;
    f32  weaponRange;     // effective weapon attackRange (melee small, ranged large)
    f32  weaponProjSpeed; // 0 for hitscan/melee (no lead)
    bool weaponIsMelee;
    u8   buildCell;
    // world
    bool  onNormalFloor;  // false in town/arena/source chamber => bot idles
    // nav
    Vec3  flowDir;        // unit XZ toward exit, or {0,0,0}
    bool  flowValid;      // false when the flow byte is 0xFF (unreachable) — disambiguates zero
    bool  atExit;         // flow byte 0xFE
    // descend gate (filled by the driver; consumed by the brain in the next task)
    bool  doorActive;
    f32   distToDoor;
    bool  hasBoss;
    bool  bossAlive;
    // targets (nearest-first, driver-capped)
    const BotTarget* targets;
    u32   targetCount;
    // globes/pickups the driver found in reach (low-hp detour goals), nearest first
    const Vec3* globes;
    u32   globeCount;
};

// One tick of decision. Semantic — the driver maps these onto GameActions.
struct BotIntent {
    f32  aimYaw = 0.0f, aimPitch = 0.0f;      // desired absolute aim (driver writes to player)
    bool moveFwd = false, moveBack = false, moveLeft = false, moveRight = false;  // WASD (rides yaw + forward)
    bool jump = false, fire = false, block = false, dodge = false;
    bool potion = false, reload = false, descend = false, interact = false;
    s8   classSkillSlot = -1;                 // 0..3 => select SKILL_n + press CLASS_SKILL; -1 none
    bool bootSkill = false, helmetSkill = false;
};

} // namespace Autoplay
