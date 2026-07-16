#pragma once

#include "core/types.h"
#include "core/math.h"
#include "game/item.h"
#include "game/projectile.h"
#include "game/entity.h"
#include "world/level_grid.h"

struct NetPlayer; // forward-declared for updateMeteors net-slot heal routing (H4)

// Pending meteor (delayed AoE) — used by Meteor Strike + Paladin holy pillars
static constexpr u32 MAX_PENDING_METEORS = 8;
struct PendingMeteor {
    Vec3 position = {0,0,0};
    f32  damage   = 0.0f;
    f32  radius   = 0.0f;
    f32  timer    = 0.0f;
    bool active   = false;
    bool healsPlayer = false;  // holy pillar heals caster on impact
    u8   caster = 0;           // local-player index to credit on kill-heal (split-screen)
    Vec3 color = {1.0f, 0.5f, 0.1f}; // visual color (fire=orange, holy=gold)
};

struct Player;     // forward decl
struct ParticlePool;
struct ScreenShake;

namespace SkillSystem {
    void init();
    // Clear cross-floor gameplay state (pending meteors, overcharge buffs, bombardment/holy-nova
    // timers). Call from startGame so a skill warming up on the previous floor can't trigger
    // on the next one. Distinct from init() which also wires callbacks at engine startup.
    void resetGameplayState();

    // Clear per-slot timers + deactivate any in-flight effects credited to that slot.
    // Called from Engine::onPlayerLeft so a remote that disconnects mid-cast doesn't
    // leave its Holy Bombardment / Holy Nova / Overcharged Magazine ticking on the host
    // (continued meteor rain crediting the gone slot, or its rejoin successor).
    void resetSlotState(u8 slot);

    // Set skill power scaling (0.0 = base, 1.0 = max). Called by engine before
    // tryActivate — 0.0 for class skills, scaled by item level for legendary skills.
    void setSkillPower(f32 power);
    // Local-player index currently casting — credits per-player buffs (overcharge,
    // meteor kill-heal) to the right player in split-screen. Default 0 in singleplayer.
    void setCastingPlayer(u8 playerIndex);
    // Returns the most-recently-stamped casting player (net slot). Used by minion spawn callbacks
    // so a remote-cast drone tethers to the REMOTE caster, not the host (N4).
    u8   getCastingPlayer();

    // Set class skill damage multiplier (scales with effective floor).
    // Called by engine before class skill activation. Item skills use 1.0.
    void setClassDamageMult(f32 mult);

    // Set equipped weapon damage — Marksman/Ranger skills scale off weapon damage.
    void setWeaponDamage(f32 dmg);
    // Which projectile mesh weapon-scaling arrow skills (Barrage) should fire — arrow for
    // bows (and everything else), bolt for crossbows. Set alongside setWeaponDamage at both
    // cast paths (local + server remote-cast) from the caster's equipped weapon subtype.
    void setWeaponProjectileMesh(u8 meshId);

    // Set arrow/bolt mesh IDs for Ranger Volley (needs to assign meshId on spawned projectiles).
    void setArrowMeshIds(u8 arrow, u8 bolt);

    // Tick cooldowns, energy regen, pending meteors
    void update(SkillState& ss, f32 dt);

    // R17: compute the cooldown in ticks for a given def + cdr. Both client and
    // server must use this identical formula so the tick-based gate agrees by
    // construction. Floor of 3 ticks (~50ms) matches tryActivate's pre-R17 floor.
    u32 computeCooldownTicks(f32 defCooldown, f32 cooldownReduction);

    // Try to activate the player's current skill (returns true if activated).
    // R17: currentTick is the press's tick reference — m_clientTick on a CLIENT,
    // input->clientTick on the server-side remote path, m_serverTick on host/SP.
    // The gate is `currentTick - ss.lastActivationTick >= cooldownTicks`. On
    // success, ss.lastActivationTick is set to max(currentTick, 1) so the gate
    // sees this activation on the next press. ss.cooldownTimer is set to the
    // initial value for HUD; tickSkillCooldowns derives subsequent frames.
    bool tryActivate(SkillState& ss, const SkillDef* skillDefs, u32 skillDefCount,
                     Vec3 eyePos, Vec3 forward, f32 yaw,
                     ProjectilePool& projectiles, EntityPool& entities,
                     const LevelGrid& grid, Player& player,
                     u32 currentTick,
                     f32 cooldownReduction = 0.0f);

    // Update orb projectiles (spawn shards) -- called from projectile update or engine update
    void updateOrbProjectiles(ProjectilePool& pool, const SkillDef* skillDefs, u32 skillDefCount, f32 dt);

    // Update pending meteors (+ holy pillar healing). `netPlayers` (optional, MAX_PLAYERS-sized)
    // is the server's authoritative NetPlayer array; if non-null, a `m.caster >= playerCount`
    // heal routes to `netPlayers[caster]` so a remote Paladin's pillar heals the REMOTE caster
    // instead of the host. Pass nullptr in non-net modes; behavior degrades to the local-player
    // array as before.
    void updateMeteors(EntityPool& entities, Player** players, u8 playerCount, f32 dt,
                       NetPlayer* netPlayers = nullptr);

    // Get the SkillDef for a given SkillId (returns nullptr if not found)
    const SkillDef* findSkillDef(const SkillDef* defs, u32 count, SkillId id);

    // Visual FX callbacks — set by Engine to trigger skill effects
    using NovaCallback = void(*)(Vec3 position, f32 radius, Vec3 color);
    using DashCallback = void(*)(Vec3 start, Vec3 end);
    using ScorchCallback = void(*)(Vec3 position, f32 radius, f32 duration, f32 dps);
    using BeamCallback = void(*)(Vec3 start, Vec3 end, Vec3 color);
    // Drone spawn callback — engine handles entity creation with proper mesh/material
    // type: 0=combat drone (spider), 1=swarm drone (bat), 2=turret
    using DroneSpawnCallback = void(*)(Vec3 position, u8 type);
    // Chain lightning visual — receives array of bounce positions
    using ChainCallback = void(*)(const Vec3* points, u8 count);
    void setNovaCallback(NovaCallback cb);
    void setDashCallback(DashCallback cb);
    void setScorchCallback(ScorchCallback cb);
    void setBeamCallback(BeamCallback cb);

    // Instant reload callback — called by skills that grant reload on kill
    using ReloadCallback = void(*)();
    void setReloadCallback(ReloadCallback cb);

    // Overcharged Magazine — buff state for Marksman
    bool isOvercharged(u8 playerIndex = 0);
    void consumeOverchargeShot(u8 playerIndex = 0);
    void tickOvercharge(f32 dt, u8 playerIndex = 0);
    void setDroneSpawnCallback(DroneSpawnCallback cb);
    void setChainCallback(ChainCallback cb);
    void setBoltMeshId(u8 meshId, u8 matId);

    // Wire in the particle pool and screen shake for skill activation FX.
    void setFXTargets(ParticlePool* particles, ScreenShake* shake);
}
