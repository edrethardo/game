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
    // Driver-assigned STABLE identity for this hostile (the entity handle packed as
    // generation<<16 | index+1; 0 = unset). The target array is rebuilt and re-sorted every tick, so
    // an ARRAY INDEX is not an identity — this is what lets the driver recognise "the same enemy" a
    // tick later and keep the aim locked to it (target stickiness).
    u32  id = 0;
    Vec3 pos;         // AABB centre (aim point)
    Vec3 vel;         // for projectile lead
    f32  dist;        // to the bot's eye
    f32  hp;
    bool isBoss;
    bool hasLOS;      // width-aware LOS from the bot's eye already computed by the driver
    // --- threat timing (the gap-closer roll + the perfect-block tap) ---
    bool isRanged = false;       // Entity::attackRange > 5 — the same test the enemy AI itself uses
    f32  attackRange = 0.0f;     // this enemy's own reach, i.e. how close it must be to hit US
    // Seconds until this enemy's NEXT attack. Entity::attackTimer counts DOWN and the swing fires at
    // <= 0 (enemy_ai_states.cpp), so SMALL = imminent. Defaulted huge so a target the driver never
    // filled reads as "not about to swing" rather than as a permanent block trigger.
    f32  attackTimer = 1e9f;
    // FEET height of this hostile (entity centre minus its half-height), so the policy can compare
    // STORIES on the stacked layouts. Defaulted 0 so a hand-built view reads "same story as a bot
    // standing on the ground", which is the flat-floor case every existing test describes.
    f32  feetY = 0.0f;
    // ENT_FLYING. A flyer is exempt from the cross-story gate below: bats and drones hover 1.5-2.5 m
    // ABOVE their target by design (enemy_ai_states.cpp), which is most of a 3 m story, and a
    // hovering enemy is shootable from anywhere — it is not the unreachable ledge sniper the gate
    // exists to ignore.
    bool isFlying = false;
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
    f32  dodgeCooldown;   // 0 = the ENGINE's dodge is ready (1 s)
    // BOT-SIDE dodge leashes, owned by the driver. The engine's 1 s cooldown is a balance number,
    // not a behaviour one: a bot allowed to roll every 1 s reads as constant panicked twitching
    // ("make autoplay dodgeroll less often and less panicky"). The driver holds a multi-second
    // timer per roll KIND and reports whether the policy may even ask. Defaulted TRUE so a
    // hand-built view (tests) behaves like a bot with both leashes clear.
    bool dodgeAllowed    = true;   // the DEFENSIVE proactive roll (doctrine dodgeCooldownSec)
    bool gapCloseAllowed = true;   // the OFFENSIVE gap-closer charge (GAP_CLOSE_COOLDOWN)
    // Seconds the block has been HELD (Player::blockTimer, 0 when not blocking). The engine's
    // perfect-block tier expires at 0.2 s (Combat::classifyBlock), so the policy uses this to let a
    // stale hold GO and re-tap, which re-opens a fresh perfect window on the next raise edge.
    f32  blockHeld = 0.0f;
    bool potionReady;
    f32  weaponRange;     // effective weapon attackRange (melee small, ranged large)
    f32  weaponProjSpeed; // 0 for hitscan/melee (no lead)
    bool weaponIsMelee;
    u8   buildCell;
    // world
    bool  onNormalFloor;  // false in town/arena/source chamber => bot idles
    // STACKED layout (VERTICAL_HALL / FOUR_STORY): the floor has walk-on slab stories, so a hostile
    // 3 m above or below is on ANOTHER STORY — a different room the bot cannot walk to and which
    // is not on its route. Off by default so every flat floor (and every hand-built test view)
    // keeps the plain "anything with LOS is engageable" rule.
    bool  stackedFloor = false;
    // nav
    Vec3  flowDir;        // unit XZ toward exit, or {0,0,0}
    bool  flowValid;      // false when the flow byte is 0xFF (unreachable) — disambiguates zero
    bool  atExit;         // flow byte 0xFE
    // descend gate (filled by the driver; consumed by the brain in the next task)
    bool  doorActive;
    f32   distToDoor;
    bool  hasBoss;
    bool  bossAlive;
    // class skills — availability MIRRORED from the real activation gates by the driver (slot has a
    // skill at all / unlocked at the EFFECTIVE floor / energy affordable / off cooldown), so
    // castableSkill[i] means "pressing SKILL_i+1 + CLASS_SKILL right now actually casts". The policy
    // must never press a slot that would no-op: a wasted press reads as a bot that ignores its build.
    bool castableSkill[4] = {};
    // EQUIPMENT legendary skills (boots = F, helmet = G), same contract as castableSkill: the driver
    // mirrors handleEquipmentSkillActivation's real gates (the slot is bound to a skill at all,
    // the shared energy pool covers the cost, the tick cooldown has elapsed) so a true here means
    // the press really casts. The helmet is additionally stun-gated and the boots deliberately are
    // NOT — Break Free is the escape FROM a stun.
    bool bootCastable   = false;
    bool helmetCastable = false;
    // Sim tick, for the DETERMINISTIC cadences below (strafe side flips, the kiting jump). rand()
    // would desync a replay and make a live bug unreproducible; a tick counter is reproducible and
    // free. Defaulted 0, which puts a hand-built test view at the start of every cadence.
    u32  tick = 0;
    // Seconds until the soonest HOSTILE projectile currently closing on the bot would reach it
    // (1e9 = nothing inbound). A ranged enemy's own attackTimer only says when the shot LEAVES, so
    // this is the only thing that can time a shield raise into the perfect window against an archer.
    f32  incomingProjectileEta = 1e9f;
    // targets (nearest-first, driver-capped)
    const BotTarget* targets;
    u32   targetCount;
    // TARGET STICKINESS (see Autoplay::pickTarget). The driver remembers the hostile the bot is
    // already fighting by BotTarget::id and resolves it back to a slot here each tick (-1 = none /
    // it died / it left the capped set); targetSwitchAllowed is its TARGET_MIN_DWELL timer. Both
    // defaulted to "no memory, switching free" so a hand-built view (tests) gets plain nearest-LOS.
    s32   currentTargetIdx    = -1;
    bool  targetSwitchAllowed = true;
    // globes/pickups the driver found in reach (low-hp detour goals), nearest first
    const Vec3* globes;
    u32   globeCount;
};

// One tick of decision. Semantic — the driver maps these onto GameActions.
struct BotIntent {
    f32  aimYaw = 0.0f, aimPitch = 0.0f;      // desired absolute aim (driver writes to player)
    bool moveFwd = false, moveBack = false, moveLeft = false, moveRight = false;  // WASD (rides yaw + forward)
    bool jump = false, fire = false, block = false, dodge = false;
    // Which leash the driver charges when `dodge` fired: the DEFENSIVE proactive roll (false) and
    // the OFFENSIVE gap-closer charge (true) are deliberately rate-limited apart — a bot that just
    // ate its defensive roll must still be able to charge, and vice versa.
    bool dodgeIsGapClose = false;
    bool potion = false, reload = false, descend = false, interact = false;
    s8   classSkillSlot = -1;                 // 0..3 => select SKILL_n + press CLASS_SKILL; -1 none
    bool bootSkill = false, helmetSkill = false;
};

} // namespace Autoplay
